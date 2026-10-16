# clang-tidy-fix-bench

A benchmark of real clang-tidy warnings, taken from open-source commits, for
measuring how well an automated tool fixes warnings that clang-tidy cannot fix
on its own.

Each sample is one file from a public GitHub repository at the commit just
before a developer fixed one or more clang-tidy warnings, together with the
fix that was actually merged. A candidate fixer gets the file and the warning
list. It is then scored with clang-tidy itself and compared with the human fix.

## Why

`clang-tidy --fix` only applies fixes that a check ships with. In the LLVM 22
check list (`clang-tools-extra/docs/clang-tidy/checks/list.rst` on
`release/22.x`, aliases and Clang Static Analyzer checks excluded), **189 of
394 checks are not marked as offering fixes**. Many of these are bug-finding
checks (`bugprone-use-after-move`, `bugprone-branch-clone`,
`bugprone-narrowing-conversions`, ...), so their warnings have to be fixed by
hand. CI integrations such as
[cpp-linter](https://github.com/cpp-linter) can report these warnings, but
cannot apply a fix.

This repository asks how often a generated fix for such a warning is
mechanically clean (the warning goes away, nothing new breaks) and actually
correct when compared with what a maintainer merged.

## Dataset

171 samples, 275 warnings, from 98 repositories (129 C++ files, 42 C files).
Every sample's code is under a permissive license (MIT, Apache-2.0,
BSD-2/3-Clause or BSL-1.0); see [NOTICE](NOTICE) for each sample's repository,
commit and license.

| Check | Samples | clang-tidy offers a fix |
|---|---:|:---:|
| `bugprone-branch-clone` | 25 | no |
| `bugprone-implicit-widening-of-multiplication-result` | 25 | yes (fix-it notes) |
| `bugprone-narrowing-conversions` | 25 | no |
| `cppcoreguidelines-special-member-functions` | 25 | no |
| `readability-magic-numbers` | 21 | no |
| `bugprone-signed-char-misuse` | 17 | no |
| `bugprone-unused-return-value` | 12 | no |
| `bugprone-use-after-move` | 11 | no |
| `bugprone-too-small-loop-variable` | 8 | no |
| `bugprone-unchecked-optional-access` | 2 | no |

### How the samples were mined

1. **Search** ([`scripts/mine.py`](scripts/mine.py)). The GitHub commit
   search was queried for each check name and its aliases (for example
   `cppcoreguidelines-narrowing-conversions` for
   `bugprone-narrowing-conversions`). LLVM repositories and forks were
   excluded. Commits were kept only if the repository has a permissive license,
   the commit has a single parent, and it modifies 1–8 C/C++ files with at most
   200 changed lines. This gave 405 candidate commits
   ([`data/candidates.jsonl`](data/candidates.jsonl)).
2. **Validate** ([`scripts/validate.py`](scripts/validate.py)). Each
   (commit, file) pair was checked by running clang-tidy 22.1.0 with only the
   target check enabled, on the file before and after the commit. A pair
   becomes a sample only if
   - at least one warning of the check disappears,
   - the commit adds no new warning of that check, and
   - every changed hunk in the file lies within 15 lines (60 for
     `cppcoreguidelines-special-member-functions`) of a warning that
     disappeared, so the commit is about the warning rather than unrelated edits.

   595 pairs were validated, with at most 25 samples per check and 4 per
   repository per check. 171 passed. The others had no warning in our build
   setup (349), unrelated edits (49), a warning that did not go away (19), or
   new warnings (7). See [`data/validate.jsonl`](data/validate.jsonl).

### Build setup

The projects are not built. The harness ([`scripts/tidy.py`](scripts/tidy.py))
does a sparse checkout of the commit's headers. It passes every header
directory as `-I` (up to 600, in a fixed order) and comments out `#include`
lines whose target cannot be found, keeping line numbers unchanged. C files use
`-std=gnu11` and C++ files `-std=gnu++20`, with the macOS SDK as sysroot. This
is enough for local checks to see the code they flag, but it produces compiler
errors in many files. Those errors are treated as a baseline: a candidate fails
only if it **increases** the error count.

### Sample layout

```
samples/<check>__<owner>__<repo>__<sha10>__<file>/
  before.<ext>    file at the parent commit (the input)
  after.<ext>     file after the human fix
  human.patch     the human diff for this file
  meta.json       repo, commit URL, license, path, flags, warnings
```

## Scoring

### Mechanical check ([`scripts/check_fix.py`](scripts/check_fix.py))

A candidate passes when all of the following hold, with the same flags and
stubs as validation:

- **Target warnings gone.** Every listed warning is gone. Warnings are matched
  by message and source-line text, so they survive line shifts.
- **No new warnings.** The edit adds no new warning of the same check.
- **No new compiler errors.** The error count is not higher than for the
  unedited file under the same harness.
- **Edits stay local.** Every edited range is within 15 lines of a target
  warning (60 for `cppcoreguidelines-special-member-functions`).
- **No suppression.** The edit adds no `NOLINT` / `NOLINTNEXTLINE` /
  `NOLINTBEGIN` covering the check. Existing markers for other checks that sit
  on an edited line do not count.

As a harness check, the human fixes were scored the same way
([`scripts/baseline.py`](scripts/baseline.py),
[`results/baseline.jsonl`](results/baseline.jsonl)). 169 of 171 pass, and all
171 unedited files fail as expected. The two human fixes that fail use names
declared in headers that the stubbed build cannot see, so the error count
goes up.

### Judge review ([`scripts/score.py`](scripts/score.py) `--report`)

Mechanical checks cannot tell a real fix from one that only gets past the
checker. So each candidate diff was also reviewed next to the human diff, with
access to the before/after files. It was given one verdict:

- `equivalent`: the same semantic change as the human fix.
- `acceptable`: a different fix that is still correct and mergeable.
- `incorrect`: changes behaviour unintentionally, only dodges the checker,
  changes what a test asserts, or would be rejected.

The judge verdicts are recorded separately from the mechanical result.
**Success** means mechanical pass **and** `equivalent` or `acceptable`.

## Results: run1

**How the fixes were generated.** All fixes in this run were generated by
Claude Code, run locally as subagents, with one attempt per sample. Each
fixer received only what [`scripts/make_tasks.py`](scripts/make_tasks.py) puts
in a task directory:

- `before.*`
- the check name
- the warning lines and messages
- the instructions in `TASK.md`

The human fixes (`after.*`, `human.patch`) were moved out of the working tree
while fixes were generated. Fixers were told not to read the dataset, the
cached upstream clones or the web. **Fixers could run `check_fix.py` on their
own edits and iterate**, the way a developer would rerun clang-tidy, so the
mechanical pass rate measures the result after tool feedback, not a single
blind attempt. The judge reviews were also done by Claude Code subagents.

| Check | Samples | Mechanical pass | Judge: equivalent to human | Judge: equivalent or acceptable | Success (mechanical and judge) |
|---|---:|---:|---:|---:|---:|
| `bugprone-branch-clone` | 25 | 24 (96%) | 18 (72%) | 25 (100%) | 24 (96%) |
| `bugprone-implicit-widening-of-multiplication-result` | 25 | 25 (100%) | 22 (88%) | 25 (100%) | 25 (100%) |
| `bugprone-narrowing-conversions` | 25 | 25 (100%) | 17 (68%) | 25 (100%) | 25 (100%) |
| `bugprone-signed-char-misuse` | 17 | 17 (100%) | 12 (71%) | 17 (100%) | 17 (100%) |
| `bugprone-too-small-loop-variable` | 8 | 8 (100%) | 5 (62%) | 8 (100%) | 8 (100%) |
| `bugprone-unchecked-optional-access` ⚠️ | 2 | 2 (100%) | 1 (50%) | 2 (100%) | 2 (100%) |
| `bugprone-unused-return-value` | 12 | 12 (100%) | 9 (75%) | 12 (100%) | 12 (100%) |
| `bugprone-use-after-move` | 11 | 11 (100%) | 4 (36%) | 4 (36%) | 4 (36%) |
| `cppcoreguidelines-special-member-functions` | 25 | 25 (100%) | 20 (80%) | 25 (100%) | 25 (100%) |
| `readability-magic-numbers` | 21 | 21 (100%) | 12 (57%) | 21 (100%) | 21 (100%) |
| **All** | 171 | 170 (99%) | 120 (70%) | 164 (96%) | 163 (95%) |

⚠️ `bugprone-unchecked-optional-access` has only 2 samples, which is too few
to say anything about this check.

Per-sample data:

- [`results/run1/results.jsonl`](results/run1/results.jsonl): mechanical fields plus verdict and reason
- [`results/run1/patches/`](results/run1/patches): candidate diffs
- [`results/run1/notes/`](results/run1/notes): the fixer's own explanation
- [`results/run1/judge/`](results/run1/judge): verdicts

### Typical failures

**Getting past `bugprone-use-after-move` in tests (6 of 11 samples).** These
tests check the moved-from state on purpose. Instead of adding a reasoned
suppression, as the maintainers did, the candidate read the object through a
reference taken before the move. The warning disappears, but the code does
exactly what the check warns about:

```diff
+    const security::SecureBuffer& movedFrom = original;
     security::SecureBuffer moved(std::move(original));
-    ok = expect(original.empty(), "moved-from secure buffer was not empty") && ok;
+    ok = expect(movedFrom.empty(), "moved-from secure buffer was not empty") && ok;
```

(BevActual/helix-desktop. The human fix added
`// NOLINTNEXTLINE(bugprone-use-after-move)` with a comment.)

**Changing what a test asserts (1 sample).** In salmanra/TT-SpMM-Nov2025
`test_cleanup.cpp`, the candidate removed `EXPECT_TRUE(captured.empty())` and
asserted something else inside the lambda. The mechanical checks pass, but the
test no longer checks the same property.

**An edit too far from the warning (1 sample).** In t0k0sh1/ry `codegen.cpp`,
merging duplicate `if constexpr` branches meant deleting a duplicate branch
about 60 lines below the warning. The judge rated the change correct, but it
fails the locality rule. The human fix was a `NOLINT`.

The reverse also occurs. In several samples the maintainers suppressed the
warning with `NOLINT`, and the candidate made a real code change that the
judge accepted (for example rxdu/ctfsm `state_machine.hpp`,
ralfkonrad/ql_performance_testing `TestSuiteFixture.hpp`). In those cases
"equivalent to human" is not the right target, which is why
`acceptable` is a separate verdict.

## Reproducing

Requirements: macOS with Xcode command line tools, clang-tidy 22 on `PATH` (or
set `CLANG_TIDY`), Python 3.10+, and `git`. `gh` (authenticated) is only needed
to mine new candidates. Upstream repositories are fetched on demand into
`cache/` (several GB for all samples; not part of this repository).

```bash
# score the human fixes (harness sanity check) -> results/baseline.jsonl
python3 scripts/baseline.py 8

# create blind task directories -> runs/<run>/tasks/<id>/
python3 scripts/make_tasks.py myrun
# ... let your fixer edit the file in each task directory ...

# mechanical scoring and review packets -> runs/myrun/scores.jsonl, runs/myrun/review/
python3 scripts/score.py myrun

# after writing runs/myrun/judge/<id>.json ({"verdict": ..., "reason": ...}):
python3 scripts/score.py myrun --report   # -> runs/myrun/report.md
```

To check a single edited file:
`python3 scripts/check_fix.py <sample-id> <path-to-file>`.

## Limitations

- **Approximate build.** Header stubbing means some warnings depend on
  incomplete type information, and compiler-error counts are only compared,
  not required to be zero. A fix that compiles in the stub environment may
  still fail in the real build. The fixes were not compiled or tested in their
  projects.
- **Tool feedback.** Fixers could run the scorer's own checker, so the
  mechanical pass rate is close to 100% by construction. The judge columns
  carry the information.
- **Judge.** The verdicts come from a model of the same family as the fixer,
  not from the projects' maintainers, and may be lenient. Treat `acceptable`
  as "plausible", and read the patches.
- **Single run.** There is one attempt per sample from one fixer, so no
  variance is reported.
- **Selection bias.** Samples come from commits whose message names the check,
  so they favour small, local fixes. Many samples are near-duplicates from
  mirrors and forks of the same code: libexpat (16 samples across three
  repositories), OpenROAD (10 across two), helix-desktop/chiroptera-shell (6
  across two) and Bitcoin-derived `httpserver.cpp` (4). Per-check rates are
  therefore less independent than the sample counts suggest.
- **Small checks.** `bugprone-unchecked-optional-access` (2) and
  `bugprone-too-small-loop-variable` (8) are too small for per-check
  conclusions.

## License

The scripts are licensed under the [Apache License 2.0](LICENSE). Code in
`samples/` (and the candidate patches derived from it in `results/`) remains
under each upstream project's own license. See [NOTICE](NOTICE).
