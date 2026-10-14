/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "../src/diff.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int lcs(const std::vector<int> &a, const std::vector<int> &b) {
    std::vector<std::vector<int>> t(a.size() + 1, std::vector<int>(b.size() + 1, 0));
    for (size_t i = 1; i <= a.size(); i++)
        for (size_t j = 1; j <= b.size(); j++) t[i][j] = a[i - 1] == b[j - 1] ? t[i - 1][j - 1] + 1 : std::max(t[i - 1][j], t[i][j - 1]);
    return t[a.size()][b.size()];
}

int main() {
    srand(7);
    for (int it = 0; it < 20000; it++) {
        int n = rand() % 14, m = rand() % 14, alpha = 1 + rand() % 5;
        std::vector<int> a((size_t)n), b((size_t)m);
        for (int &x : a) x = rand() % alpha;
        for (int &x : b) x = rand() % alpha;
        bool exact;
        auto h = diff_ids(a, b, 1000, &exact);
        CHECK(exact);
        /* replaying the hunks over a must give b, and the script must be minimal (n + m - 2 * LCS edits) */
        std::vector<int> r;
        int ai = 0, edits = 0;
        for (const DiffHunk &k : h) {
            CHECK(k.a0 >= ai);
            while (ai < k.a0) r.push_back(a[(size_t)ai++]);
            for (int i = 0; i < k.bn; i++) r.push_back(b[(size_t)(k.b0 + i)]);
            ai = k.a0 + k.an;
            edits += k.an + k.bn;
        }
        while (ai < n) r.push_back(a[(size_t)ai++]);
        CHECK(r == b);
        CHECK(edits == n + m - 2 * lcs(a, b));
        if (fails) { printf("n=%d m=%d it=%d\n", n, m, it); return 1; }
    }
    {   /* rows: the two sides reassemble from the aligned rows, with the filler where a side has no line */
        DiffModel dm;
        diff_build(dm, "a\nb\nc\nd\ne\n", "a\nB\nc\nx\ny\ne\nf\n");
        std::string la, lb;
        for (const DiffRow &r : dm.rows) { if (r.a >= 0) la += dm.a[(size_t)r.a] + "|"; if (r.b >= 0) lb += dm.b[(size_t)r.b] + "|"; }
        CHECK(la == "a|b|c|d|e|");
        CHECK(lb == "a|B|c|x|y|e|f|");
        CHECK(dm.hunks.size() == 3);
        int mods = 0, adds = 0, dels = 0;
        for (const DiffRow &r : dm.rows) { mods += r.kind == DK_MOD; adds += r.kind == DK_ADD; dels += r.kind == DK_DEL; }
        CHECK(mods == 2 && adds == 2 && dels == 0);   /* b->B and d->x are changes, y and f are new */
    }
    {   /* the changed part of a modified line, for the stronger highlight */
        DiffModel dm;
        diff_build(dm, "int main() { return 0; }\n", "int main() { return 42; }\n");
        CHECK(dm.rows.size() == 1 && dm.rows[0].kind == DK_MOD);
        CHECK(dm.a[0].substr((size_t)dm.rows[0].ia0, (size_t)(dm.rows[0].ia1 - dm.rows[0].ia0)) == "0");
        CHECK(dm.b[0].substr((size_t)dm.rows[0].ib0, (size_t)(dm.rows[0].ib1 - dm.rows[0].ib0)) == "42");
    }
    {   /* CRLF files compare equal line by line, and a missing final newline is not a change */
        DiffModel dm;
        diff_build(dm, "a\r\nb\r\n", "a\nb");
        CHECK(dm.hunks.empty());
    }
    {   /* a huge disjoint pair falls back to one hunk instead of running away */
        std::vector<int> a(5000), b(5000);
        for (size_t i = 0; i < a.size(); i++) { a[i] = (int)i; b[i] = (int)i + 100000; }
        bool exact = true;
        auto h = diff_ids(a, b, 200, &exact);
        CHECK(!exact && h.size() == 1 && h[0].an == 5000 && h[0].bn == 5000);
    }
    printf(fails ? "diff: %d failures\n" : "diff ok\n", fails);
    return fails != 0;
}
