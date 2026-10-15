# Editing

## Editor details

What makes the text area read as an editor rather than a text dump, in the manner of Sublime Text and friends: a faint band behind the **cursor line** (it also runs under the line numbers), **indent guides** (a faint vertical line at every indent level, brighter for the block the cursor is in; the width follows the file's indentation), the **bracket pair** next to the cursor highlighted, every **other occurrence of a selected word** marked, **all matches of the find box** marked while it is open, and a thin **scrollbar** on the right with a tick for the cursor line (drag it, or click the track). Bracket matching counts brackets and does not skip strings or comments.

## File dialogs

Open File, Open Folder and Save As are one file browser: the list of the current folder (folders first), `..` to go up, the buttons Home, Workspace and `/`, and a box that fuzzy-filters the list as you type. Type a path that starts with `/` or `~` to jump there (a trailing `/` opens that folder). Keys: Up/Down/PgUp/PgDn, Enter (enter a folder or open a file), Backspace on an empty box goes up a level, Ctrl+H shows hidden files, Ctrl+Enter or the main button accepts, Esc cancels; double-click works too. Open Folder lists folders only and starts beside the current workspace with it highlighted; the **Open Folder** button opens the highlighted folder, or the one you are in if you chose nothing. Save As has a Name field (Tab switches between the name and the list) and asks before replacing an existing file.

## Syntax highlighting

Own engine, no regex and no backtracking: each line is lexed in one forward pass from a per-language table (keywords, comment markers, quote rules). State that crosses lines (block comments, triple-quoted strings, fenced code, markup tags) is cached per line and recomputed only from the edited line. Languages and file types: C, C++, Python, JavaScript, TypeScript, Go, Rust, Java, C#, Kotlin, Swift, Shell, Perl, Ruby, PHP, Lua, SQL, HTML, XML, CSS, JSON, YAML, TOML, INI, Markdown, Makefile, Dockerfile, Jinja, Diff, CMake, Assembly, plus Plain Text. The language is detected from the file name or a `#!` line and can be overridden from the picker.

## Themes

The same themes as Lestrix: Xylonic Dark and Light, Graphite, Nord, Gruvbox, Solarized Dark, and the fleetwm set (Dark, Catppuccin, Dracula, OLED Black, Light). View > Color Theme (Ctrl+Shift+T) or `--theme NAME`. "Follow fleetwm theme" tracks the fleetwm config live, the way Lestrix does (an inotify watch, no polling). The theme colours the whole UI, the syntax highlighting (from the theme's ANSI palette) and the terminal.
