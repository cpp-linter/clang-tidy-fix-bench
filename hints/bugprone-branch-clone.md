Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/branch-clone.html
Two or more branches of an if/else-if chain or switch have identical bodies.
- If the branches really should behave the same, merge them: put the case
  labels together in a switch, or combine the conditions with `||` in an if
  chain. Keep the order in which conditions are evaluated and any side effects.
- If one branch was copied by mistake, the real fix is to correct the branch
  that should differ, not to merge them.
- Do not delete a branch unless its conditions are folded into the remaining one.
