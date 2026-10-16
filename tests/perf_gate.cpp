/* Speed gate: a regression that makes a hot path several times slower fails here. The limits are loose (about 8x below what a 2-core laptop
 * measures) so a slow machine does not trip them; they exist to catch a quadratic loop or a copy per line, not a 10% change.
 * Built without sanitizers. Run with: meson test --suite perf */
#include <gtest/gtest.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include "../src/buffer.h"
#include "../src/doc.h"
#include "../src/fuzzy.h"
#include "../src/lint.h"
#include "../src/regex.h"
#include "../src/syntax.h"

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static unsigned long long rs = 88172645463325252ull;
static unsigned rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 11); }

static std::string source_like(size_t bytes) {
    std::string s;
    while (s.size() < bytes) s += "static int compute_value(const char *s, size_t n) { return n > 42 ? s[0] + 1 : helper(n); } // note \"x\"\n";
    return s;
}

TEST(PerfGate, LexerThroughput) {
    std::string line = "static int compute(const char *s, size_t n) { return n > 42 ? s[0] + 1 : 0; } // note \"x\"";
    const Lang *c = lang_at(1);
    std::vector<uint8_t> tok(line.size());
    uint8_t st = 0;
    const int N = 200000;
    double t0 = now();
    for (int i = 0; i < N; i++) st = lex_line(c, line.data(), line.size(), st, tok.data());
    double mbs = (double)line.size() * N / (now() - t0) / 1e6;
    RecordProperty("lexer_MB_per_s", (int)mbs);
    EXPECT_GT(mbs, 40.0) << "the lexer slowed down to " << mbs << " MB/s";
}

TEST(PerfGate, FuzzyFilterOver200kPaths) {
    std::vector<std::string> files;
    const char *seg[] = {"src", "native", "ui", "bench", "results", "kitty", "lestrix", "docs", "tests", "include", "driver", "net", "fs", "core", "util"};
    for (int i = 0; i < 200000; i++) {
        std::string f = "/home/user/project";
        for (unsigned d = 0, nd = 2 + rnd() % 4; d < nd; d++) { f += "/"; f += seg[rnd() % 15]; f += std::to_string(rnd() % 40); }
        f += "/file" + std::to_string(rnd() % 5000) + (rnd() % 2 ? ".cpp" : ".h");
        files.push_back(f);
    }
    FuzzyQuery q;
    q.parse("ntvut");
    double t0 = now();
    size_t hits = 0;
    for (const std::string &f : files) { int sc; if (q.match(f.data(), f.size(), &sc, nullptr)) hits++; }
    double dt = now() - t0;
    RecordProperty("fuzzy_ms", (int)(dt * 1000));
    EXPECT_LT(dt, 0.8) << files.size() << " paths took " << dt * 1e3 << " ms (" << hits << " matched)";
}

TEST(PerfGate, RegexScanOf32MbOfText) {
    std::string big = source_like(32u << 20);
    big += "needle_in_haystack here\n";
    const char *pats[] = {"needle_in_haystack", "needle_\\w+", "(?i)NEEDLE_IN", "\\bneedle"};
    for (const char *p : pats) {
        Regex re;
        std::string err;
        ASSERT_TRUE(re.compile(p, false, &err)) << err;
        Matcher m(re);
        size_t hits = 0;
        double t0 = now();
        m.scan(big.data(), big.size(), [&](size_t, size_t) { hits++; return true; });
        double mbs = (double)big.size() / (now() - t0) / 1e6;
        EXPECT_EQ(hits, 1u) << p;
        EXPECT_GT(mbs, 150.0) << "pattern " << p << " scanned at " << mbs << " MB/s";
    }
}

TEST(PerfGate, PieceTableRandomEditsOnAnOpenFile) {
    char path[] = "/tmp/hootedit-perf-XXXXXX";
    int fd = mkstemp(path);
    std::string chunk = source_like(1u << 20);
    for (int i = 0; i < 16; i++) ASSERT_EQ(write(fd, chunk.data(), chunk.size()), (ssize_t)chunk.size());
    close(fd);
    Buffer b;
    ASSERT_TRUE(b.open(path));
    double t0 = now();
    for (int i = 0; i < 20000; i++) {
        size_t pos = rnd() % (b.size() + 1);
        if (i % 3 == 2) b.erase(pos, 2); else b.insert(pos, "ab", 2);
    }
    double dt = now() - t0;
    unlink(path);
    EXPECT_LT(dt, 1.5) << "20000 edits took " << dt << " s";
}

TEST(PerfGate, FlatScanOfAnEditedBufferIsOneCopy) {
    Buffer b;
    std::string chunk = source_like(1u << 20);
    for (int i = 0; i < 8; i++) b.insert(b.size(), chunk.data(), chunk.size());
    b.insert(5, "x", 1);
    double t0 = now();
    size_t n = 0;
    for (int i = 0; i < 10; i++) { const char *p = b.flat(&n); (void)p; }
    EXPECT_LT(now() - t0, 0.5) << "flat() copies again although nothing changed";
}

TEST(PerfGate, LiveChecksOnACodeFile) {
    std::string text = source_like(1u << 20);
    LintInput in;
    in.text = text.data(); in.n = text.size(); in.path = "big.c"; in.lang = lang_detect("big.c", "", 0);
    std::vector<Diag> d;
    double t0 = now();
    lint_run(in, d);
    double dt = now() - t0;
    EXPECT_LT(dt, 0.35) << "1 MB of C took " << dt * 1e3 << " ms to check";
}

TEST(PerfGate, LiveChecksOnAKubernetesManifest) {
    std::string text;
    for (int i = 0; i < 40; i++) for (const AnsibleTemplate &t : k8s_templates()) { text += t.text; text += "---\n"; }
    LintInput in;
    in.text = text.data(); in.n = text.size(); in.path = "m.yaml"; in.lang = lang_detect("m.yaml", "", 0);
    std::vector<Diag> d;
    double t0 = now();
    lint_run(in, d);
    double dt = now() - t0;
    EXPECT_LT(dt, 0.08) << text.size() / 1024 << " KB of manifests took " << dt * 1e3 << " ms to check";
}
