# Why Hootedit, and how to check the claims yourself

Hootedit is for people who notice latency: it is a native C++ editor that draws itself with OpenGL, keeps the text in a piece table, has no extension host and no embedded browser, and does its own checking of code in-process. This page says what that buys, what published data says about other editors, and how to measure all of it on your own machine with the tools in this repository.

## What matters to a performance-sensitive developer, and where the time goes

- **Key to pixel.** The delay between a key press and the changed pixels. Research and the editor benchmarks below agree that it should be one display frame or less; editors that run an extension host, a layout engine or a garbage-collected UI layer add jitter on top of the average.
- **Open a big file.** Log files, generated code, data dumps. The editor should open it without a progress bar and stay responsive.
- **Memory and idle CPU.** An editor that sits at a few percent CPU or gigabytes of memory while you read is costing you a laptop battery and the rest of your tools.
- **Work that should not need a second process.** Searching, finding and replacing, bracket matching, syntax colour, and checking for mistakes are text problems; a language-server round trip is a good tool for semantics (types, references) but not for "this line has trailing whitespace" or "this task has no name".

## What the published numbers say (and how far to trust them)

| Source | What it measured | Result |
|---|---|---|
| Pavel Fatin, "Typing with pleasure" (2015), Typometer, Linux, XML file | key to pixel, ms average (max) | GVim 4.5 (8.4), Gedit 12.4, Emacs 20.3, Sublime Text 23.1 (35.4), Atom 32.5, Eclipse 46.5, IntelliJ IDEA 69.2 (198.0), IDEA with its zero-latency mode 1.7 (3.9) |
| Peeknext (formerly Koi) benchmarks, 2026, macOS, Mac mini M2 Pro, 60 Hz, Keypress.sh, sqlite3.c | key to pixel, P95 ms | TextMate 16.1, Vim 17.5, Sublime Text 30.0, VS Code 35.3, Zed 38.2, Neovim 52.3, Emacs 85.2 |
| same, large files | P95 typing latency and memory | at 1M lines: Vim 18.2 ms, Sublime Text 31.6 ms, VS Code 50.6 ms (1.6 GB for 1M lines, 207 ms at 5M lines); Zed used 1.8 GB at 1M lines and failed at 10M; only the vendor's own editor passed 20M lines |
| zed.dev comparison page, 2026 | vendor claims | Zed: under 1 s start, under 10 ms typing, about 600 MB; VS Code: 3-5 s start with extensions, 3-4 GB |

Read these carefully. The 2015 data is old and Windows/Linux. The 2026 table is published by an editor vendor that wins most of its own rows, on macOS and a 60 Hz display, so results cluster at multiples of 16.7 ms. The Zed page is a vendor claim, and the independent table above shows Zed at 38 ms P95 in one test, not under 10 ms. None of these are Hootedit numbers, and none were measured on the machine you are about to buy. That is why the tools below exist.

Sources: pavelfatin.com/typing-with-pleasure, peeknext.com/benchmarks (methodology: Keypress.sh, 200 keypresses per test), zed.dev/compare/vscode, and the ansible-lint rule list at docs.ansible.com/projects/lint/rules (the basis of the Ansible checks).

## Measure it yourself

All three tools are in the repository and need only a C++ compiler, `Xvfb`, and `xdotool`.

- `tools/xlatency.c`: key-to-pixel latency of any X11 editor, the Linux counterpart of Typometer and Keypress.sh. It sends a key through XTEST and polls the screen with shared-memory `XGetImage` until the pixels change. Also `-P` mode: how long until a window is painted.
- `scripts/compare-editors.sh [--editors "hootedit code code-yours subl zeditor kate gedit"] [--big FILE]`: runs each installed editor on its own fresh Xvfb display and prints a markdown table: window painted, memory (PSS of the whole process tree), idle CPU, and typing latency, on an empty file, a 40 MB file and a generated Ansible role. `code-yours` is your own VS Code profile copied into a throwaway directory. Editors that are not installed are skipped.
- `hootedit --bench-core FILE [REPS] [DIR]`: the CPU paths of Hootedit without a window (open, lexer, regex scan, fuzzy filter, outline, live checks, edits, undo, workspace search).

Method notes: every editor gets the same display size, window size and file, and the same window manager (Marco, needed because Electron ignores keys until a window manager marks its window active); software rendering and no compositor, so the numbers compare editors with each other on this machine, they are not key-to-photon figures. Run it a few times and look at the spread, not one number.

## Measured here

Machine: 2 cores, Debian 13 (kernel 6.12), Xvfb at 1280x1024 with the Marco window manager, software rendering (no GPU), window 1024x768, 60 samples at 100 ms spacing, VS Code 1.138.0 started with `--disable-gpu`. "VS Code" is a clean profile with no extensions. "your VS Code" is a copy of the author's own profile (settings.json and all 25 installed extensions, among them the Red Hat Ansible and YAML language servers, Error Lens, Python, markdownlint and the Remote-SSH family), copied into a throwaway directory so the real profile is never opened; two first-run settings were added to both VS Code rows only to stop a sign-in dialog from covering the editor. Memory is PSS summed over the whole process tree, measured 10 s after the window appeared; idle CPU is the next 5 s.

| editor | file | window appears / shows content | memory | idle CPU | typing, key to pixels (median / p95) |
|---|---|---|---|---|---|
| Hootedit | empty | 0.34 s / 0.35 s | 67 MB | 0.2% | 7.1 / 9.9 ms |
| Hootedit | 40 MB C file | 0.26 s / 0.26 s | 113 MB | 0.2% | 8.8 / 11.1 ms |
| Hootedit | Ansible role, 300 tasks | 0.30 s / 0.30 s | 68 MB | 0.2% | 7.5 / 11.6 ms |
| VS Code, clean | empty | 0.71 s / 2.34 s | 712 MB | 1.0% | 16.4 / 405 ms |
| VS Code, clean | 40 MB C file | 0.65 s / 2.29 s | 1135 MB | 1.0% | 24.2 / 400 ms |
| VS Code, clean | Ansible role, 300 tasks | 0.66 s / 2.50 s | 741 MB | 0.8% | 14.7 / 400 ms |
| your VS Code | empty | 0.64 s / 2.37 s | 920 MB | 3.6% | 21.2 / 401 ms |
| your VS Code | 40 MB C file | 0.73 s / 2.39 s | 1417 MB | 3.6% | 33.3 / 400 ms |
| your VS Code | Ansible role, 300 tasks | 0.67 s / 2.25 s | 932 MB | 2.8% | 60.9 / 320 ms |

The first time column is when a window of the editor first appears on the screen, the second when it first has content in it (more than 40 distinct colours, which a blank window never has). For VS Code the window is on screen for 1.6 to 1.8 seconds with nothing in it; Hootedit keeps its window hidden until the first frame is drawn, so the two times are within a few milliseconds of each other. All times are measured from the launch, include the window manager's work and are slower than they would be on a GPU.

Checking the same 300-task role for mistakes: `ansible-lint --offline` took 8.9 to 10.1 s on this machine (three runs); Hootedit's built-in check took 0.8 ms (`hootedit --bench-core`, best of 5, 30 KB file). The two do not cover the same rules (see below), but the cheap ones are the ones you need while typing.

What to take from it, and what not:

- Start-up, memory and idle CPU are large, repeatable differences. Hootedit uses about a tenth of the memory and has content on screen in about a seventh of the time, and its window never shows empty. In this run your VS Code setup used 2.8% CPU ten seconds after opening the Ansible role (an earlier run caught the Ansible language server using a full core while it analysed the file).
- Typing latency is closer for small files and diverges on large ones. The medians are the trustworthy part. The p95 column for VS Code contains a repeated value of almost exactly 400 ms that looks like a timer in the editor or the measurement, not typing delay; I could not attribute it, so do not read it as a measured p95. (An earlier version of this table showed Hootedit taking 0.44 to 0.64 s to start; that was my script sleeping half a second before it started measuring, now fixed.)
- This machine renders in software. That hurts every editor, and Electron more than a native OpenGL program would be hurt on a GPU; VS Code on a real GPU will do better than `--disable-gpu`. An earlier run without a window manager gave Hootedit a median of 6.9 ms on the empty file; the window manager adds its own work. Treat the numbers as "same conditions for everyone on this box", and rerun the script on your hardware.
- Zed, Sublime Text and others are not installed here, so they are not in the table; `scripts/compare-editors.sh --editors "hootedit code subl zeditor"` adds any that are.
- Hootedit's Ansible check is a subset of ansible-lint: the rules that can be decided from the text. Rules that need the Ansible runtime (module argument validation, role dependency checks) still need ansible-lint, which is why it is slower.

## What Hootedit adds for people who write Ansible

Ansible playbooks are YAML where the common mistakes are mechanical: a task without a name, a module without its full collection name, a `mode: 644` that YAML reads as decimal, a `shell:` where `command:` would do, `command:` tasks that report "changed" on every run, a `when: "{{ x }}"`. ansible-lint catches these from the command line, usually after you have saved, committed and waited for CI. Hootedit shows them as you type, offers the fix under Ctrl+. (or all of them at once), and has 28 task skeletons that start out correct. It does this with a text pass of about 2 ms for a 100 KB playbook and no process to start. It does not replace ansible-lint (rules that need the Ansible runtime, such as module argument validation, still need it); it moves the cheap, certain rules to the moment you type.

## What it does not do yet

No language server client (completion, go to definition, rename), no debugger, no split panes. Those are next; the checks above share their diagnostics model, so a language server's problems will show up in the same places.
