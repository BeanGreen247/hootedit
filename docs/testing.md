# Tests

Hootedit, fleetwm and Lestrix test the same way, so a command or a habit learned in one works in the others: GoogleTest for the unit tests, the same group names, the same runner command line.

```
scripts/test.sh                  everything: unit, perf, ui, static
scripts/test.sh unit             one group (unit | perf | ui | static)
scripts/test.sh lint vim         one or more areas of the unit tests
scripts/test.sh -f 'Lint.K8s*'   any gtest filter          scripts/test.sh -v lint   every test case
scripts/test.sh -l               list the test cases       scripts/test.sh --areas   list the areas
scripts/test.sh -r 20 --shuffle  repeat in random order    scripts/test.sh --xml out.xml   JUnit report
```

`meson test -C build` runs the same things (`--suite areas`, `--suite perf`, `--suite ui`).

## The groups

| group | what it is | here |
|---|---|---|
| unit | GoogleTest, one binary built with AddressSanitizer and UBSan, sliced into areas with `--gtest_filter` | 111 tests in 12 areas: buffer, doc, syntax, fuzzy, diff, merge, regex, vim, outline, tree, lint, preview |
| differential | the code against a reference that is obviously right | the piece table against a `std::string` with random edits (`Buffer.RandomEditsAgreeWithAStdString`), the regex engine against `std::regex` on 30,000 random patterns, `scan()` against line-by-line `find()`, the line diff against an LCS table, undo/redo walks against a string model |
| fuzz | seeded random input that must not crash or disagree | the same random edit and pattern generators, running under the sanitizers; the generators use fixed seeds so a failure repeats |
| perf | a speed gate with loose limits (about 8x below a 2-core laptop) that catches a quadratic loop or an extra copy per line, not a 10% change | `hootedit-perf-gate`: lexer, fuzzy filter, regex scan of 32 MB, 20,000 edits on an open file, `flat()`, live checks on code and on manifests |
| ui | the real window driven by `--script`, state read back with the `dump` step, under `xvfb-run` | `tests/ui_test.sh`: 87 scenarios (typing, vim, document previews, git markers, live checks, Kubernetes, hot reload, ...) |
| static | checks that need no build | `scripts/check-static.sh`: scripts parse, relative links in the docs resolve, source files have a header |

## Writing a test

Add a `TEST(Area, WhatItChecks)` to the matching `tests/test_*.cpp`; new files go into `unit_sources` in `meson.build` and, if they are a new area, into `test_areas` and `scripts/test.sh`. Use `EXPECT_*` for checks that can go on and `ASSERT_*` when the rest of the test makes no sense after a failure. Random tests use a fixed seed. A new hot path gets a line in `tests/perf_gate.cpp` with a limit well above what it measures. A user-visible feature gets a scenario in `tests/ui_test.sh`.

## Measuring instead of gating

`hootedit --bench-core FILE [REPS] [DIR]` times the CPU paths without a window. `scripts/compare-editors.sh` compares start-up, memory, idle CPU and typing latency with other editors (see [COMPARISON.md](COMPARISON.md)). `hootedit --bench N FILE` times frames (on a real GPU, for the numbers in [performance.md](performance.md)).
