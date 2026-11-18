Docs: https://clang.llvm.org/extra/clang-tidy/checks/readability/magic-numbers.html
A numeric literal is used without a name (also reported as
`cppcoreguidelines-avoid-magic-numbers`).
- Replace it with a named constant that says what it means (`constexpr`,
  `static const`, an `enum`, or `#define` in C code that already uses them),
  declared in the narrowest sensible scope.
- Keep the value and its type exactly the same (including suffixes such as
  `U`, `L` or `f`).
