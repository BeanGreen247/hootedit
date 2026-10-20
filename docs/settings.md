# Settings

The cog at the top right (always there) or at the foot of the sidebar rail, File > Settings, Ctrl+, (or "Open Settings" in the command palette) all open one page for everything that can be changed. Categories are on the left, a search box on top (type `vim`, `git`, `theme`; every word must match the name or the description), Up/Down move, Enter or Space changes the setting, Left/Right switch category, Esc clears the search and then closes the page. Everything is stored in `settings.ini` and applies at once.

| category | settings |
|---|---|
| Appearance | colour theme, follow the fleetwm theme, timing in the status bar, CPU, memory and cache statistics in the status bar |
| Editor | vim keys, live checks, ignored check codes, preview tabs, reload files changed on disk |
| Explorer | outline, pin the sidebar, compact folders |
| Source Control | what to do when committing with nothing staged, ask before Sync Changes, show changes as a tree |
| Terminal | built-in panel or an external terminal |
| Files | open `settings.ini`, open `remotes.ini`, reset everything |

The toggles that used to live in the View menu (Outline, Vim Keys, Live Checks) are here now, and still in the command palette as "Settings: Toggle ...". They were in View only because there was no settings page.

The top right of the window has two buttons, like VS Code: the sidebar toggle (Ctrl+B) and the settings cog. The toggle pins the sidebar; pressed again it unpins and hides it, and it then slides in when the pointer touches the left edge (10 px) and leaves again when the pointer does. The toggle pins the sidebar; pressed again it unpins and hides it, and it then slides in when the pointer touches the left edge (10 px) and leaves again when the pointer does, so Settings stays reachable when the sidebar is closed.
