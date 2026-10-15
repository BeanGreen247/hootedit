# Terminal panel

`` Ctrl+` `` toggles a terminal panel under the editor, like VS Code: a header with one chip per terminal and the buttons **+** (new), **x** (kill) and a chevron (hide); drag the top edge to resize (the height is remembered). The panel is the Lestrix terminal engine, so it is the same code that parses at hundreds of MB/s there, with its built-in fast `cat` (plain `cat file` and `producer | cat` run at memory speed), scrollback up to 10,000 lines in at most 64 MB, mouse reporting, selection and the full colour palette of the active theme. It starts your `$SHELL` in the workspace folder; a shell that exits closes its tab.

| Keys | |
|---|---|
| `` Ctrl+` `` | show / focus / hide the panel |
| `` Ctrl+Shift+` `` | new terminal |
| Ctrl+Shift+C, Ctrl+Shift+V | copy, paste (also middle click) |
| Ctrl+PgUp, Ctrl+PgDn | previous / next terminal |
| click the editor | focus goes back to the editor |

While the terminal has focus, every other key goes to the shell, so Ctrl+B, Ctrl+N and friends keep their terminal meaning.

**Choosing the terminal**: Terminal > Choose Terminal lists the built-in panel plus every emulator found on the system (the system default `x-terminal-emulator`, gnome-terminal, konsole, xfce4-terminal, mate-terminal, tilix, terminator, kitty, alacritty, foot, wezterm, Lestrix, xterm, `$TERMINAL`). Pick an external one and `` Ctrl+` `` and "new terminal" open that emulator in its own window, in the workspace folder; Terminal > Open External Terminal Here does it once whatever the choice. The default is the built-in panel.
