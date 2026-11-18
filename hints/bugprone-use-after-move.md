Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/use-after-move.html
An object is used after it was passed to `std::move` (or forwarded), so its
value is unspecified.
- Use the value before moving it, or copy instead of moving when the value is
  still needed later.
- If the object is reused on purpose, reinitialise it first (assign a new
  value or call `clear()`), which the check recognises.
- Do not read the object through a reference or pointer taken before the move:
  that hides the access from the check and keeps the problem.
