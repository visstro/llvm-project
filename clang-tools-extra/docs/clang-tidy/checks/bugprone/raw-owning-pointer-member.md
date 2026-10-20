```{title} clang-tidy - bugprone-raw-owning-pointer-member
```

# bugprone-raw-owning-pointer-member

Finds classes that release a raw pointer member in their destructor but copy
or move it shallowly. After such a copy, both objects release the same memory,
which is a double free.

A member counts as owned if the destructor, or a member function of the same
class that the destructor calls, releases it with `delete`, `delete[]`, an
explicit destructor call such as `P->~T()`, or one of the
{option}`Deallocators`. A member array of pointers, or a container of pointers
whose elements are deleted in a range-based `for` loop, also counts. Nothing is
reported if the body of the destructor is not visible.

The check reports:

- Implicitly-declared copy operations of a class that owns a pointer member.
  Copy operations that are deleted, for example because a base class or a
  member is not copyable, are not reported.

  ```c++
  struct Buffer {   // warning: 'Buffer' releases pointer member 'Data' in its
                    // destructor, but its implicitly-declared copy constructor
                    // and copy assignment operator copy it shallowly
    char *Data;
    Buffer() : Data(new char[64]) {}
    ~Buffer() { delete[] Data; }
  };
  ```

- User-written or defaulted copy operations that copy the owned pointer as it
  is, as in `P(Other.P)` or `P = Other.P`.

  ```c++
  struct Buffer {
    char *Data;
    Buffer(const Buffer &Other) : Data(Other.Data) {} // warning: copy
        // constructor copies owning pointer member 'Data' shallowly; both
        // objects will release it
    ~Buffer() { delete[] Data; }
  };
  ```

- User-written or defaulted move operations that copy the owned pointer
  without resetting it in the source.

  ```c++
  struct Buffer {
    char *Data;
    Buffer(Buffer &&Other) : Data(Other.Data) {} // warning: move constructor
        // copies owning pointer member 'Data' without resetting it in the
        // source; both objects will release it
    ~Buffer() { delete[] Data; }
  };
  ```

To avoid false positives, a user-written copy or move operation is not reported
if:

- it uses the member again, for example to make a deep copy
  (`P = new T(*Other.P)`) or to update a reference count (`++P->Refs`);
- it passes `this` or `*this` to a function, or calls a member function on it,
  because a helper such as `copyFrom(Other)` may set the member;
- it writes another member that guards the release in the destructor, as in
  `Owns(false)` for `~T() { if (Owns) delete P; }`;
- for a move operation, it resets the member in the source, or passes the
  source to a function such as `Other.release()` or `swap(*this, Other)`.

Fix the issue by deleting the copy operations, by copying the pointee, or
preferably by replacing the raw pointer with a smart pointer such as
`std::unique_ptr`.

The check is related to
{doc}`cppcoreguidelines-special-member-functions
<../cppcoreguidelines/special-member-functions>`, which reports every class
that declares some of the special member functions but not all of them. This
check reports only classes that own a pointer member, and it also looks at the
bodies of the copy and move operations.

## Options

```{option} Deallocators
A semicolon-separated list of the names of functions that release the memory
passed as their first argument, such as a custom `destroy` function. A name
that starts with `::` must match the fully qualified name of the function.
Other names match the unqualified or the fully qualified name. Default is
`::free;::std::free`.
```

```{option} WarnOnImplicitCopy
When `true`, the check reports classes that own a pointer member and whose
copy constructor or copy assignment operator is implicitly declared. Default is
`true`.
```
