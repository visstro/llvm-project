// RUN: %check_clang_tidy -std=c++11-or-later %s bugprone-raw-owning-pointer-member %t -- \
// RUN:   -config="{CheckOptions: { \
// RUN:     bugprone-raw-owning-pointer-member.IgnorePolymorphicClasses: true}}"

struct Polymorphic {
  int *p;
  virtual void f();
  virtual ~Polymorphic() { delete p; }
};

struct Interface { virtual void g() = 0; };
struct DerivedPolymorphic : Interface {
  int *p;
  void g() override;
  ~DerivedPolymorphic() { delete p; }
};

template <class Base> struct DependentBase : Base {
  int *p;
  ~DependentBase() { delete p; }
};

// User-written shallow copies are still diagnosed in polymorphic classes.
struct PolymorphicShallowCopy {
  int *p;
  virtual void f();
  PolymorphicShallowCopy(const PolymorphicShallowCopy &o) : p(o.p) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: copy constructor copies owning pointer member 'p' shallowly
  PolymorphicShallowCopy &operator=(const PolymorphicShallowCopy &) = delete;
  virtual ~PolymorphicShallowCopy() { delete p; }
};

struct NotPolymorphic {
  // CHECK-MESSAGES: :[[@LINE-1]]:8: warning: 'NotPolymorphic' releases pointer member 'p' in its destructor
  int *p;
  ~NotPolymorphic() { delete p; }
};
