# Search

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
