# run2: remediation hint vs no hint, single attempt

> [!WARNING]
> Experimental and AI-judged. The fixer and the judge are models of the same
> family, and no maintainer has reviewed the fixes. See [Limitations](#limitations).

run2 asks whether a short per-check remediation hint, the kind of text a linter
integration could print next to a warning, changes how well a coding agent
fixes clang-tidy warnings that clang-tidy cannot fix on its own.

## Setup

- **Agent:** Claude Code 2.1.311 (Anthropic), default model, which was
  `claude-fable-5-2` for all 342 tasks. Run headless with
  [`scripts/run_agents.py`](../../scripts/run_agents.py): one fresh session per
  task, in a scratch copy of the task directory outside this repository. Only
  the Read, Edit and Write tools were available. User settings, CLAUDE.md files,
  skills, hooks and MCP servers were disabled (`--safe-mode --setting-sources ""`).
- **No checker.** Unlike run1, the agent could not run `check_fix.py`, clang-tidy
  or a compiler. Each sample is one blind attempt.
- **Two conditions** ([`scripts/make_tasks.py`](../../scripts/make_tasks.py)):
  - *plain*: `TASK.md` lists the warnings and the rules (edit only this file, no
    `NOLINT`, write `NOTE.md`).
  - *hint*: the same, plus the check's hint from [`hints/`](../../hints)
    (5–10 lines: what the check means and how to fix it properly).
- **Judge:** Claude Code with the same model, one fresh read-only session per
  candidate ([`scripts/judge.py`](../../scripts/judge.py)). It is a **single
  same-family judge**. The judge saw the warnings, the human diff, the candidate
  diff, the before/human/candidate files and the mechanical result. It did not
  see which condition produced the candidate. Verdicts: `equivalent`,
  `acceptable`, `incorrect`, `no-fix`.
- **Success** = mechanical pass ([`scripts/check_fix.py`](../../scripts/check_fix.py),
  same rules as run1, including the no-`NOLINT` rule in `score.py`) **and** the
  judge verdict `equivalent` or `acceptable`.

## Results (171 samples)

| Run | Hint | Mechanical pass | Judge: equivalent or acceptable | Success | `bugprone-use-after-move` success |
|---|:---:|---:|---:|---:|---:|
| claude-plain-all | no | 156/171 (91%) | 164/171 (96%) | 152/171 (89%) | 6/11 |
| claude-hint-all | yes | 162/171 (95%) | 163/171 (95%) | 156/171 (91%) | 4/11 |
| human fix (scored the same way) | - | 156/171 | - | - | - |

The human fixes pass the mechanical check 156 times out of 171 under
`score.py`. Of the other 15, 13 add a `NOLINT` that covers the target check,
which the scorer rejects for any candidate. The remaining 2 raise the compiler
error count in the stubbed build (dagike/hungryeditor `PreviewBackendFactory.cpp`
4→5, rogy-AquaLab/omniboat_robokit `schneider_model.cpp` 14→23).
`scripts/baseline.py` reports 169/171 for the same fixes because it does not
apply the `NOLINT` rule. Per-sample data:
[`human-baseline/scores.jsonl`](human-baseline/scores.jsonl).

Per-check tables: [`claude-plain-all/report.md`](claude-plain-all/report.md),
[`claude-hint-all/report.md`](claude-hint-all/report.md). Summary:
[`compare.md`](compare.md).

### Hint effect per check

| Check | Samples | Mechanical plain → hint | Judge-accepted plain → hint | Success plain → hint |
|---|---:|---:|---:|---:|
| `bugprone-branch-clone` | 25 | 24 → 24 | 25 → 25 | 24 → 24 |
| `bugprone-implicit-widening-of-multiplication-result` | 25 | 25 → 25 | 25 → 25 | 25 → 25 |
| `bugprone-narrowing-conversions` | 25 | 24 → 25 | 25 → 25 | 24 → 25 |
| `bugprone-signed-char-misuse` | 17 | 17 → 17 | 17 → 17 | 17 → 17 |
| `bugprone-too-small-loop-variable` | 8 | 8 → 8 | 8 → 8 | 8 → 8 |
| `bugprone-unchecked-optional-access` | 2 | 2 → 2 | 2 → 2 | 2 → 2 |
| `bugprone-unused-return-value` | 12 | 8 → 8 | 10 → 11 | 8 → 8 |
| `bugprone-use-after-move` | 11 | 10 → 10 | 6 → 4 | 6 → 4 |
| `cppcoreguidelines-special-member-functions` | 25 | 24 → 24 | 25 → 25 | 24 → 24 |
| `readability-magic-numbers` | 21 | 14 → 19 | 21 → 21 | 14 → 19 |
| **All** | 171 | 156 → 162 | 164 → 163 | 152 → 156 |

Ten samples changed outcome between the conditions: seven improved with the
hint (five magic-numbers, one narrowing-conversions, one unused-return-value)
and three got worse (two use-after-move, one unused-return-value).

### Time and cost

Both runs used `--jobs 2` and ran at the same time on one Mac.

| Run | Wall time | Median / max per task | Cost reported by Claude Code (`total_cost_usd`) |
|---|---:|---:|---:|
| claude-plain-all | 1668 s | 16.8 / 51.3 s | $28.98 |
| claude-hint-all | 1627 s | 16.4 / 52.3 s | $28.01 |

The runs used a Claude subscription login, so the cost column is the
notional figure the CLI reports, not a bill. Judging took 560 s per run;
its cost was not recorded.

### Audit

All 342 sessions ended with `success`, no permission denials and no timeouts.
Every task edited the file and wrote `NOTE.md`. The transcripts contain 776
Read, 475 Edit and 342 Write calls, all inside the task's scratch directory. The
`audit()` heuristic flagged 22 transcripts for `github.com/`. In all 22 the URL
is in the source file the agent read, so these are false positives. Per-task
records: `*/agent.jsonl` (CLI version, model, seconds, cost, audit flags).

## Findings

**The hint mostly helped by keeping edits local.** In plain mode, 7 of the 21
magic-numbers candidates failed the locality rule, typically by putting the new
named constants at file or namespace scope. That is a reasonable fix, but it
lands outside the warned hunk. The hint says "declared in the narrowest sensible
scope", and with it 2 of 21 failed this way. Example (haditabealhojeh/rtopt
`local_search.cpp`):

```diff
 # plain: constants placed ~1100 lines above the warnings (out of scope)
+constexpr std::size_t INTRA_CROSS_EXCHANGE_MIN_ROUTE_SIZE = 5;
+constexpr double REFILL_REGRET_COEFF = 1.5;
 # hint: constants next to their use
+    constexpr int min_intra_cross_exchange_route_size = 5;
+      constexpr double refill_regret_coeff = 1.5;
```

**`bugprone-use-after-move` produced "silent green" fixes.** Seven of the 11
human fixes are a `NOLINT` on a test that checks the moved-from state on
purpose. The task rules forbid `NOLINT`, so the agent had no clean option. It
made the warning disappear anyway, by one of three means:

- deleting the assertion (helix-desktop and chiroptera-shell
  `security_primitives_test.cpp`, libosmium, TT-SpMM `test_cleanup.cpp` and
  `test_optional_reference.cpp`, ClickHouse);
- spelling the move as `static_cast<T&&>(x)` so the check no longer sees
  `std::move` (sequelite-cpp `test_database.cpp`, helix-desktop in plain mode);
- reading the object through a reference taken before the move (libosmium in
  plain mode).

All of these pass the mechanical check except ClickHouse, which also had an
out-of-scope edit (and, in hint mode, one more compiler error). The judge
rejected 5 of them in plain mode and 7 in hint mode. All 12 rejections are on
those seven `NOLINT` samples. The hint for this check warns against reading
through a reference taken before the move. In the two samples that went from
success to failure with the hint, the agent instead deleted the assertion
(chiroptera-shell) or respelled the move (sequelite-cpp).

```diff
 # sequelite-cpp test_database.cpp, hint run
-    Database db_moved_to(std::move(db));
+    // Intentionally leave db with a null handle; it is used below on purpose
+    Database db_moved_to(static_cast<Database&&>(db));
```

**The judge is lenient where the mechanical check is strict.** Of the 15 plain
candidates that failed the mechanical check, the judge accepted 12: 10 had
out-of-scope edits and 2 added a new warning. In hint mode it accepted 7 of 9:
6 out-of-scope and 1 new warning. One case is `(void)item.release()` in the
Bitcoin-derived `httpserver.cpp` samples. It still triggers
`bugprone-unused-return-value`, whose `AllowCastToVoid` option is off by
default. Of the four candidates that used it, the judge marked three
`equivalent` and one `incorrect`. Success keeps the mechanical gate for this reason. Without it, plain mode
would have scored 164 instead of 152.

## Limitations

- **One model, one judge.** One agent model and a judge from the same model
  family, with one attempt per sample and no variance. The judge saw the
  mechanical result, which may have influenced it.
- **No checker in the loop.** The agent never saw clang-tidy output. Results
  measure a single blind attempt, not what an agent achieves with tool feedback.
- **Not comparable 1:1 with run1.** run1 let the fixer run `check_fix.py` and
  iterate, and ran fixer and judge as Claude Code subagents rather than as
  separate headless sessions. Read the differences between runs as differences
  in setup, not as model progress.
- **Same dataset caveats as run1.** Approximate build, near-duplicate samples
  across forks and mirrors, and checks with very few samples
  (`bugprone-unchecked-optional-access`: 2). See the main
  [README](../../README.md#limitations).
- **Hints were written for this benchmark.** They are not taken from the
  clang-tidy documentation, and they were written after run1's failures were
  known.

## Pilot with a second agent (20 samples)

Before the full run, a pilot on 20 samples (2 per check,
[`data/pilot.txt`](../../data/pilot.txt)) also ran GitHub Copilot CLI 1.0.73
with its model set to `auto`, which chose `mai-code-2-flash` for every task.
Each candidate there was judged by both Claude Code and Copilot CLI, and
accepted only if both judges accepted it. Codex CLI was planned but did not
run: its login failed on the test machine. **This is a pilot. 20 samples are too
few to compare agents, and the full run did not include Copilot.**

| Run | Agent (model) | Hint | Mechanical pass | Accepted by both judges | Success | Judges agree on verdict |
|---|---|:---:|---:|---:|---:|---:|
| claude-plain | Claude Code (claude-fable-5-2) | no | 18/20 | 19/20 | 17/20 | 20/20 |
| claude-hint | Claude Code (claude-fable-5-2) | yes | 19/20 | 19/20 | 18/20 | 18/20 |
| copilot-plain | Copilot CLI (mai-code-2-flash) | no | 16/20 | 17/20 | 14/20 | 16/20 |
| copilot-hint | Copilot CLI (mai-code-2-flash) | yes | 16/20 | 18/20 | 15/20 | 18/20 |

"Agree" counts identical verdicts, so `equivalent` vs `acceptable` counts as a
disagreement. In one pilot candidate (copilot-hint, agnocast
`special-member-functions`) the Copilot judge said `equivalent` while the
compiler error count rose from 3 to 4. The Claude judge said `incorrect`, and
the candidate failed the mechanical check. Data: [`pilot/`](pilot).

## Files

```
compare.md                     one row per run
claude-{plain,hint}-all/
  report.md                    per-check table
  results.jsonl                mechanical fields, verdict, success per sample
  judges.jsonl                 judge verdict and reason per sample
  agent.jsonl                  CLI version, model, seconds, cost, audit flags per task
human-baseline/scores.jsonl    human fixes under score.py
pilot/                         the 20-sample pilot, same layout
```

Candidate diffs and agent transcripts are not included; they can be
regenerated with the commands in the main README.
