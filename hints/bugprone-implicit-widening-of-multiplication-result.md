Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/implicit-widening-of-multiplication-result.html
A multiplication is computed in a narrow type (for example `int`) and only the
result is widened (for example to `size_t` or a pointer offset), so it can
overflow before the widening.
- Widen an operand before multiplying: `static_cast<size_t>(a) * b`
  (in C: `(size_t)a * b`). This is the fix clang-tidy suggests in its notes.
- Casting the whole product (`(size_t)(a * b)`) only silences the warning and
  keeps the overflow.
