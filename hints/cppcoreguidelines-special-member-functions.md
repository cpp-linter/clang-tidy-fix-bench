Docs: https://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/special-member-functions.html
A class defines or deletes some of the special member functions (destructor,
copy constructor, copy assignment, move constructor, move assignment) but not
all of them ("rule of five").
- Declare the missing ones explicitly with `= default` or `= delete`, choosing
  what matches the class: a class that owns a resource through a custom
  destructor usually deletes copying; a polymorphic base with
  `virtual ~Base() = default;` usually defaults (or deletes) copy and move.
- Do not change the existing members' behaviour.
