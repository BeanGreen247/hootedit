# Changelog

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
