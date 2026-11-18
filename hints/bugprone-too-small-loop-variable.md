Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/too-small-loop-variable.html
The loop counter has a narrower type than the loop bound, so it can overflow
and the loop may never end.
- Give the counter the bound's type (for example `size_t` for `v.size()`, or
  `decltype(n)`), or use a range-based for loop when the index is not needed.
- Check other uses of the counter in the body still compile without new
  signed/unsigned warnings.
