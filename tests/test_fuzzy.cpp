/* The fuzzy matcher: matching, ranking the way fzf ranks, positions, extended syntax. */
#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include <vector>
#include "../src/fuzzy.h"

static bool m(const char *q, const char *t, int *sc = nullptr, std::vector<int> *pos = nullptr) {
    FuzzyQuery fq;
    fq.parse(q);
    int s = 0;
    std::vector<int> p;
    bool ok = fq.match(t, strlen(t), &s, &p);
    if (sc) *sc = s;
    if (pos) *pos = p;
    return ok;
}
static bool better(const char *q, const char *a, const char *b) { int sa = 0, sb = 0; return m(q, a, &sa) && (!m(q, b, &sb) || sa > sb); }

TEST(Fuzzy, Matching) {
    EXPECT_TRUE(m("abc", "a_b_c") && m("abc", "xaxbxc") && !m("abc", "acb") && !m("abc", "ab"));
    EXPECT_TRUE(m("", "anything"));
    EXPECT_TRUE(m("ABC", "ABC") && !m("ABC", "abc"));            /* smart case: upper-case in the query is exact */
    EXPECT_TRUE(m("abc", "ABC"));
}
TEST(Fuzzy, RankingTheWayFzfRanks) {
    EXPECT_TRUE(better("foo", "src/foo.cpp", "src/xfxoxo.cpp"));          /* consecutive beats scattered */
    EXPECT_TRUE(better("fb", "foo_bar", "fxxbxx"));                       /* word starts beat the middle */
    EXPECT_TRUE(better("fb", "fooBar", "foobar"));                        /* camelCase hump */
    EXPECT_TRUE(better("util", "native/src/util.cpp", "native/bench/results/kitty/log.txt"));
    EXPECT_TRUE(better("mn", "main.cpp", "xmxnx"));
}
TEST(Fuzzy, Positions) {
    std::vector<int> p;
    EXPECT_TRUE(m("fb", "foo_bar", nullptr, &p) && p.size() == 2 && p[0] == 0 && p[1] == 4);
    EXPECT_TRUE(m("abc", "xabcx", nullptr, &p) && p.size() == 3 && p[0] == 1 && p[1] == 2 && p[2] == 3);
}
TEST(Fuzzy, ExtendedSyntax) {
    EXPECT_TRUE(m("'foo", "a foo b") && !m("'foo", "f o o"));
    EXPECT_TRUE(m("^src", "src/main.cpp") && !m("^src", "native/src/main.cpp"));
    EXPECT_TRUE(m("cpp$", "main.cpp") && !m("cpp$", "main.cpp.bak"));
    EXPECT_TRUE(m("!test main", "src/main.cpp") && !m("!test main", "src/test/main.cpp"));
    EXPECT_TRUE(m("main !^src", "native/main.cpp") && !m("main !^src", "src/main.cpp"));
    EXPECT_TRUE(m("src cpp", "src/main.cpp") && !m("src cpp", "src/main.h"));
}
