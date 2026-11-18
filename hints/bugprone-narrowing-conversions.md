Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/narrowing-conversions.html
An implicit conversion may lose information: floating point to integer, a wider
integer to a narrower one, or signed/unsigned of the same size.
- Prefer a destination type that matches the source (for example `size_t` for
  a container size, `double` for a floating point result).
- When the narrowing is intended and the value is known to fit, make it explicit
  with `static_cast<T>(...)` (in C a cast), so the intent is visible.
- Keep behaviour identical: do not change rounding, and add a range check only
  where the value may really not fit.
