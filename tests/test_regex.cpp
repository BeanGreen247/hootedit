/* Differential test: random patterns and texts, our engine against std::regex (ECMAScript, same leftmost-first semantics).
 * Also checks scan() against a line-by-line find(). */
#include <gtest/gtest.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <regex>
#include <string>
#include <vector>
#include "../src/regex.h"

static unsigned long long s = 0x9E3779B97F4A7C15ull;
static unsigned rnd() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)(s >> 11); }
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

/* returns a pattern; sets nullable when it can match the empty string (those are not repeated, ECMAScript treats empty loops specially) */
static std::string gen(int depth, bool *nullable) {
    int r = rnd() % (depth > 2 ? 6 : 12);
    *nullable = false;
    switch (r) {
    case 0: case 1: case 2: return std::string(1, "abc"[rnd() % 3]);
    case 3: return ".";
    case 4: { static const char *c[] = {"[ab]", "[^a]", "[a-c]", "\\d", "\\w", "[^bc]", "\\s"}; return c[rnd() % 7]; }
    case 5: return std::string(1, "ab1 _"[rnd() % 5]);
    case 6: case 7: {   /* concatenation */
        bool n1, n2;
        std::string a = gen(depth + 1, &n1), b = gen(depth + 1, &n2);
        *nullable = n1 && n2;
        return a + b;
    }
    case 8: {           /* alternation */
        bool n1, n2;
        std::string a = gen(depth + 1, &n1), b = gen(depth + 1, &n2);
        *nullable = n1 || n2;
        return "(?:" + a + "|" + b + ")";
    }
    case 9: case 10: {  /* quantified non-nullable atom */
        bool n1;
        std::string a = gen(depth + 1, &n1);
        if (n1) { *nullable = true; return a; }
        static const char *q[] = {"*", "+", "?", "{2}", "{1,2}", "*?", "+?", "??", "{0,2}", "{2,}"};
        const char *qq = q[rnd() % 10];
        *nullable = qq[0] == '*' || qq[0] == '?' || !strcmp(qq, "{0,2}");
        return "(?:" + a + ")" + qq;
    }
    default: {          /* assertion around an atom */
        bool n1;
        std::string a = gen(depth + 1, &n1);
        switch (rnd() % 3) { case 0: return "^" + a; case 1: return a + "$"; default: return "\\b" + a; }
    }
    }
}

TEST(Regex, RandomPatternsAgainstStdRegex) {
    int cases = 0;
    for (int it = 0; it < 30000; it++) {
        bool nl;
        std::string pat = gen(0, &nl);
        bool icase = rnd() % 5 == 0;
        std::regex ref;
        try { ref = std::regex(pat, icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript); } catch (...) { continue; }
        Regex re;
        std::string err;
        ASSERT_TRUE(re.compile(pat, icase, &err)) << "compile " << pat << ": " << err;
        Matcher m(re);
        for (int t = 0; t < 6; t++) {
            std::string text;
            for (unsigned k = 0, n = rnd() % 10; k < n; k++) text += "abcAB1 _"[rnd() % 8];
            std::smatch sm;
            bool want = std::regex_search(text, sm, ref);
            size_t ms = 0, me = 0;
            bool got = m.find(text.data(), text.size(), 0, &ms, &me);
            bool tst = m.test(text.data(), text.size());
            cases++;
            ASSERT_FALSE(got != want || tst != want || (want && (ms != (size_t)sm.position(0) || me != (size_t)sm.position(0) + (size_t)sm.length(0))))
                << "pattern '" << pat << "'" << (icase ? " (i)" : "") << " text '" << text << "': std " << want << " ours find=" << got << " test=" << tst << " [" << ms << "," << me << ")";
        }
    }
    EXPECT_GT(cases, 100000);
}

TEST(Regex, Utf8DotAndNegatedClassesConsumeWholeCharacters) {
    struct U { const char *pat, *text; long ms, me; } u[] = {
        {".", "\xc3\xa9", 0, 2}, {"[^a]", "a\xc3\xa9", 1, 3}, {"..", "\xc3\xa9", -1, -1}, {"\xc3\xa9+", "x\xc3\xa9\xc3\xa9y", 1, 5},
        {"caf.", "caf\xc3\xa9!", 0, 5}, {"\\W", "a\xc3\xa9", 1, 3}, {"a.b", "a\xe2\x82\xac" "b", 0, 5}, {"a.b", "a\xf0\x9f\x98\x80" "b", 0, 6},
    };
    for (const U &c : u) {
        Regex re;
        std::string err;
        re.compile(c.pat, false, &err);
        Matcher m(re);
        size_t ms = 0, me = 0;
        bool got = m.find(c.text, strlen(c.text), 0, &ms, &me);
        bool ok = c.ms < 0 ? !got : got && (long)ms == c.ms && (long)me == c.me;
        EXPECT_TRUE(ok) << "utf8 '" << c.pat << "': got " << got << " [" << ms << "," << me << ") want [" << c.ms << "," << c.me << ")";
    }
}

TEST(Regex, ScanEqualsLineByLineFind) {
    const char *pats[] = {"foo", "fo+bar", "^foo", "bar$", "\\bfoo\\b", "f.o", "[0-9]+x", "(?i)FOO", "a|b|zzz", "^$", "x*", "fooo?bar\\d"};
    std::string buf;
    for (int i = 0; i < 4000; i++) {
        static const char *w[] = {"foo", "bar", "fooo", "foobar", "12x", "zzz", "", "Foo", "a b", "fo9", "xx foo yy", "fooobar7"};
        buf += w[rnd() % 12];
        buf += rnd() % 3 ? "\n" : " bar\n";
    }
    buf += "tail foo";
    for (const char *p : pats) {
        Regex re;
        std::string err;
        ASSERT_TRUE(re.compile(p, false, &err)) << p << ": " << err;
        Matcher m(re), m2(re);
        std::vector<std::pair<size_t, size_t>> a, b;
        m.scan(buf.data(), buf.size(), [&](size_t ls, size_t le) { a.push_back({ls, le}); return true; });
        size_t ls = 0;
        while (ls < buf.size()) {
            size_t le = buf.find('\n', ls);
            if (le == std::string::npos) le = buf.size();
            size_t x, y;
            if (m2.find(buf.data() + ls, le - ls, 0, &x, &y)) b.push_back({ls, le});
            ls = le + 1;
        }
        EXPECT_EQ(a, b) << "pattern " << p;
    }
}
