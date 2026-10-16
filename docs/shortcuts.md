# Shortcuts

| Keys | Action |
|---|---|
| Ctrl+N, or double click the empty tab strip | New empty tab |
| Ctrl+W | Close tab (asks Save / Don't save / Cancel if it has unsaved edits) |
| Ctrl+O | Open a file (file browser) |
| Ctrl+P | Quick open (fuzzy) |
| Ctrl+S, Ctrl+Shift+S | Save, Save As (an untitled file opens the Save As browser) |
| Ctrl+Q | Quit (asks about every modified tab) |
| Ctrl+Tab, Ctrl+Shift+Tab | Switch to the previously used tab; keep Ctrl down and press again to walk back through the tabs in order of use |
| Ctrl+PgDn, Ctrl+PgUp | Next / previous tab in tab order |
| Ctrl+Shift+P, F1 | Command palette: every menu command and the git and remote actions; every typed word must appear in the name |
| Ctrl+B | Show or hide the sidebar |
| Ctrl+Shift+E, Ctrl+Shift+F, Ctrl+Shift+G | Files, Search, Git tab |
| `` Ctrl+` `` , `` Ctrl+Shift+` `` | Terminal panel, new terminal |
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
| Ctrl+H | Replace: opens a Replace box under the find box (Tab switches boxes; Enter in it replaces the match and moves on; **All** or Ctrl+Alt+Enter replaces everything in one undo step; the replacement is plain text) |
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
