Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/unchecked-optional-access.html
An optional's value is read (`*opt`, `opt->`, `opt.value()`) on a path where it
is not known to hold a value.
- Check first in the same function: `if (opt) { use(*opt); }` or
  `if (!opt) return ...;` before the access.
- Use `opt.value_or(default)` when a default makes sense.
- The check only sees checks in the same function; a check hidden in another
  function call does not count.
