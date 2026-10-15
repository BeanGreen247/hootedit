# Changelog

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
