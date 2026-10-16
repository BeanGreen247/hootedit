# Sidebar, Explorer and tabs

## Sidebar

A narrow rail on the left holds the **pin** button and three rotated tabs. Pinned, the sidebar is a column of its own; unpinned, it floats over the editor only when asked for (pointer at the left window edge, Ctrl+B, or a rail click) and leaves when the pointer moves away, when you click the editor, or on Esc. Pin state, tab, theme and terminal choice are remembered in `~/.config/hootedit/settings.ini`.

- **FILES**: the workspace tree (opened lazily, one folder at a time; `.git`, `node_modules`, `vendor` and `dist` hidden). A green dot marks a file open in a tab, orange one with unsaved edits. The box filters every file by fuzzy match (below); **+** makes a new file.
- **SEARCH**: search the text of every file in the workspace ([Search](search.md)).
- **GIT**: the branch, then STAGED, CHANGES and UNTRACKED with a coloured dot each. The tab is always there. When the workspace folder is not a repository it follows the repository of the file in the active tab, and when there is none it says so. Clicking a change opens its diff in a tab (Ctrl+click opens the file). It runs the real `git`; nothing is reimplemented.

## Explorer, preview tabs, outline

- Single click on a file opens an italic preview tab that the next single click replaces; double click, or typing, keeps it.
- OPEN EDITORS lists every tab (unsaved dot, close button). OUTLINE (Settings > Outline, off by default) lists the types, functions and markdown headings of the active file; click to jump.
- Single-child folders show as one row (`a/b/c`), and the parents of the first visible row stay pinned while you scroll.
- In a git project, changed files are coloured with a status letter (M, A, U, !), folders get a dot, and files deleted from disk stay listed, struck through with a red D; clicking one opens the diff.
- Help > Keyboard Shortcuts (Ctrl+K Ctrl+H) lists every shortcut in one filterable list.

## Tabs

With no file open and nothing typed, the window shows the Hootedit start screen and no tab strip. The start screen has the owl logo as a faint watermark above the name, tinted from the active theme. It is drawn from vector shapes at run time (`src/icons.cpp`, about 6 ms at 1x and 28 ms on a 2x display, once per size), with no image file. A double click on the start screen, or on the strip, opens an empty tab. Once tabs exist, a double click on the empty space to the right of the last one opens another empty tab, and Ctrl+N does the same. Closing the last tab brings the start screen back. Line numbers are centered in the gutter, and nothing under the status bar can be clicked.

Tabs keep their natural width and the strip scrolls when they do not fit. The mouse wheel over the strip slides it without changing the active tab; switching tabs (click, Ctrl+Tab, Alt+1..9) scrolls just enough to show the active one. Drag a tab to reorder it (the strip follows the pointer near its edges), or press Ctrl+Shift+PgUp / Ctrl+Shift+PgDn to move the active tab. Ctrl+Shift+T brings back the last closed tab.

`hootedit FOLDER` opens a folder; `hootedit FILE` uses the file's repository (or its folder) as the workspace.
