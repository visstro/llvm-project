//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RawOwningPointerMemberCheck.h"
#include "../utils/OptionsUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>

using namespace clang::ast_matchers;

namespace clang::tidy::bugprone {

namespace {

/// How the destructor releases a member.
struct Release {
  /// The 'delete', deallocator call or destructor call that releases it.
  const Stmt *Site = nullptr;
  /// True if the member itself is released, false if only its elements are
  /// (a pointer array or a container of pointers).
  bool Direct = false;
  /// Members tested by an 'if' around the release, e.g. 'if (Owns) delete P;'.
  llvm::SmallPtrSet<const FieldDecl *, 2> Guards;
};

using ReleaseMap = llvm::MapVector<const FieldDecl *, Release>;

} // namespace

/// Returns the member of \p RD named by \p E if \p E is 'this->F' (explicit or
/// implicit 'this').
static const FieldDecl *getThisMember(const Expr *E, const CXXRecordDecl *RD) {
  const auto *ME = dyn_cast<MemberExpr>(E->IgnoreParenCasts());
  if (!ME || !isa<CXXThisExpr>(ME->getBase()->IgnoreParenImpCasts()))
    return nullptr;
  const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!FD || FD->getParent()->getCanonicalDecl() != RD->getCanonicalDecl())
    return nullptr;
  return FD;
}

static const FieldDecl *getThisPointerMember(const Expr *E,
                                             const CXXRecordDecl *RD) {
  const FieldDecl *FD = getThisMember(E, RD);
  return FD && FD->getType()->isPointerType() ? FD : nullptr;
}

/// Returns \p E as a member expression if it is 'Param.F'.
static const MemberExpr *getParamMember(const Expr *E, const ParmVarDecl *Param,
                                        const FieldDecl *F) {
  // The initializer 'F(Other.F)' in a template is a 'ParenListExpr'.
  if (const auto *PLE = dyn_cast<ParenListExpr>(E);
      PLE && PLE->getNumExprs() == 1)
    E = PLE->getExpr(0);
  const auto *ME = dyn_cast<MemberExpr>(E->IgnoreParenCasts());
  if (!ME || ME->getMemberDecl() != F)
    return nullptr;
  const auto *DRE = dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreParenImpCasts());
  return DRE && DRE->getDecl() == Param ? ME : nullptr;
}

/// Returns true if \p E is 'this' or '*this'.
static bool isThisObject(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(E);
      UO && UO->getOpcode() == UO_Deref)
    E = UO->getSubExpr()->IgnoreParenImpCasts();
  return isa<CXXThisExpr>(E);
}

static bool refersTo(const Expr *E, const ValueDecl *D) {
  const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
  return DRE && DRE->getDecl() == D;
}

static bool matchesName(const FunctionDecl *FD, ArrayRef<StringRef> Names) {
  if (!FD->getDeclName().isIdentifier())
    return false;
  const std::string Qualified = FD->getQualifiedNameAsString();
  for (StringRef Name : Names) {
    if (Name.consume_front("::")) {
      if (Qualified == Name)
        return true;
    } else if (FD->getName() == Name || Qualified == Name) {
      return true;
    }
  }
  return false;
}

namespace {

/// Collects the pointer members that a destructor releases, following calls to
/// member functions of the same class one level deep.
class ReleaseCollector {
public:
  ReleaseCollector(const CXXRecordDecl *RD, ArrayRef<StringRef> Deallocators)
      : RD(RD), Deallocators(Deallocators) {}

  ReleaseMap collect(const Stmt *Body) {
    visit(Body, /*Depth=*/0);
    return std::move(Releases);
  }

private:
  void record(const FieldDecl *FD, const Stmt *Site, bool Direct) {
    auto [It, Inserted] = Releases.try_emplace(FD);
    Release &R = It->second;
    if (Inserted || (Direct && !R.Direct)) {
      R.Site = Site;
      R.Direct = Direct;
    }
    R.Guards.insert(Guards.begin(), Guards.end());
  }

  void collectGuards(const Stmt *S, SmallVectorImpl<const FieldDecl *> &Out) {
    if (!S)
      return;
    if (const auto *E = dyn_cast<Expr>(S))
      if (const FieldDecl *FD = getThisMember(E, RD)) {
        Out.push_back(FD);
        return;
      }
    for (const Stmt *Child : S->children())
      collectGuards(Child, Out);
  }

  void visitDelete(const CXXDeleteExpr *Delete) {
    const Expr *Arg = Delete->getArgument()->IgnoreParenImpCasts();
    if (const FieldDecl *FD = getThisPointerMember(Arg, RD)) {
      record(FD, Delete, /*Direct=*/true);
      return;
    }
    // 'delete Array[I]' where 'Array' is a member array of pointers.
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Arg)) {
      if (const FieldDecl *FD = getThisMember(ASE->getBase(), RD);
          FD && FD->getType()->isArrayType())
        record(FD, Delete, /*Direct=*/false);
      return;
    }
    // 'for (T *E : Container) delete E;'
    for (const auto &[LoopVar, Container] : LoopVars)
      if (refersTo(Arg, LoopVar))
        record(Container, Delete, /*Direct=*/false);
  }

  void visit(const Stmt *S, unsigned Depth) {
    if (!S)
      return;

    if (const auto *Delete = dyn_cast<CXXDeleteExpr>(S)) {
      visitDelete(Delete);
    } else if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
      const CXXMethodDecl *Method = MCE->getMethodDecl();
      const Expr *Object = MCE->getImplicitObjectArgument();
      if (Method && Object) {
        if (isa<CXXDestructorDecl>(Method)) {
          // 'P->~T()'
          if (const FieldDecl *FD = getThisPointerMember(Object, RD))
            record(FD, MCE, /*Direct=*/true);
        } else if (Depth == 0 && isThisObject(Object) &&
                   Method->getParent()->getCanonicalDecl() ==
                       RD->getCanonicalDecl()) {
          // A helper such as 'clear()' called from the destructor.
          const FunctionDecl *Def = nullptr;
          if (Method->hasBody(Def))
            visit(Def->getBody(), Depth + 1);
        }
      }
    } else if (const auto *Call = dyn_cast<CallExpr>(S)) {
      if (const FunctionDecl *Callee = Call->getDirectCallee();
          Callee && Call->getNumArgs() > 0 && matchesName(Callee, Deallocators))
        if (const FieldDecl *FD = getThisPointerMember(Call->getArg(0), RD))
          record(FD, Call, /*Direct=*/true);
    } else if (const auto *If = dyn_cast<IfStmt>(S)) {
      const size_t OldSize = Guards.size();
      collectGuards(If->getCond(), Guards);
      visit(If->getThen(), Depth);
      visit(If->getElse(), Depth);
      Guards.resize(OldSize);
      return;
    } else if (const auto *ForRange = dyn_cast<CXXForRangeStmt>(S)) {
      if (const FieldDecl *FD = getThisMember(ForRange->getRangeInit(), RD)) {
        LoopVars.emplace_back(ForRange->getLoopVariable(), FD);
        visit(ForRange->getBody(), Depth);
        LoopVars.pop_back();
        return;
      }
    }

    for (const Stmt *Child : S->children())
      visit(Child, Depth);
  }

  const CXXRecordDecl *RD;
  ArrayRef<StringRef> Deallocators;
  ReleaseMap Releases;
  SmallVector<const FieldDecl *, 4> Guards;
  SmallVector<std::pair<const VarDecl *, const FieldDecl *>, 2> LoopVars;
};

/// Facts about the body of a user-written copy or move operation.
struct OperationFacts {
  /// Members copied plainly from the source: 'F(Other.F)' or 'F = Other.F'.
  llvm::MapVector<const FieldDecl *, const Expr *> PlainCopies;
  /// Member expressions that are part of a plain copy.
  llvm::SmallPtrSet<const MemberExpr *, 8> CopyParts;
  /// Every expression naming a member, on 'this' or on the source.
  SmallVector<const MemberExpr *, 8> MemberRefs;
  /// Members written by an initializer or an assignment.
  llvm::SmallPtrSet<const FieldDecl *, 8> Written;
  /// 'this' or '*this' is used as an object or argument of a call.
  bool PassesThis = false;
  /// The source object is used as an object or argument of a call.
  bool PassesSource = false;
};

class OperationAnalyzer {
public:
  OperationAnalyzer(const CXXMethodDecl *Def, const CXXRecordDecl *RD)
      : Def(Def), RD(RD), Param(Def->getParamDecl(0)) {}

  OperationFacts analyze() {
    if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(Def)) {
      for (const CXXCtorInitializer *Init : Ctor->inits()) {
        const FieldDecl *FD = Init->getMember();
        if (FD && Init->isWritten()) {
          Facts.Written.insert(FD);
          if (FD->getType()->isPointerType())
            if (const MemberExpr *Source =
                    getParamMember(Init->getInit(), Param, FD))
              addCopy(FD, Source, /*LHS=*/nullptr);
        }
        if (Init->isWritten() || Init->isBaseInitializer())
          visit(Init->getInit());
      }
    }
    visit(Def->getBody());
    return std::move(Facts);
  }

private:
  void addCopy(const FieldDecl *FD, const MemberExpr *Source, const Expr *LHS) {
    Facts.PlainCopies.try_emplace(FD, Source);
    Facts.CopyParts.insert(Source);
    if (LHS)
      Facts.CopyParts.insert(cast<MemberExpr>(LHS->IgnoreParenCasts()));
  }

  void visitCallArgument(const Expr *E) {
    if (isThisObject(E))
      Facts.PassesThis = true;
    if (refersTo(E, Param))
      Facts.PassesSource = true;
  }

  void visit(const Stmt *S) {
    if (!S)
      return;

    if (const auto *ME = dyn_cast<MemberExpr>(S)) {
      Facts.MemberRefs.push_back(ME);
    } else if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
      if (BO->isAssignmentOp())
        if (const FieldDecl *FD = getThisMember(BO->getLHS(), RD)) {
          Facts.Written.insert(FD);
          if (BO->getOpcode() == BO_Assign && FD->getType()->isPointerType())
            if (const MemberExpr *Source =
                    getParamMember(BO->getRHS(), Param, FD))
              addCopy(FD, Source, BO->getLHS());
        }
    } else if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(S)) {
      if (const Expr *Object = MCE->getImplicitObjectArgument())
        visitCallArgument(Object);
      for (const Expr *Arg : MCE->arguments())
        visitCallArgument(Arg);
    } else if (const auto *Call = dyn_cast<CallExpr>(S)) {
      // 'Base(std::move(Other))' does not reset the members of the source.
      const FunctionDecl *Callee = Call->getDirectCallee();
      if (!Callee || !Callee->isInStdNamespace() ||
          !Callee->getDeclName().isIdentifier() ||
          (Callee->getName() != "move" && Callee->getName() != "forward"))
        for (const Expr *Arg : Call->arguments())
          visitCallArgument(Arg);
    } else if (const auto *Construct = dyn_cast<CXXConstructExpr>(S)) {
      for (const Expr *Arg : Construct->arguments())
        visitCallArgument(Arg);
    }

    for (const Stmt *Child : S->children())
      visit(Child);
  }

  const CXXMethodDecl *Def;
  const CXXRecordDecl *RD;
  const ParmVarDecl *Param;
  OperationFacts Facts;
};

enum class OperationKind { CopyConstructor, CopyAssignment, Move };

} // namespace

/// Returns the member that \p Facts shows to be copied shallowly, if any.
static const FieldDecl *findShallowCopy(const OperationFacts &Facts,
                                        const ReleaseMap &Releases,
                                        OperationKind Kind) {
  // The operation may re-seat the member through a helper.
  if (Facts.PassesThis)
    return nullptr;
  // The source may be reset through a call, e.g. 'Other.release()'.
  if (Kind == OperationKind::Move && Facts.PassesSource)
    return nullptr;
  for (const auto &Copy : Facts.PlainCopies) {
    const FieldDecl *FD = Copy.first;
    const auto *const It = Releases.find(FD);
    if (It == Releases.end() || !It->second.Direct)
      continue;
    // Any other use of the member, such as 'P->addRef()', 'P = new T(*P)' or
    // 'Other.P = nullptr', means the copy is not left as it is.
    const bool UsedElsewhere =
        llvm::any_of(Facts.MemberRefs, [&](const MemberExpr *ME) {
          return ME->getMemberDecl() == FD && !Facts.CopyParts.contains(ME);
        });
    if (UsedElsewhere)
      continue;
    // A copy that clears an ownership flag, e.g. 'Owns(false)', is
    // intentionally non-owning.
    const bool WritesGuard =
        llvm::any_of(It->second.Guards, [&](const FieldDecl *Guard) {
          return Guard != FD && Facts.Written.contains(Guard);
        });
    if (WritesGuard)
      continue;
    return FD;
  }
  return nullptr;
}

/// Returns whether the implicitly declared copy constructor of \p RD is
/// deleted, or std::nullopt if that is not known without Sema.
static std::optional<bool>
isImplicitCopyConstructorDeleted(const CXXRecordDecl *RD) {
  if (RD->hasUserDeclaredMoveConstructor() ||
      RD->hasUserDeclaredMoveAssignment())
    return true;
  if (!RD->needsImplicitCopyConstructor()) {
    for (const CXXConstructorDecl *Ctor : RD->ctors())
      if (Ctor->isImplicit() && Ctor->isCopyConstructor())
        return Ctor->isDeleted();
    return std::nullopt;
  }
  if (RD->needsOverloadResolutionForCopyConstructor())
    return std::nullopt;
  return RD->defaultedCopyConstructorIsDeleted();
}

static std::optional<bool>
isImplicitCopyAssignmentDeleted(const CXXRecordDecl *RD) {
  if (RD->hasUserDeclaredMoveConstructor() ||
      RD->hasUserDeclaredMoveAssignment())
    return true;
  if (!RD->needsImplicitCopyAssignment()) {
    for (const CXXMethodDecl *Method : RD->methods())
      if (Method->isImplicit() && Method->isCopyAssignmentOperator())
        return Method->isDeleted();
    return std::nullopt;
  }
  if (RD->needsOverloadResolutionForCopyAssignment())
    return std::nullopt;
  return !RD->hasSimpleCopyAssignment();
}

/// Returns the first member that is released directly, if any.
static const ReleaseMap::value_type *
firstDirectRelease(const ReleaseMap &Releases) {
  for (const auto &Entry : Releases)
    if (Entry.second.Direct)
      return &Entry;
  return nullptr;
}

RawOwningPointerMemberCheck::RawOwningPointerMemberCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      Deallocators(utils::options::parseStringList(
          Options.get("Deallocators", "::free;::std::free"))),
      WarnOnImplicitCopy(Options.get("WarnOnImplicitCopy", true)) {}

void RawOwningPointerMemberCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "Deallocators",
                utils::options::serializeStringList(Deallocators));
  Options.store(Opts, "WarnOnImplicitCopy", WarnOnImplicitCopy);
}

void RawOwningPointerMemberCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      cxxRecordDecl(isDefinition(), unless(isUnion()), unless(isLambda()),
                    unless(ast_matchers::isTemplateInstantiation()),
                    has(cxxDestructorDecl().bind("dtor")))
          .bind("class"),
      this);
}

void RawOwningPointerMemberCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *RD = Result.Nodes.getNodeAs<CXXRecordDecl>("class");

  // 'CXXRecordDecl::getDestructor()' does not find the destructor of a class
  // template pattern, so take it from the match.
  const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("dtor");
  const FunctionDecl *DtorDef = nullptr;
  if (!Dtor->hasBody(DtorDef) || DtorDef->isDefaulted())
    return;

  const ReleaseMap Releases =
      ReleaseCollector(RD, Deallocators).collect(DtorDef->getBody());
  if (Releases.empty())
    return;

  const auto NoteRelease = [&](const Release &R, const FieldDecl *FD) {
    diag(R.Site->getBeginLoc(), "%0 is released here", DiagnosticIDs::Note)
        << FD;
  };

  // Implicitly declared copy operations.
  if (WarnOnImplicitCopy) {
    const bool ImplicitCtor =
        !RD->hasUserDeclaredCopyConstructor() &&
        isImplicitCopyConstructorDeleted(RD) == std::optional<bool>(false);
    const bool ImplicitAssign =
        !RD->hasUserDeclaredCopyAssignment() &&
        isImplicitCopyAssignmentDeleted(RD) == std::optional<bool>(false);
    if (ImplicitCtor || ImplicitAssign) {
      const auto &[FD, R] = Releases.front();
      diag(RD->getLocation(),
           "%0 releases pointer member %1 in its destructor, but its "
           "implicitly-declared %select{copy constructor|copy assignment "
           "operator|copy constructor and copy assignment operator}2 "
           "%select{copies|copy}3 it shallowly")
          << RD << FD
          << (ImplicitCtor && ImplicitAssign
                  ? 2U
                  : static_cast<unsigned>(ImplicitAssign))
          << (ImplicitCtor && ImplicitAssign);
      NoteRelease(R, FD);
    }
  }

  // User-declared copy and move operations.
  SmallVector<const CXXMethodDecl *, 4> Operations;
  for (const CXXConstructorDecl *Ctor : RD->ctors())
    if (!Ctor->isImplicit() &&
        (Ctor->isCopyConstructor() || Ctor->isMoveConstructor()))
      Operations.push_back(Ctor);
  for (const CXXMethodDecl *Method : RD->methods())
    if (!Method->isImplicit() && (Method->isCopyAssignmentOperator() ||
                                  Method->isMoveAssignmentOperator()))
      Operations.push_back(Method);

  for (const CXXMethodDecl *Op : Operations) {
    if (Op->isDeleted())
      continue;
    const bool IsCtor = isa<CXXConstructorDecl>(Op);
    const bool IsMove = IsCtor
                            ? cast<CXXConstructorDecl>(Op)->isMoveConstructor()
                            : Op->isMoveAssignmentOperator();
    const OperationKind Kind = IsMove
                                   ? OperationKind::Move
                                   : (IsCtor ? OperationKind::CopyConstructor
                                             : OperationKind::CopyAssignment);
    const unsigned OpIndex = (IsMove ? 2 : 0) + (IsCtor ? 0 : 1);

    const FunctionDecl *Def = nullptr;
    if (!Op->hasBody(Def) && !Op->isExplicitlyDefaulted())
      continue;

    const FieldDecl *Shallow = nullptr;
    const SourceLocation Loc = Def ? Def->getLocation() : Op->getLocation();
    const bool Defaulted = Op->isExplicitlyDefaulted() ||
                           (Def && cast<CXXMethodDecl>(Def)->isDefaulted());
    if (Defaulted) {
      // A defaulted move of a container of pointers empties the source.
      if (!IsMove)
        Shallow = Releases.front().first;
      else if (const auto *Entry = firstDirectRelease(Releases))
        Shallow = Entry->first;
    } else {
      const OperationFacts Facts =
          OperationAnalyzer(cast<CXXMethodDecl>(Def), RD).analyze();
      Shallow = findShallowCopy(Facts, Releases, Kind);
    }
    if (!Shallow)
      continue;

    if (IsMove)
      diag(Loc, "%select{|defaulted }0%select{copy constructor|copy assignment "
                "operator|move constructor|move assignment operator}1 copies "
                "owning pointer member %2 without resetting it in the source; "
                "both objects will release it")
          << Defaulted << OpIndex << Shallow;
    else
      diag(Loc, "%select{|defaulted }0%select{copy constructor|copy assignment "
                "operator|move constructor|move assignment operator}1 copies "
                "owning pointer member %2 shallowly; both objects will release "
                "it")
          << Defaulted << OpIndex << Shallow;
    NoteRelease(Releases.find(Shallow)->second, Shallow);
  }
}

} // namespace clang::tidy::bugprone
