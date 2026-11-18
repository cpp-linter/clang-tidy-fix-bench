Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/unused-return-value.html
The result of a function whose result must be used is ignored. In C code this
is usually reported as `cert-err33-c` for standard library calls such as
`fclose`, `fprintf`, `snprintf` or `remove`, whose return value reports errors.
- Use the result for what it is for: handle the error code, or for C++ use the
  returned iterator (`v.erase(std::remove(...), v.end())`).
- `empty()` used where `clear()` was meant is a common bug.
- When ignoring the result is really intended, `cert-err33-c` accepts an
  explicit `(void)` cast; prefer handling the error where it matters.
