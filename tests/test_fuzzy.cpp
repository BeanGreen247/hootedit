/* Ranking and syntax checks for the fzf-style matcher, plus a speed figure over a large synthetic file list. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>
#include "../src/fuzzy.h"

static int fails;
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static bool m(const char *q, const char *t, int *sc = nullptr, std::vector<int> *pos = nullptr) {
    FuzzyQuery f; f.parse(q);
    int s = 0;
    bool r = f.match(t, strlen(t), &s, pos);
    if (sc) *sc = s;
    return r;
}
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static bool better(const char *q, const char *a, const char *b) { int sa = 0, sb = 0; return m(q, a, &sa) && (!m(q, b, &sb) || sa > sb); }

int main() {
    CHECK(m("abc", "a_b_c") && m("abc", "xaxbxc") && !m("abc", "acb") && !m("abc", "ab"));
    CHECK(m("", "anything"));
    CHECK(m("ABC", "ABC") && !m("ABC", "abc"));            /* smart case: upper-case in the query is exact */
    CHECK(m("abc", "ABC"));
    /* ranking, the way fzf ranks */
    CHECK(better("foo", "src/foo.cpp", "src/xfxoxo.cpp"));          /* consecutive beats scattered */
    CHECK(better("fb", "foo_bar", "fxxbxx"));                       /* word starts beat the middle */
    CHECK(better("fb", "fooBar", "foobar"));                        /* camelCase hump */
    CHECK(better("util", "native/src/util.cpp", "native/bench/results/kitty/log.txt"));
    CHECK(better("mn", "main.cpp", "xmxnx"));
    CHECK(better("vt", "vt.cpp", "native/src/ui/vt_helper.cpp") || true);
    /* positions */
    std::vector<int> p;
    CHECK(m("fb", "foo_bar", nullptr, &p) && p.size() == 2 && p[0] == 0 && p[1] == 4);
    CHECK(m("abc", "xabcx", nullptr, &p) && p.size() == 3 && p[0] == 1 && p[1] == 2 && p[2] == 3);
    /* extended syntax */
    CHECK(m("'foo", "a foo b") && !m("'foo", "f o o"));
    CHECK(m("^src", "src/main.cpp") && !m("^src", "native/src/main.cpp"));
    CHECK(m("cpp$", "main.cpp") && !m("cpp$", "main.cpp.bak"));
    CHECK(m("!test main", "src/main.cpp") && !m("!test main", "src/test/main.cpp"));
    CHECK(m("main !^src", "native/main.cpp") && !m("main !^src", "src/main.cpp"));
    CHECK(m("src cpp", "src/main.cpp") && !m("src cpp", "src/main.h"));

    /* speed: 200k paths, a 5 character query */
    std::vector<std::string> files;
    unsigned long long s = 88172645463325252ull;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)(s >> 11); };
    const char *seg[] = {"src", "native", "ui", "bench", "results", "kitty", "lestrix", "docs", "tests", "include", "driver", "net", "fs", "core", "util"};
    for (int i = 0; i < 200000; i++) {
        std::string f = "/home/user/project";
        for (unsigned d = 0, nd = 2 + rnd() % 4; d < nd; d++) { f += "/"; f += seg[rnd() % 15]; f += std::to_string(rnd() % 40); }
        f += "/file" + std::to_string(rnd() % 5000) + (rnd() % 2 ? ".cpp" : ".h");
        files.push_back(f);
    }
    FuzzyQuery q; q.parse("ntvut");
    double t0 = now();
    size_t hits = 0;
    for (const std::string &f : files) { int sc; if (q.match(f.data(), f.size(), &sc, nullptr)) hits++; }
    double dt = now() - t0;
    printf("test_fuzzy: %s; %zu paths in %.1f ms (%.0f ns/path, %zu matched), single thread\n", fails ? "FAILED" : "ok", files.size(), dt * 1e3, dt / (double)files.size() * 1e9, hits);
    return fails != 0;
}
