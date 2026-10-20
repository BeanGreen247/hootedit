# Performance

Every number below was measured on the development machine with a command in this repository, unless it says otherwise. They are examples of the method, not promises for your hardware. The development machine is a 2-core VM with **software OpenGL** (llvmpipe, under `xvfb-run`) that other programs were also using, so GPU time and keystroke-to-screen latency on real hardware are **not** claimed.

## How long things take

The status bar shows the latest operation and Ctrl+Shift+I lists the last ten, each timed from the input event to the frame that shows the result having been built and submitted to the GL driver (display scan-out is not included), plus the **work** part: the time spent in the operation itself (mapping and indexing a file, scoring file names, scanning files), apart from drawing. `hootedit --timed --cache-stats --script "..."` prints the full list for a scripted session.

Best of three scripted runs (`--timed`, window 1280x720, `/usr/include` as the workspace):

| Operation | total, event to frame | work |
|---|---|---|
| a full redraw with nothing changed (the software-GL floor on this VM) | 6.9 ms | none |
| open `stdio.h` (33 KB) | 9.2 ms | 0.06 ms |
| open a 210 KB header | 7.0 ms | 0.12 ms |
| open a 6.5 MB file, 60,001 lines | 9.8 ms | 1.4 ms |
| open a **995 MB file, 14.5 million lines** | 287 ms | 278 ms (memory-map and index every line) |
| new tab / close tab | 0.27 ms / 1.4 ms | |
| open a menu | 20 ms | |
| open the language picker / theme picker / find box | 15 / 13 / 13 ms | |
| quick open, first time (lists 26,133 files) | 71 ms | 52 ms |
| quick open, one more typed character (scores 26,133 files) | 14 ms | 5.4 ms |
| text search `static_assert` across 26,133 files (283 MB), first 5,000 hits | 137 ms | 137 ms |

Read it as: opening a normal file costs under a millisecond of work and then one frame; a gigabyte opens in under a third of a second; a search over a third of a gigabyte answers in about a tenth of a second. Every row that says 6.9 to 20 ms is mostly the software rasteriser drawing a full 1280x720 frame on a CPU; on a GPU that part is expected to be much smaller, but it has not been measured here.

## Other numbers

| What | Result | How to reproduce |
|---|---|---|
| Edit in the middle of the 14.5M-line file | about 3 us per insert, 4.5 us per erase (a flat line index took about 5,000 us) | 1000 edits through `Buffer` |
| 20,000 scattered edits in a 16 MB file, then undo and redo of all of them | 30 ms and 53 ms; a flat piece vector took 390 ms and 1,060 ms (linear piece search and a memmove per edit), so the pieces now live in chunks of 256 with one total length each, and the line index keeps its offsets in flat arrays with chunks of about sqrt(lines) | `hootedit --bench-core FILE 3` (best of 6) |
| Read one line from that file | about 0.6 us | same |
| Build one frame, 60k-line C++ file, about 38 rows | text rows about 43 us with 98.6% row-cache hits, tab row 6 us, status bar 9 us (GL submit and the line-number pass not included) | `hootedit --bench 2000 FILE` |
| Syntax lexer | about 220 MB/s on dense C code; runs only on row-cache misses | `build/test_syntax` |
| Regex scan, plain string | about 6 to 9 GB/s (a substring search) | `build/test_regex` |
| Regex scan, case-insensitive string | about 6.7 GB/s (SIMD pair prefilter) | `build/test_regex` |
| Regex scan, DFA only | about 1.4 GB/s | `build/test_regex` |
| Idle, editor only | 25 frames in 25 s (cursor blink for the first 10 s, then status text only); about 0.3 s CPU including start-up | `hootedit --exit-after 25 --cache-stats FILE` |
| Idle, terminal panel open with an idle shell | 28 frames in 28 s; about 0.5 s CPU including start-up and the shell | `hootedit --exit-after 25 --script act:term_toggle` |

## Headless CPU benchmark and build flags (2026-10-20, GCC 14.2, every build at -O3)

`hootedit --bench-core FILE 3` on a 16 MB C++ file (480,000 lines), best of 6 runs, run twice and interleaved. Same machine as above (2 cores; `perf` is blocked there, `valgrind --tool=callgrind` works).

| Phase | plain -O3, flat piece vector | now |
|---|---|---|
| open | 8.5 ms | 8.2 ms |
| lex (whole file) | 81 ms | 77 ms |
| regex scan | 36 ms | 33 ms |
| fuzzy filter | 23 ms | 22 ms |
| live checks (lint) | 48 ms | 45 ms |
| 20,000 scattered edits | 390 ms | 30 ms |
| undo and redo of those | 1,060 ms | 53 ms |
| **total** | **1,655 ms** | **about 280 ms** |

On a 190 MB, 4.9M-line file the benchmark total went 2,220 -> 2,040 ms and open 174 -> 135 ms; edits and undo did not regress.

Flags tried against the plain -O3 build (total time of the old benchmark; a change counts only if it beats the noise of about 3% in two interleaved rounds):

| Flag | Result |
|---|---|
| `-fno-plt` with `-Wl,-O1,--as-needed,-z,pack-relative-relocs` | **kept**: about 4% (lint, lex, regex and fuzzy all better), binary 3% smaller, start-up unchanged |
| `-flto` | within noise, binary 16% smaller; not used |
| `-march=native`, `-mtune=skylake`, `-falign-functions=32 -falign-loops=32` | regex scan 36 -> 33 ms and nothing else moves: code-alignment luck, not used |
| `-funroll-loops` | slower: regex scan 36 -> 58 ms, lexer +20% |
| `-fno-threadsafe-statics` | the binary crashes (segmentation fault) |
| `-ffast-math`, `-fno-asynchronous-unwind-tables`, `-fno-stack-protector -fcf-protection=none`, `-fno-pie -no-pie`, `-fprefetch-loop-arrays`, `-ffunction-sections -fdata-sections --gc-sections` | within 1-2%: not used |
| clang 19, same flags | lint -21%, edits -12%, but lexer +8% and fuzzy +14%: about equal in total, not used |
| GCC profile-guided build (`scripts/build-pgo.sh`) | lint -25%, lexer +5%, total about -2%; with `-fno-plt` the same as `-fno-plt` alone. Kept in the installer |
| jemalloc through `LD_PRELOAD` | file open 8.5 -> 16.6 ms, nothing faster: not used |

Code-level changes that mattered: the chunked piece list (a linear piece search and a memmove per edit), line-index offsets in flat arrays with sqrt-sized chunks, and `Doc::col_of_byte` copying only the bytes before the cursor into a stack buffer. Tried and dropped: a hint for the last piece found, and prefix sums per chunk with bisection (fewer instructions, more time). `-O3` is set explicitly in every build, test binaries included.

## Start-up and memory (2026-10-20)

Time to the first frame is about 160 ms (range 150-250 ms) on this machine with software GL: SDL, window and GL context 115-185 ms (the driver), fonts about 24 ms, first frame 20-30 ms, tree and git under 1 ms (git status runs on a thread). The start screen shows the measured start-up time, and the time the last folder open took. A bare launch (no file or folder argument) reads no folder at all. Loading fontconfig on a helper thread made the font phase 10 ms shorter but not the time to the first frame, so it is not done.

Memory at idle is about 130 MB resident, of which 56 MB is libLLVM and 11 MB libgallium (the software GL driver of this machine); the glyph atlas is one byte per pixel (4 MB at 2048x2048) with no copy in main memory. A 190 MB, 4.9M-line file adds about 8 bytes per line for the line index (39 MB); the mapped file pages are shared with the page cache.

Reproduce: `hootedit --bench-core FILE 3`; `HOOTEDIT_TRACE=1 hootedit --exit-after 0.3` for the start-up phases; `valgrind --tool=callgrind hootedit --bench-core FILE 1` then `callgrind_annotate` for instruction counts.

Design rules behind these numbers: nothing is redrawn unless something changed; finished rows are cached as GPU instances and reused until their text, language state or the atlas changes; the buffer never copies the file; search, listing and the terminal run off the input thread and wake the UI only when there is something to show. Two things were measured and fixed along the way: an uncached glyph-advance lookup (150 us per frame) and a flat line index (5 ms per keystroke in a big file).

Not measured yet: keystroke-to-screen latency and start-up on a real GPU, memory growth over long sessions, very long lines, and a search over a repository with millions of files.
