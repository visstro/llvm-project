// RUN: %check_clang_tidy -std=c++11-or-later %s bugprone-raw-owning-pointer-member %t

typedef __SIZE_TYPE__ size_t;
extern "C" void free(void *);
extern "C" void *malloc(size_t);
extern "C" char *strdup(const char *);

namespace std {
template <class T> struct remove_reference { typedef T type; };
template <class T> struct remove_reference<T &> { typedef T type; };
template <class T>
typename remove_reference<T>::type &&move(T &&t) { return static_cast<typename remove_reference<T>::type &&>(t); }
template <class T> void swap(T &a, T &b) { T tmp = a; a = b; b = tmp; }
template <class T> struct vector {
  T *begin();
  T *end();
  T &operator[](size_t);
  size_t size() const;
};
template <class K, class V> struct pair { K first; V second; };
template <class K, class V> struct map {
  struct iterator {
    pair<K, V> &operator*();
    pair<K, V> *operator->();
    iterator &operator++();
    bool operator!=(const iterator &) const;
  };
  iterator begin();
  iterator end();
};
struct string {
  string();
  string(const string &);
  string &operator=(const string &);
  ~string();
};
} // namespace std

// Implicit copy operations.

struct ImplicitCopy {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'ImplicitCopy' releases pointer member 'p' in its destructor, but its implicitly-declared copy constructor and copy assignment operator copy it shallowly [bugprone-raw-owning-pointer-member]
  int *p;
  ImplicitCopy() : p(new int) {}
  ~ImplicitCopy() { delete p; }
  // CHECK-MESSAGES: :[[@LINE-1]]:21: note: 'p' is released here
};

struct ImplicitAssignOnly {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'ImplicitAssignOnly' releases pointer member 'p' in its destructor, but its implicitly-declared copy assignment operator copies it shallowly
  int *p;
  ImplicitAssignOnly() : p(new int) {}
  ImplicitAssignOnly(const ImplicitAssignOnly &o) : p(new int(*o.p)) {}
  ~ImplicitAssignOnly() { delete p; }
};

struct ArrayNew {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'ArrayNew' releases pointer member 'p' in its destructor
  int *p;
  ArrayNew() : p(new int[4]) {}
  ~ArrayNew() { delete[] p; }
};

struct MallocFree {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'MallocFree' releases pointer member 's' in its destructor
  char *s;
  MallocFree() : s(strdup("x")) {}
  ~MallocFree() { free(s); }
};

struct Elem { ~Elem(); };

struct ExplicitDestructorCall {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'ExplicitDestructorCall' releases pointer member 'e' in its destructor
  Elem *e;
  ~ExplicitDestructorCall() { e->~Elem(); free(e); }
};

struct ReleasedByHelper {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'ReleasedByHelper' releases pointer member 'p' in its destructor
  int *p;
  ~ReleasedByHelper() { clear(); }
  void clear() { delete p; p = 0; }
  // CHECK-MESSAGES: :[[@LINE-1]]:18: note: 'p' is released here
};

struct PointerArray {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'PointerArray' releases pointer member 'children' in its destructor
  int *children[2];
  ~PointerArray() { delete children[0]; delete children[1]; }
};

struct PointerContainer {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'PointerContainer' releases pointer member 'v' in its destructor
  std::vector<int *> v;
  ~PointerContainer() { for (int *x : v) delete x; }
};

struct ConditionalOwner {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'ConditionalOwner' releases pointer member 'p' in its destructor
  int *p;
  bool owns;
  ~ConditionalOwner() { if (owns) delete p; }
};

struct OutOfLineDtor {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'OutOfLineDtor' releases pointer member 'p' in its destructor
  int *p;
  ~OutOfLineDtor();
};
OutOfLineDtor::~OutOfLineDtor() { delete p; }

template <class T> struct TemplateOwner {
  // CHECK-MESSAGES: :[[@LINE-1]]:27: warning: 'TemplateOwner' releases pointer member 'p' in its destructor
  T *p;
  ~TemplateOwner() { delete p; }
};
TemplateOwner<int> Instantiated;

template <class T> struct TemplateWithClassMember {
  // CHECK-MESSAGES: :[[@LINE-1]]:27: warning: 'TemplateWithClassMember' releases pointer member 'p' in its destructor, but its implicitly-declared copy constructor and copy assignment operator copy it shallowly
  std::string name;
  T *p;
  ~TemplateWithClassMember() { delete p; }
};

template <class T> struct TemplateWithDependentMember {
  // CHECK-MESSAGES: :[[@LINE-1]]:27: warning: 'TemplateWithDependentMember' releases pointer member 'p' in its destructor
  T value;
  T *p;
  ~TemplateWithDependentMember() { delete p; }
};
TemplateWithDependentMember<int> InstantiatedDependent;

template <class T> struct TemplateNeverInstantiated {
  // CHECK-MESSAGES: :[[@LINE-1]]:27: warning: 'TemplateNeverInstantiated' releases pointer member 'p' in its destructor
  T value;
  T *p;
  ~TemplateNeverInstantiated() { delete p; }
};

struct IndexLoop {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'IndexLoop' releases pointer member 'v' in its destructor
  std::vector<int *> v;
  ~IndexLoop() { for (size_t i = 0; i < v.size(); ++i) delete v[i]; }
};

struct PointerToPointers {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'PointerToPointers' releases pointer member 'items' in its destructor
  int **items;
  int count;
  ~PointerToPointers() { for (int i = 0; i < count; ++i) delete items[i]; delete[] items; }
};

struct IteratorLoop {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'IteratorLoop' releases pointer member 'm' in its destructor
  std::map<int, int *> m;
  ~IteratorLoop() {
    for (std::map<int, int *>::iterator it = m.begin(); it != m.end(); ++it)
      delete it->second;
  }
};

struct RangeForPair {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'RangeForPair' releases pointer member 'm' in its destructor
  std::map<int, int *> m;
  ~RangeForPair() { for (auto &kv : m) delete kv.second; }
};

struct Node { Node *next; };
struct LinkedList {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'LinkedList' releases pointer member 'head' in its destructor
  Node *head;
  ~LinkedList() {
    Node *cur = head;
    while (cur) { Node *next = cur->next; delete cur; cur = next; }
  }
};

// User-written copy operations.

struct ShallowCopyCtor {
  int *p;
  ShallowCopyCtor() : p(new int) {}
  ShallowCopyCtor(const ShallowCopyCtor &o) : p(o.p) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: copy constructor copies owning pointer member 'p' shallowly; both objects will release it
  // CHECK-MESSAGES: :[[@LINE+4]]:24: note: 'p' is released here
  ShallowCopyCtor &operator=(const ShallowCopyCtor &o) { p = o.p; return *this; }
  // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: copy assignment operator copies owning pointer member 'p' shallowly; both objects will release it
  // CHECK-MESSAGES: :[[@LINE+1]]:24: note: 'p' is released here
  ~ShallowCopyCtor() { delete p; }
};

struct OutOfLineCopy {
  int *p;
  OutOfLineCopy(const OutOfLineCopy &o);
  OutOfLineCopy &operator=(const OutOfLineCopy &o) = delete;
  ~OutOfLineCopy() { delete p; }
};
OutOfLineCopy::OutOfLineCopy(const OutOfLineCopy &o) : p(o.p) {}
// CHECK-MESSAGES: :[[@LINE-1]]:16: warning: copy constructor copies owning pointer member 'p' shallowly

struct DefaultedCopy {
  int *p;
  DefaultedCopy(const DefaultedCopy &) = default;
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: defaulted copy constructor copies owning pointer member 'p' shallowly
  DefaultedCopy &operator=(const DefaultedCopy &) = delete;
  ~DefaultedCopy() { delete p; }
};

struct ByValueAssign {
  int *p;
  ByValueAssign(const ByValueAssign &) = delete;
  ByValueAssign &operator=(ByValueAssign o) { p = o.p; return *this; }
  // CHECK-MESSAGES: :[[@LINE-1]]:18: warning: copy assignment operator copies owning pointer member 'p' shallowly
  ~ByValueAssign() { delete p; }
};

template <class T> struct TemplateShallowCopy {
  T *p;
  TemplateShallowCopy(const TemplateShallowCopy &o) : p(o.p) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: copy constructor copies owning pointer member 'p' shallowly
  TemplateShallowCopy &operator=(const TemplateShallowCopy &) = delete;
  ~TemplateShallowCopy() { delete p; }
};

// User-written move operations.

struct MoveWithoutReset {
  int *p;
  MoveWithoutReset(const MoveWithoutReset &) = delete;
  MoveWithoutReset &operator=(const MoveWithoutReset &) = delete;
  MoveWithoutReset(MoveWithoutReset &&o) : p(o.p) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: move constructor copies owning pointer member 'p' without resetting it in the source; both objects will release it
  MoveWithoutReset &operator=(MoveWithoutReset &&o) { p = o.p; return *this; }
  // CHECK-MESSAGES: :[[@LINE-1]]:21: warning: move assignment operator copies owning pointer member 'p' without resetting it in the source
  ~MoveWithoutReset() { delete p; }
};

struct DefaultedMove {
  int *p;
  DefaultedMove(DefaultedMove &&) = default;
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: defaulted move constructor copies owning pointer member 'p' without resetting it in the source
  ~DefaultedMove() { delete p; }
};

struct Base { Base(); Base(Base &&); };
struct DerivedMove : Base {
  int *p;
  DerivedMove(DerivedMove &&o) : Base(std::move(o)), p(o.p) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: move constructor copies owning pointer member 'p' without resetting it in the source
  ~DerivedMove() { delete p; }
};

// No warnings below.

struct DeepCopy {
  int *p;
  DeepCopy() : p(new int) {}
  DeepCopy(const DeepCopy &o) : p(new int(*o.p)) {}
  DeepCopy &operator=(const DeepCopy &o) { *p = *o.p; return *this; }
  ~DeepCopy() { delete p; }
};

struct DeepCopyInBody {
  int *p;
  DeepCopyInBody(const DeepCopyInBody &o) : p(o.p) { p = new int(*o.p); }
  DeepCopyInBody &operator=(const DeepCopyInBody &) = delete;
  ~DeepCopyInBody() { delete p; }
};

struct DeletedCopy {
  int *p;
  DeletedCopy(const DeletedCopy &) = delete;
  DeletedCopy &operator=(const DeletedCopy &) = delete;
  ~DeletedCopy() { delete p; }
};

class PrivateUndefinedCopy {
  int *p;
  PrivateUndefinedCopy(const PrivateUndefinedCopy &);
  PrivateUndefinedCopy &operator=(const PrivateUndefinedCopy &);
public:
  ~PrivateUndefinedCopy() { delete p; }
};

struct MoveOnlyByDeclaration {
  int *p;
  MoveOnlyByDeclaration(MoveOnlyByDeclaration &&o) : p(o.p) { o.p = 0; }
  ~MoveOnlyByDeclaration() { delete p; }
};

struct NonCopyable {
  NonCopyable();
  NonCopyable(const NonCopyable &) = delete;
  NonCopyable &operator=(const NonCopyable &) = delete;
};
struct CopyDeletedByBase : NonCopyable {
  int *p;
  ~CopyDeletedByBase() { delete p; }
};
struct CopyDeletedByMember {
  NonCopyable m;
  int *p;
  ~CopyDeletedByMember() { delete p; }
};

struct CopyAndSwap {
  int *p;
  CopyAndSwap(const CopyAndSwap &o) : p(new int(*o.p)) {}
  CopyAndSwap &operator=(CopyAndSwap o) { std::swap(p, o.p); return *this; }
  ~CopyAndSwap() { delete p; }
};

struct CopyThroughHelper {
  int *p;
  CopyThroughHelper(const CopyThroughHelper &o) : p(o.p) { copyFrom(o); }
  CopyThroughHelper &operator=(const CopyThroughHelper &o) { p = o.p; copyFrom(o); return *this; }
  void copyFrom(const CopyThroughHelper &);
  ~CopyThroughHelper() { delete p; }
};

struct Counted { int refs; };
struct InlineRefCount {
  Counted *c;
  InlineRefCount(const InlineRefCount &o) : c(o.c) { ++c->refs; }
  InlineRefCount &operator=(const InlineRefCount &) = delete;
  ~InlineRefCount() { if (--c->refs == 0) delete c; }
};

struct RefCounted { void ref(); void unref(); };
struct IntrusiveRefCount {
  RefCounted *r;
  ~IntrusiveRefCount() { r->unref(); }
};

struct NonOwningCopy {
  int *p;
  bool owns;
  NonOwningCopy(const NonOwningCopy &o) : p(o.p), owns(false) {}
  NonOwningCopy &operator=(const NonOwningCopy &) = delete;
  ~NonOwningCopy() { if (owns) delete p; }
};

struct MoveResetsSource {
  int *p;
  MoveResetsSource(MoveResetsSource &&o) : p(o.p) { o.p = 0; }
  MoveResetsSource &operator=(MoveResetsSource &&o) { std::swap(p, o.p); return *this; }
  ~MoveResetsSource() { delete p; }
};

struct MoveReleasesSource {
  int *p;
  int *release();
  MoveReleasesSource(MoveReleasesSource &&o) : p(o.p) { o.release(); }
  ~MoveReleasesSource() { delete p; }
};

struct Observer {
  int *p;
  Observer(int *q) : p(q) {}
  ~Observer() {}
};

struct DefaultedDtor {
  int *p;
  ~DefaultedDtor() = default;
};

struct BodyNotVisible {
  int *p;
  ~BodyNotVisible();
};

struct ContainerMovedByDefault {
  std::vector<int *> v;
  ContainerMovedByDefault(ContainerMovedByDefault &&) = default;
  ~ContainerMovedByDefault() { for (int *x : v) delete x; }
};

template <class T> struct TemplateWithDeletedMember {
  NonCopyable m;
  T *p;
  ~TemplateWithDeletedMember() { delete p; }
};

struct AliasMemberDeleted {
  Node *head;
  AliasMemberDeleted(const AliasMemberDeleted &) = delete;
  AliasMemberDeleted &operator=(const AliasMemberDeleted &) = delete;
  ~AliasMemberDeleted() { Node *cur = head; delete cur->next; }
};
struct AliasNotOwned {
  Node *head;
  ~AliasNotOwned() { Node *cur = head; delete cur->next; }
};

struct UnrelatedDelete {
  int *p;
  ~UnrelatedDelete() { int *q = p; (void)q; delete new int; }
};

union U {
  int *p;
  ~U() { delete p; }
};
