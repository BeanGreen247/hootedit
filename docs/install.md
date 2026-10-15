# Install

```
./install.sh                    # asks: system-wide (/usr/local) or just for you (~/.local)
./install.sh --system           # /usr/local, sudo password asked once
./install.sh --user             # ~/.local, no sudo for the files
./install.sh --prefix DIR       # anywhere else
./install.sh --tests            # run the test suite before installing
./install.sh --no-pgo           # skip the profile-guided build (see below)
./install.sh --dry-run          # show the steps, change nothing
./install.sh --uninstall        # same as ./uninstall.sh
./uninstall.sh                  # removes every copy found in /usr/local and ~/.local (--system, --user or --prefix DIR to pick one)
```

Run it as your normal user, not with sudo. Without a flag in a terminal it asks where to install; without a terminal it defaults to system-wide, the same as Lestrix. Missing build packages are installed through apt, dnf, pacman or zypper (`--no-deps` skips that). The program, a desktop entry and the icon are installed; uninstall removes only those files and never touches your files or settings.

The installer builds with profile-guided optimisation: an instrumented build, a short training run under `xvfb-run` (open and scroll a big file, type, undo, find, replace, vim commands, tabs, palette, git view, terminal, plus the headless benchmark), then the final build that uses the profile (`scripts/build-pgo.sh`, about 2.5 minutes). Without `xvfb-run` or a display it falls back to the normal build and says so; if the PGO build fails it also falls back. Measured on a 40 MB file with `hootedit --bench-core FILE [REPS] [DIR]`: about 3 to 6% less total time (edits, undo and workspace search gain the most; the lexer and fuzzy filter are within noise).

Run: `hootedit [FILE|FOLDER...]`. Options: `--theme NAME`, `--cache-stats` (print the statistics and the operation timings at exit), `--size WxH`, `--version`.
