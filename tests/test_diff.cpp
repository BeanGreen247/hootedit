/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "../src/diff.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int lcs(const std::vector<int> &a, const std::vector<int> &b) {
    std::vector<std::vector<int>> t(a.size() + 1, std::vector<int>(b.size() + 1, 0));
    for (size_t i = 1; i <= a.size(); i++)
        for (size_t j = 1; j <= b.size(); j++) t[i][j] = a[i - 1] == b[j - 1] ? t[i - 1][j - 1] + 1 : std::max(t[i - 1][j], t[i][j - 1]);
    return t[a.size()][b.size()];
}

TEST(Diff, RandomPairsReplayAndAreMinimalAgainstLcs) {
    srand(7);
    for (int it = 0; it < 20000; it++) {
        int n = rand() % 14, m = rand() % 14, alpha = 1 + rand() % 5;
        std::vector<int> a((size_t)n), b((size_t)m);
        for (int &x : a) x = rand() % alpha;
        for (int &x : b) x = rand() % alpha;
        bool exact;
        auto h = diff_ids(a, b, 1000, &exact);
        ASSERT_TRUE(exact);
        /* replaying the hunks over a must give b, and the script must be minimal (n + m - 2 * LCS edits) */
        std::vector<int> r;
        int ai = 0, edits = 0;
        for (const DiffHunk &k : h) {
            ASSERT_GE(k.a0, ai);
            while (ai < k.a0) r.push_back(a[(size_t)ai++]);
            for (int i = 0; i < k.bn; i++) r.push_back(b[(size_t)(k.b0 + i)]);
            ai = k.a0 + k.an;
            edits += k.an + k.bn;
        }
        while (ai < n) r.push_back(a[(size_t)ai++]);
        ASSERT_EQ(r, b) << "n=" << n << " m=" << m << " it=" << it;
        ASSERT_EQ(edits, n + m - 2 * lcs(a, b)) << "n=" << n << " m=" << m << " it=" << it;
    }
}

TEST(Diff, AlignedRowsReassembleBothSides) {
    DiffModel dm;
    diff_build(dm, "a\nb\nc\nd\ne\n", "a\nB\nc\nx\ny\ne\nf\n");
    std::string la, lb;
    for (const DiffRow &r : dm.rows) { if (r.a >= 0) la += dm.a[(size_t)r.a] + "|"; if (r.b >= 0) lb += dm.b[(size_t)r.b] + "|"; }
    EXPECT_EQ(la, "a|b|c|d|e|");
    EXPECT_EQ(lb, "a|B|c|x|y|e|f|");
    EXPECT_EQ(dm.hunks.size(), 3u);
    int mods = 0, adds = 0, dels = 0;
    for (const DiffRow &r : dm.rows) { mods += r.kind == DK_MOD; adds += r.kind == DK_ADD; dels += r.kind == DK_DEL; }
    EXPECT_TRUE(mods == 2 && adds == 2 && dels == 0);   /* b->B and d->x are changes, y and f are new */
}

TEST(Diff, ChangedPartOfAModifiedLine) {
    DiffModel dm;
    diff_build(dm, "int main() { return 0; }\n", "int main() { return 42; }\n");
    ASSERT_TRUE(dm.rows.size() == 1 && dm.rows[0].kind == DK_MOD);
    EXPECT_EQ(dm.a[0].substr((size_t)dm.rows[0].ia0, (size_t)(dm.rows[0].ia1 - dm.rows[0].ia0)), "0");
    EXPECT_EQ(dm.b[0].substr((size_t)dm.rows[0].ib0, (size_t)(dm.rows[0].ib1 - dm.rows[0].ib0)), "42");
}

TEST(Diff, CrlfAndMissingFinalNewlineAreNotChanges) {
    DiffModel dm;
    diff_build(dm, "a\r\nb\r\n", "a\nb");
    EXPECT_TRUE(dm.hunks.empty());
}

TEST(Diff, HugeDisjointPairFallsBackToOneHunk) {
    std::vector<int> a(5000), b(5000);
    for (size_t i = 0; i < a.size(); i++) { a[i] = (int)i; b[i] = (int)i + 100000; }
    bool exact = true;
    auto h = diff_ids(a, b, 200, &exact);
    EXPECT_TRUE(!exact && h.size() == 1 && h[0].an == 5000 && h[0].bn == 5000);
}
