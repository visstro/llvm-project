// RUN: %check_clang_tidy -std=c++11-or-later %s bugprone-raw-owning-pointer-member %t -- \
// RUN:   -config="{CheckOptions: { \
// RUN:     bugprone-raw-owning-pointer-member.Deallocators: 'release_buffer;::ns::destroy', \
// RUN:     bugprone-raw-owning-pointer-member.WarnOnImplicitCopy: false}}"

void release_buffer(char *);
namespace ns { void destroy(int *); }
extern "C" void free(void *);

struct CustomDeallocator {
  char *b;
  CustomDeallocator(const CustomDeallocator &o) : b(o.b) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: copy constructor copies owning pointer member 'b' shallowly; both objects will release it [bugprone-raw-owning-pointer-member]
  ~CustomDeallocator() { release_buffer(b); }
};

struct QualifiedDeallocator {
  int *p;
  QualifiedDeallocator(const QualifiedDeallocator &o) : p(o.p) {}
  // CHECK-MESSAGES: :[[@LINE-1]]:3: warning: copy constructor copies owning pointer member 'p' shallowly
  ~QualifiedDeallocator() { ns::destroy(p); }
};

// 'free' is not in the configured list.
struct NotListed {
  char *b;
  NotListed(const NotListed &o) : b(o.b) {}
  ~NotListed() { free(b); }
};

// Implicit copy operations are not diagnosed with 'WarnOnImplicitCopy: false'.
struct ImplicitCopy {
  int *p;
  ~ImplicitCopy() { delete p; }
};
