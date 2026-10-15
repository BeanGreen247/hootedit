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
| Read one line from that file | about 0.6 us | same |
| Build one frame, 60k-line C++ file, about 38 rows | text rows about 43 us with 98.6% row-cache hits, tab row 6 us, status bar 9 us (GL submit and the line-number pass not included) | `hootedit --bench 2000 FILE` |
| Syntax lexer | about 220 MB/s on dense C code; runs only on row-cache misses | `build/test_syntax` |
| Regex scan, plain string | about 6 to 9 GB/s (a substring search) | `build/test_regex` |
| Regex scan, case-insensitive string | about 6.7 GB/s (SIMD pair prefilter) | `build/test_regex` |
| Regex scan, DFA only | about 1.4 GB/s | `build/test_regex` |
| Idle, editor only | 25 frames in 25 s (cursor blink for the first 10 s, then status text only); about 0.3 s CPU including start-up | `hootedit --exit-after 25 --cache-stats FILE` |
| Idle, terminal panel open with an idle shell | 28 frames in 28 s; about 0.5 s CPU including start-up and the shell | `hootedit --exit-after 25 --script act:term_toggle` |

Design rules behind these numbers: nothing is redrawn unless something changed; finished rows are cached as GPU instances and reused until their text, language state or the atlas changes; the buffer never copies the file; search, listing and the terminal run off the input thread and wake the UI only when there is something to show. Two things were measured and fixed along the way: an uncached glyph-advance lookup (150 us per frame) and a flat line index (5 ms per keystroke in a big file).

Not measured yet: keystroke-to-screen latency and start-up on a real GPU, memory growth over long sessions, very long lines, and a search over a repository with millions of files.
