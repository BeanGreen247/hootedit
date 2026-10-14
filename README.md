# Hootedit

<p align="center">
  <img src="data/hootedit.png" alt="Hootedit logo" width="96"/>
</p>

A fast native code editor in C++. No Electron, no GTK, no web view. Everything in the text path is our own code: a piece-table buffer, a chunked line index, a table-driven syntax engine, an own regular-expression engine and fuzzy matcher for search, and the glyph-atlas OpenGL renderer and terminal engine from [Lestrix](https://github.com/BeanGreen247/lestrix) (vendored in `src/engine/` and `src/lestrix/`, from Lestrix commit 0289c8e). Dependencies: SDL2, FreeType, fontconfig, OpenGL.

Look: dark and quiet, in the style of Lestrix. A menu bar, a command box with the language picker, a rail with the pin and rotated tabs, a tab row, a plain status bar. No minimap, no breadcrumbs, no animations, no welcome page; an empty window shows the name, not a logo.

## Install

```
./install.sh                    # asks: system-wide (/usr/local) or just for you (~/.local)
./install.sh --system           # /usr/local, sudo password asked once
./install.sh --user             # ~/.local, no sudo for the files
./install.sh --prefix DIR       # anywhere else
./install.sh --tests            # run the test suite before installing
./install.sh --dry-run          # show the steps, change nothing
./install.sh --uninstall        # same as ./uninstall.sh
./uninstall.sh                  # removes every copy found in /usr/local and ~/.local (--system, --user or --prefix DIR to pick one)
```

Run it as your normal user, not with sudo. Without a flag in a terminal it asks where to install; without a terminal it defaults to system-wide, the same as Lestrix. Missing build packages are installed through apt, dnf, pacman or zypper (`--no-deps` skips that). The program, a desktop entry and the icon are installed; uninstall removes only those files and never touches your files or settings.

Run: `hootedit [FILE|FOLDER...]`. Options: `--theme NAME`, `--cache-stats` (print the statistics and the operation timings at exit), `--size WxH`, `--version`.

## Sidebar

A narrow rail on the left holds the **pin** button and three rotated tabs. Pinned, the sidebar is a column of its own; unpinned, it floats over the editor only when asked for (pointer at the left window edge, Ctrl+B, or a rail click) and leaves when the pointer moves away, when you click the editor, or on Esc. Pin state, tab, theme and terminal choice are remembered in `~/.config/hootedit/settings.ini`.

- **FILES**: the workspace tree (opened lazily, one folder at a time; `.git`, `node_modules`, `vendor` and `dist` hidden). A green dot marks a file open in a tab, orange one with unsaved edits. The box filters every file by fuzzy match (below); **+** makes a new file.
- **SEARCH**: search the text of every file in the workspace (below).
- **GIT**: the branch, then STAGED, CHANGES and UNTRACKED with a coloured dot each. The tab is always there. When the workspace folder is not a repository it follows the repository of the file in the active tab, and when there is none it says so. Clicking a change opens its diff in a tab (Ctrl+click opens the file). It runs the real `git`; nothing is reimplemented.

## Tabs

With no file open and nothing typed, the window shows the Hootedit start screen and no tab strip. The start screen has the owl logo as a faint watermark above the name, tinted from the active theme. It is drawn from vector shapes at run time (`src/icons.cpp`, about 6 ms at 1x and 28 ms on a 2x display, once per size), with no image file. A double click on the start screen, or on the strip, opens an empty tab. Once tabs exist, a double click on the empty space to the right of the last one opens another empty tab, and Ctrl+N does the same. Closing the last tab brings the start screen back. Line numbers are centered in the gutter, and nothing under the status bar can be clicked.

Tabs keep their natural width and the strip scrolls when they do not fit. The mouse wheel over the strip slides it without changing the active tab; switching tabs (click, Ctrl+Tab, Alt+1..9) scrolls just enough to show the active one. Drag a tab to reorder it (the strip follows the pointer near its edges), or press Ctrl+Shift+PgUp / Ctrl+Shift+PgDn to move the active tab. Ctrl+Shift+T brings back the last closed tab.

`hootedit FOLDER` opens a folder; `hootedit FILE` uses the file's repository (or its folder) as the workspace.

## Source control

The Git tab follows VS Code's Source Control view, with the colours taken from VS Code itself (the git extension's file status colours, the Dark and Light Modern gutter colours, the diff editor's line and word backgrounds). Everything runs through the `git` command line; nothing is reimplemented.

- A badge on the GIT rail tab shows how many files changed. The view shows the branch with ahead and behind counts, a summary (modified, added, deleted, untracked), a commit box and button (Ctrl+Enter commits; with nothing staged it stages everything first), **Staged Changes** and **Changes** with a count each, and per file a coloured status letter (M, A, D, U, !). Hover a row for open file and stage / unstage buttons; the + and - on a section header stage or unstage everything.
- **GRAPH** lists the branch history with branch and tag labels. Click a commit to see its files, click a file for its diff. Ctrl+click marks a commit, Shift+click another one lists the files that differ between the two (the marked one on the left).
- **TIMELINE** lists the commits that touched the file in the active tab. Click one for that file's diff against its parent, or Ctrl+click one revision and Shift+click another to compare the two.
- Diff tabs show side by side when the editor is 900 px or wider and inline below that (button or `i` to switch), word level highlights inside changed lines, hatched filler where a side has no line, and unchanged stretches folded (click a fold, `e` expands all). F7 / Shift+F7 or the arrow buttons jump between changes; the scrollbar marks them. Enter or Open File opens the real file.
- In the editor, bars next to the line numbers mark lines added (green), changed (blue) and removed (a red wedge) against HEAD; click one to peek at the change: a panel under the lines shows the HEAD lines in red and yours in green, with buttons to revert the change in the buffer (Ctrl+Z undoes it), open the full diff, and step to the previous or next change (Esc or the X closes it). The status bar shows who last changed the cursor line and when (click it for that commit's diff), and the same text is shown faintly after the line.

Acting on changes works like VS Code. The **...** menu has Pull, Push, Fetch, Sync, Checkout to..., and submenus for Commit (Amend, Signed Off, Commit and Push, Undo Last Commit), Changes (Stage, Unstage or Discard All), Branch (Merge, Create, Create From, Rename, Delete, Publish), Stash and Tags, plus View as Tree or List. The arrow on the Commit button opens the same commit variants; with nothing to commit the button becomes **Sync Changes** or **Publish Branch**. Committing with nothing staged asks whether to stage everything (Yes, Always, Never). **Discard Changes** (the undo button on a row or on the Changes header, or right-click) asks first, in VS Code's words: "Are you sure you want to discard changes in 'x'?", "Are you sure you want to DELETE 'x'?" for an untracked file, and for several files "Are you sure you want to discard ALL changes in N files?" with **Discard All N Files**, **Discard Tracked Changes** and Cancel. The Pull submenu has Pull, Pull (Rebase) and Pull from..., Push has Push and Push to..., the Branch submenu has Rebase Branch..., and the Remote submenu adds or removes remotes and fetches from all of them. While a merge, rebase, cherry-pick or revert is unfinished, the view shows a banner with Continue, Skip (rebase and cherry-pick) and Abort. Files with conflicts get their own **Merge Changes** section (click one to open it, the + stages it as resolved). In the file, the conflict blocks are tinted (current green, incoming blue) and the `<<<<<<<` line carries **Accept Current**, **Accept Incoming** and **Accept Both**; each is one undo step. Checking out a branch that would overwrite local changes offers Stash & Checkout or Force Checkout. Push, pull and fetch run in the background. In a diff tab, **Stage Change**, **Unstage Change** and **Revert Change** act on the current change (F7 to pick it). Hovering a change in a diff tab shows the same buttons (stage and revert, or unstage) on its first row, so you can act on it without selecting it first. Right-click a file, a commit, a section or a tab for context menus.

## Password prompts

git and ssh ask for credentials through a dialog inside the editor instead of a terminal: HTTPS user names and passwords or tokens, ssh key passphrases, and "trust this host key?" questions. The secret goes from the dialog to git or ssh through a private unix socket and is never written to disk or put on a command line. Whether git remembers it is up to your git credential helper.

## Remote

The **REMOTE** tab in the sidebar keeps connections to servers over **SSH**, **SFTP**, **FTP**, **FTPS** and **SMB**. Press **+**, pick the type (for SSH and SFTP the hosts of `~/.ssh/config` are offered first; ssh then applies that entry's user, port and key), enter `user@host[:port]` and a folder. Click the connection to connect, browse folders in the tree, and click a file to download it into a tab. Saving the tab uploads the file again. Right-click a folder (or the connection) and choose Search to grep the folder on the server over ssh (ssh and sftp connections; the matches open at their line). Right-click a folder for New File, New Folder, Rename, Delete (with the contents, after a confirmation) and Refresh, a file for Rename and Delete, or the connection for New File and New Folder in its root; changes happen on the server at once and the tree refreshes. Right-click a connection to reconnect, disconnect, open an SSH terminal in the built-in terminal, edit `~/.config/hootedit/remotes.ini` (add `key=/path/to/identity` for a specific ssh key) or remove it. ssh and sftp keep one authenticated connection open, so the password or passphrase is asked once. ftp and smb ask for the password in a dialog and keep it in memory for the session only; passwords are never stored. The work is done by the command line clients (`ssh`, `sftp`, `curl`, `smbclient`), which must be installed. FTPS uses explicit TLS (`AUTH TLS`, the usual setup; implicit TLS only on port 990) and checks the server certificate; for a self-signed one add `insecure=1` to the connection in `remotes.ini`. Credentials for curl and smbclient go through a private 0600 file in `$XDG_RUNTIME_DIR` that is removed right after the call, never on a command line. The SSH and SFTP paths are tested against a local sshd, FTP and FTPS against a local pyftpdlib server (browse, open, save); New file, folder, rename and delete are tested over ssh, sftp and ftp; SMB is only tested against a stand-in `smbclient` (its new/rename/delete commands not at all), not a real Samba share.

## Search

Everything here is our own code, built for speed.

**Text search** (the SEARCH tab, Ctrl+Shift+F). Type and it searches as you type, in the background, so typing never waits. Results are grouped by file with the match highlighted; Up/Down and Enter, or a click, jump to the match. The box has three toggles: **Aa** match case, **.\*** regular expression, **ab** whole word (Alt+C, Alt+R, Alt+W). Smart case: a lower-case query ignores case, one capital letter makes it exact.

- **Regex engine** (`src/regex.cpp`): no backtracking, so no pattern can hang the editor. Literals, `.`, classes, `\d \w \s \b`, `^ $`, groups, alternation, `* + ? {n,m}` with lazy forms, `(?i)`; UTF-8 aware (`.` is one character). Leftmost-first like Perl. A lazy DFA decides which lines match at about one table lookup per byte, a Pike VM finds the exact span only on lines that match, and a prefilter (a plain-string search, a SIMD case-insensitive one, or a required-literal search) skips straight to candidate lines. Patterns with `\b` use a relaxed DFA to pick lines, then the exact check. It is tested against `std::regex` on 180,000 random pattern/text pairs.
- **Workspace scan** (`src/search.cpp`): parallel over files; the file list comes from `git ls-files` (so `.gitignore` is respected) or a directory walk that never follows symlinks; binary files are skipped; a new query cancels the old one without waiting.
- **Measured against ripgrep and grep** on `/usr/include` (26,133 files, 347 MB, 2 cores, warm cache, best of 5, same hit counts; a loaded VM, so treat as indicative):

| pattern | hootedit | ripgrep | grep |
|---|---|---|---|
| `static_assert` | 126 ms | 167 ms | 259 ms |
| `ERRNO` (ignore case) | 120 ms | 191 ms | 253 ms |
| `[A-Z_]+_MAX\b` | 203 ms | 168 ms | 308 ms |
| `\w+_t\s*\(` | 289 ms | 295 ms | 946 ms |
| `\b(EAGAIN\|EINTR)\b` | 253 ms | 289 ms | 231 ms |
| `(errno\|EAGAIN)` (ignore case) | 399 ms | 175 ms | 243 ms |

  So it is ahead of ripgrep on plain and case-insensitive strings, level on several regexes, and **slower on a case-insensitive alternation** and slightly on `[A-Z_]+_MAX\b`; a multi-string prefilter for alternations is the next thing to build. There is no index: at these scan speeds an index would cost memory and start-up time to save a fraction of a second. Reproduce: `build/bench_search ROOT PATTERN [regex] [i]`.

**Fuzzy file finder** (Ctrl+P, and the box in the FILES tab), in the style of fzf: every character must appear in order; the score rewards matches at word starts, after `/ _ - .`, at camelCase humps and right after the previous match, and penalises gaps. Smart case. Matched characters are highlighted. fzf's extended syntax works: space separates terms that must all match, `'exact`, `^prefix`, `suffix$`, `!not`. It ranks 200,000 paths in about 30 ms on one thread.

**Find in file** (Ctrl+F, the command box): the same regex engine with **Aa** and **.\*** toggles (Alt+C, Alt+R); Enter jumps to the next match.

## Terminal panel

Ctrl+` toggles a terminal panel under the editor, like VS Code: a header with one chip per terminal and the buttons **+** (new), **x** (kill) and a chevron (hide); drag the top edge to resize (the height is remembered). The panel is the Lestrix terminal engine, so it is the same code that parses at hundreds of MB/s there, with its built-in fast `cat` (plain `cat file` and `producer | cat` run at memory speed), scrollback up to 10,000 lines in at most 64 MB, mouse reporting, selection and the full colour palette of the active theme. It starts your `$SHELL` in the workspace folder; a shell that exits closes its tab.

| Keys | |
|---|---|
| Ctrl+` | show / focus / hide the panel |
| Ctrl+Shift+` | new terminal |
| Ctrl+Shift+C, Ctrl+Shift+V | copy, paste (also middle click) |
| Ctrl+PgUp, Ctrl+PgDn | previous / next terminal |
| click the editor | focus goes back to the editor |

While the terminal has focus, every other key goes to the shell, so Ctrl+B, Ctrl+N and friends keep their terminal meaning.

**Choosing the terminal**: Terminal > Choose Terminal lists the built-in panel plus every emulator found on the system (the system default `x-terminal-emulator`, gnome-terminal, konsole, xfce4-terminal, mate-terminal, tilix, terminator, kitty, alacritty, foot, wezterm, Lestrix, xterm, `$TERMINAL`). Pick an external one and Ctrl+` and "new terminal" open that emulator in its own window, in the workspace folder; Terminal > Open External Terminal Here does it once whatever the choice. The default is the built-in panel.

## Editor details

What makes the text area read as an editor rather than a text dump, in the manner of Sublime Text and friends: a faint band behind the **cursor line** (it also runs under the line numbers), **indent guides** (a faint vertical line at every indent level, brighter for the block the cursor is in; the width follows the file's indentation), the **bracket pair** next to the cursor highlighted, every **other occurrence of a selected word** marked, **all matches of the find box** marked while it is open, and a thin **scrollbar** on the right with a tick for the cursor line (drag it, or click the track). Bracket matching counts brackets and does not skip strings or comments.

## File dialogs

Open File, Open Folder and Save As are one file browser: the list of the current folder (folders first), `..` to go up, the buttons Home, Workspace and `/`, and a box that fuzzy-filters the list as you type. Type a path that starts with `/` or `~` to jump there (a trailing `/` opens that folder). Keys: Up/Down/PgUp/PgDn, Enter (enter a folder or open a file), Backspace on an empty box goes up a level, Ctrl+H shows hidden files, Ctrl+Enter or the main button accepts, Esc cancels; double-click works too. Open Folder lists folders only and starts beside the current workspace with it highlighted; the **Open Folder** button opens the highlighted folder, or the one you are in if you chose nothing. Save As has a Name field (Tab switches between the name and the list) and asks before replacing an existing file.

## Themes

The same themes as Lestrix: Xylonic Dark and Light, Graphite, Nord, Gruvbox, Solarized Dark, and the fleetwm set (Dark, Catppuccin, Dracula, OLED Black, Light). View > Color Theme (Ctrl+Shift+T) or `--theme NAME`. "Follow fleetwm theme" tracks the fleetwm config live, the way Lestrix does (an inotify watch, no polling). The theme colours the whole UI, the syntax highlighting (from the theme's ANSI palette) and the terminal.

## Shortcuts

| Keys | Action |
|---|---|
| Ctrl+N, or double click the empty tab strip | New empty tab |
| Ctrl+W | Close tab (asks Save / Don't save / Cancel if it has unsaved edits) |
| Ctrl+O | Open a file (file browser) |
| Ctrl+P | Quick open (fuzzy) |
| Ctrl+S, Ctrl+Shift+S | Save, Save As (an untitled file opens the Save As browser) |
| Ctrl+Q | Quit (asks about every modified tab) |
| Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgDn, Ctrl+PgUp | Next / previous tab |
| Ctrl+B | Show or hide the sidebar |
| Ctrl+Shift+E, Ctrl+Shift+F, Ctrl+Shift+G | Files, Search, Git tab |
| Ctrl+` , Ctrl+Shift+` | Terminal panel, new terminal |
| Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z | Undo, redo |
| Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V | Select all, copy, cut, paste |
| Shift + arrows / Home / End / PgUp / PgDn, mouse drag | Select |
| Ctrl+F | Find in file (Alt+R regex, Alt+C case, Enter next, Esc leave) |
| Ctrl+K M, or click the language name | Language picker (type to filter; "Auto Detect" first) |
| Ctrl+K Ctrl+T | Color theme picker |
| Ctrl+K Ctrl+O, Ctrl+K Ctrl+W, Ctrl+K Ctrl+S | Open folder, close all tabs without unsaved edits, save all |
| Alt+1 .. Alt+9, Alt+0 | Go to tab N, to the last tab |
| Ctrl+K B | Show or hide the faint blame text after the cursor line |
| F7, Shift+F7 | Next / previous change in a diff tab |
| Ctrl+Shift+T | Reopen the last closed tab |
| Ctrl+Shift+PgUp, Ctrl+Shift+PgDn | Move the tab left / right |
| Ctrl+G | Go to line (also `:42` in Ctrl+P) |
| Ctrl+Left / Right, Ctrl+Backspace / Delete | Move by word, delete a word |
| Alt+Up / Down, Shift+Alt+Up / Down | Move the line(s), copy the line(s) |
| Ctrl+Shift+K, Ctrl+L | Delete the line, select the line |
| Ctrl+Enter, Ctrl+Shift+Enter | New line below, above |
| Ctrl+/ | Toggle a line comment (by language) |
| Ctrl+], Ctrl+[, Tab / Shift+Tab over several lines | Indent, outdent |
| Ctrl+D | Select the word, then the next occurrence |
| F3 | Next find match |
| Ctrl+Shift+I | Statistics panel: caches and the latest operation timings |
| Mouse wheel | Scroll (over the sidebar it scrolls the list) |

The menu bar (File, Edit, View, Terminal, Help) offers the same actions with their shortcuts. Saving writes a temp file next to the target, calls fsync, then renames it over the original, so a crash or full disk never leaves a half-written file.

## Syntax highlighting

Own engine, no regex and no backtracking: each line is lexed in one forward pass from a per-language table (keywords, comment markers, quote rules). State that crosses lines (block comments, triple-quoted strings, fenced code, markup tags) is cached per line and recomputed only from the edited line. Languages and file types: C, C++, Python, JavaScript, TypeScript, Go, Rust, Java, C#, Kotlin, Swift, Shell, Perl, Ruby, PHP, Lua, SQL, HTML, XML, CSS, JSON, YAML, TOML, INI, Markdown, Makefile, Dockerfile, Jinja, Diff, CMake, Assembly, plus Plain Text. The language is detected from the file name or a `#!` line and can be overridden from the picker.

## Performance

Every number below was measured on the development machine with a command in this repository, unless it says otherwise. They are examples of the method, not promises for your hardware. The development machine is a 2-core VM with **software OpenGL** (llvmpipe, under `xvfb-run`) that other programs were also using, so GPU time and keystroke-to-screen latency on real hardware are **not** claimed.

### How long things take

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

### Other numbers

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

## Tests

`meson test -C build` runs: the buffer against a plain string model (random edits, ASan and UBSan), the syntax lexer (expected token classes, language detection), undo/redo and save tracking, the regex engine against `std::regex` on 180,000 random cases plus UTF-8 and scan-versus-find checks (ASan and UBSan), and the fuzzy matcher (ranking and extended syntax). The editor can be driven without a keyboard for screenshots and timing: `hootedit --script "ctrl+n;text:hi;act:search;click:120,150;wait:2000" --shot out.ppm` under `xvfb-run` (steps: key combos, `text:`, `act:NAME`, `menu:N`, `open:PATH`, `mouse:x,y`, `click:x,y`, `wait:MS`, `frame`; add `--timed` to draw a frame per step). The minimum supported window is 1024x768 and is checked.

## Status

v0.4.0. Built: editing with undo and selection, tabs, the three-tab sidebar with pin, workspace search with own regex and fuzzy engines, the terminal panel and terminal choice, Lestrix themes with fleetwm following, git status and diff. Not built yet: git stage and commit, replace, LSP, remote editing over SSH/SFTP/FTP/SMB (safe over fast), settings sync through a private git repository, a multi-string prefilter for case-insensitive alternations in search, and a Windows/macOS port.
