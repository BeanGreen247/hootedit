# Changelog

## Unreleased
- Ctrl+click on an http, https, ftp, file or mailto address in the text opens it with `xdg-open` (no shell, scheme checked, double fork like the terminal launcher). Detection is `link_at` in `src/link.h`, covered by `Link.*` in `tests/test_link.cpp`.
- Terminal panel: vendored Lestrix engine brought up to Lestrix 944dee3 (`vt.cpp`: a full-screen scroll slides a window over the row order instead of moving it; `tcore.cpp`: default read delay 200 -> 400 us). The newer Lestrix features (OSC 133, kitty keyboard, images, links) are not in the vendored copy yet.
- CPU rasterizer: `--render auto|cpu|gpu` (or `HOOTEDIT_RENDER`). The same draw instructions as the GL path are painted into the window surface, with the glyph atlas kept in memory; no OpenGL context is created. `auto` picks it when no readable DRM render node or NVIDIA device exists, and when no OpenGL context can start. Only the 32x32 tiles whose content hash changed are repainted and presented (an unchanged frame is not presented at all); with an XRGB8888 surface the frame is drawn straight into the window's pixels. `HOOTEDIT_SOFT_VERIFY=1` repaints every frame from scratch and exits 3 if the incremental frame differs. Pixel tests in `tests/test_raster.cpp`, a verified scripted session in `tests/ui_test.sh`.
- Fix: the text cursor could stay hidden. The first key or click after a pause started the blink with a stale clock and hid the cursor at once, and when blinking ended after 10 s of idle in the off phase nothing woke the loop to show it again (up to several seconds hidden).

## 0.3.1 (2026-10-27)
- The syntax-token scratch buffer is released after 10 s without input once it has grown past 1 MB (a 16 MB single-line file: 198 MB -> 149 MB resident when idle; massif and numbers in `docs/performance.md`).
- Fix: the Markdown preview stopped scrolling long before the end of a document. The scroll range came from a separate height estimate that ignored tables and mis-sized code blocks, quotes and lists; it is now the height measured by the drawing code itself (`preview_layout`), cached per document, width and font. Scroll range before -> after (1024x768, 400-block test files; `tests/ui_test.sh` `preview-long.md` also covers it): tables 1,996 -> 13,004 px (85% of the document was unreachable), code blocks 8,496 -> 10,404, quotes 4,596 -> 4,604, lists 11,096 -> 9,304 (scrolled past the end), paragraphs 19,396 -> 10,604 (blank space past the end), diagrams 4,996 -> 3,804. Cost of the scroll-range calculation per wheel event (400-block files, one run each, in-process timer): paragraphs 6.8 ms -> 0.00005 ms, quotes 2.4 ms -> 0.00006 ms (the old estimate re-wrapped every block on each event; the new one is cached); the first measuring pass after an edit or a resize costs 0.03-1.8 ms.
## 0.3.0 (2026-10-20)
- Documentation preview for Markdown, AsciiDoc and reStructuredText (`.md`, `.adoc`, `.rst` and more): Preview and Split controls in the tab strip, GitHub and GitLab rendering profiles, light or dark preview palette, drawn natively with no scripts or remote content (`docs/document-preview.md`).
- Settings: separate toggles for the timing and for the CPU, memory and cache statistics in the status bar.
- `install.sh` builds on a fresh clone without GoogleTest, and shows the meson and ninja errors when a build fails.
- `docs/performance.md` has the headless benchmark table, every build flag tried with its result, the start-up breakdown and the memory notes.
- Piece table in chunks (`src/piecelist.h`, pieces 16 bytes instead of 24): 20,000 scattered edits in a 16 MB file 390 ms -> 33 ms, undo and redo of them 1,060 ms -> 60 ms (`--bench-core`, total 1,655 ms -> about 300 ms).
- Column lookup (`Doc::col_of_byte`) copies only the bytes before the cursor, into a stack buffer; the line index sizes its chunks to the square root of the line count and keeps its per-chunk offsets and first-line numbers in flat arrays. Together about 12% less time for scattered edits and undo (edits 34 -> 30 ms, undo 63 -> 53 ms on the 16 MB benchmark; on a 190 MB, 4.9M-line file open 174 -> 135 ms and the whole benchmark 7% faster).
- `-fno-plt` and lighter link flags: about 4% less time on `--bench-core`, binary 3% smaller; `-O3` is set explicitly for every build, test binaries included.
- A bare launch starts with the sidebar hidden and reads no folder until one is opened; the start screen shows the start-up time and the time the last folder open took.
- Files ignored by git get no change bars in the gutter (and no inline blame).
- Mouse wheel scrolls the Open File / Open Folder / Save As lists.
- The Kubernetes CRD lint test creates its own scratch folder.

## 0.2.0 (2026-10-15)
- Settings page (cog at the top right, Ctrl+, , File > Settings) with search; vim keys, live checks, outline, themes, git, terminal and files settings moved there from the View menu. Sidebar toggle button at the top right.
- Live checks while you type: ansible-lint rules, Kubernetes field checks from the API tables, CRDs from the workspace, Helm, Kustomize, YAML, JSON, shell, Python, C, JS, Dockerfile, Makefile, TOML, XML, Markdown, SQL, CSS; fixes, problem list, F8, Ctrl+.
- Hot reload of files changed on disk, with the time it took.
- Vim engine (modes, operators, text objects, registers, macros, `:s`, `:g`, block mode, surround).
- Preview tabs, compact folders, Explorer git status, open editors, outline.
- Window stays hidden until the first frame; git status and blame on threads.
- Faster find and replace on large files (flat buffer scans), in-place piece erase, `--bench-core`, profile-guided install build.
- GoogleTest unit tests under ASan/UBSan, speed gate, `scripts/test.sh`, static checks; `xlatency` and `compare-editors.sh` for measured editor comparisons.

## 0.1.0 (2026-10-14)
First tagged state: the editor with tabs, VS Code shortcuts, command palette, Replace, the Source Control view (graph, blame, diff and merge editors, rebase, stash, branches, tags), password prompts for git and ssh, and the Remote tab (ssh, sftp, ftp, ftps, smb).
