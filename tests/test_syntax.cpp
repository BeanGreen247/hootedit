/* Expected token classes for small samples, state carry across lines, language detection, and a lexer throughput report. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include "../src/syntax.h"

static int fails;
static std::string render(const Lang *l, const char *s, uint8_t in, uint8_t *out) {
    std::vector<uint8_t> t(strlen(s) + 1);
    *out = lex_line(l, s, strlen(s), in, t.data());
    std::string r;
    for (size_t i = 0; i < strlen(s); i++) r += "pcsnkTPoFKHID"[t[i]];   /* plain comment string number keyword Type Preproc punct Func Key Heading Ins Del */
    return r;
}
static void expect(const char *lang, const char *src, const char *want, uint8_t in = 0, int want_state = -1) {
    const Lang *l = nullptr;
    for (int i = 0; i < lang_count(); i++) if (!strcmp(lang_name(lang_at(i)), lang)) l = lang_at(i);
    if (!l) { printf("FAIL no language %s\n", lang); fails++; return; }
    uint8_t out;
    std::string got = render(l, src, in, &out);
    if (got != want || (want_state >= 0 && out != want_state)) { printf("FAIL %s: %s\n  want %s\n  got  %s (state %d)\n", lang, src, want, got.c_str(), out); fails++; }
}
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

int main() {
    expect("C", "int x = 42; // hi", "TTTpppopnnopccccc");
    expect("C", "#include <stdio.h>", "PPPPPPPPpsssssssss", 0);
    expect("C", "/* a", "cccc", 0, 1);
    expect("C", "b */ x", "ccccpp", 1, 0);
    expect("Python", "def f(x): return 'a'", "kkkpFopoopkkkkkkpsss");
    expect("Python", "x = \"\"\"abc", "ppopssssss", 0, 2);
    expect("Python", "abc\"\"\" + 1", "sssssspopn", 2, 0);
    expect("JSON", "{\"a\": 1, \"b\": \"c\"}", "oKKKopnopKKKopssso");
    expect("YAML", "name: value # c", "KKKKopppppppccc");
    expect("Markdown", "# Title", "HHHHHHH");
    expect("Markdown", "```c", "kkkk", 0, 6);
    expect("Markdown", "int x;", "ssssss", 6, 6);
    expect("Diff", "+added", "IIIIII");
    expect("Diff", "-gone", "DDDDD");
    expect("HTML", "<a href=\"x\">t</a>", "kkpTTTTpssskpkkkk");
    if (lang_detect("/x/main.cpp", nullptr, 0) != lang_at(2)) { printf("FAIL detect cpp\n"); fails++; }
    if (strcmp(lang_name(lang_detect("Makefile", nullptr, 0)), "Makefile")) { printf("FAIL detect Makefile\n"); fails++; }
    const char *sb = "#!/usr/bin/env python3\n";
    if (strcmp(lang_name(lang_detect("script", sb, strlen(sb) - 1)), "Python")) { printf("FAIL detect shebang\n"); fails++; }

    /* throughput: 200k lines of C-like text */
    std::string line = "static int compute(const char *s, size_t n) { return n > 42 ? s[0] + 1 : 0; } // note \"x\"";
    const Lang *c = lang_at(1);
    std::vector<uint8_t> tok(line.size());
    uint8_t st = 0;
    double t0 = now();
    const int N = 200000;
    for (int i = 0; i < N; i++) st = lex_line(c, line.data(), line.size(), st, tok.data());
    double dt = now() - t0;
    printf("test_syntax: %s; lexer %.0f MB/s (%.0f ns/line of %zu bytes)\n", fails ? "FAILED" : "all expected classes match", (double)line.size() * N / dt / 1e6, dt / N * 1e9, line.size());
    return fails != 0;
}
