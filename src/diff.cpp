/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "diff.h"

#include <algorithm>
#include <string.h>
#include <string_view>
#include <unordered_map>

void diff_split_lines(const std::string &text, std::vector<std::string> &out) {
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = nl == std::string::npos ? text.size() : nl;
        size_t e2 = end;
        if (e2 > pos && text[e2 - 1] == '\r') e2--;
        out.emplace_back(text, pos, e2 - pos);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

std::vector<DiffHunk> diff_ids(const std::vector<int> &a, const std::vector<int> &b, int max_d, bool *exact) {
    std::vector<DiffHunk> out;
    if (exact) *exact = true;
    const int n0 = (int)a.size(), m0 = (int)b.size();
    int pre = 0;
    while (pre < n0 && pre < m0 && a[(size_t)pre] == b[(size_t)pre]) pre++;
    int suf = 0;
    while (suf < n0 - pre && suf < m0 - pre && a[(size_t)(n0 - 1 - suf)] == b[(size_t)(m0 - 1 - suf)]) suf++;
    const int n = n0 - pre - suf, m = m0 - pre - suf;
    if (n == 0 && m == 0) return out;
    if (n == 0 || m == 0) { out.push_back({pre, n, pre, m}); return out; }

    const int *A = a.data() + pre, *B = b.data() + pre;
    const int maxd = std::min(n + m, max_d);
    const int off = maxd + 1;
    std::vector<int> v((size_t)(2 * maxd + 3), 0);
    std::vector<std::vector<int>> trace;
    int found = -1;
    for (int d = 0; d <= maxd && found < 0; d++) {
        trace.emplace_back(v.begin() + (off - d - 1), v.begin() + (off + d + 2));   /* the values round d starts from, indices -d-1..d+1 */
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[(size_t)(off + k - 1)] < v[(size_t)(off + k + 1)])) x = v[(size_t)(off + k + 1)];
            else x = v[(size_t)(off + k - 1)] + 1;
            int y = x - k;
            while (x < n && y < m && A[x] == B[y]) { x++; y++; }
            v[(size_t)(off + k)] = x;
            if (x >= n && y >= m) { found = d; break; }
        }
    }
    if (found < 0) {   /* too different to bother: the whole middle is one change */
        if (exact) *exact = false;
        out.push_back({pre, n, pre, m});
        return out;
    }
    std::vector<char> del((size_t)n, 0), ins((size_t)m, 0);
    int x = n, y = m;
    for (int d = found; d > 0; d--) {
        const std::vector<int> &t = trace[(size_t)d];   /* t[k + d + 1] */
        int k = x - y;
        auto at = [&](int kk) { return t[(size_t)(kk + d + 1)]; };
        int pk = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        int px = at(pk), py = px - pk;
        while (x > px && y > py) { x--; y--; }   /* the diagonal run of equal lines just before the edit */
        if (x == px) ins[(size_t)(y - 1)] = 1;   /* moved down: a line of B was inserted */
        else del[(size_t)(x - 1)] = 1;           /* moved right: a line of A was deleted */
        x = px; y = py;
    }
    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && !del[(size_t)i] && !ins[(size_t)j]) { i++; j++; continue; }
        int a0 = i, b0 = j;
        while (i < n && del[(size_t)i]) i++;
        while (j < m && ins[(size_t)j]) j++;
        if (i == a0 && j == b0) { i++; j++; continue; }   /* cannot happen with a valid script; keeps the walk finite */
        out.push_back({pre + a0, i - a0, pre + b0, j - b0});
    }
    return out;
}

void diff_build(DiffModel &m, const std::string &old_text, const std::string &new_text) {
    diff_split_lines(old_text, m.a);
    diff_split_lines(new_text, m.b);
    std::unordered_map<std::string_view, int> ids;
    auto idv = [&](const std::vector<std::string> &L) {
        std::vector<int> r(L.size());
        for (size_t i = 0; i < L.size(); i++) { auto it = ids.emplace(std::string_view(L[i]), (int)ids.size()); r[i] = it.first->second; }
        return r;
    };
    std::vector<int> ia = idv(m.a), ib = idv(m.b);
    m.hunks = diff_ids(ia, ib, 3000, &m.exact);
    m.rows.clear();
    int ai = 0, bi = 0;
    for (size_t h = 0; h < m.hunks.size(); h++) {
        const DiffHunk &k = m.hunks[h];
        while (ai < k.a0 && bi < k.b0) { m.rows.push_back({ai++, bi++, DK_SAME, -1, 0, 0, 0, 0}); }
        int common = std::min(k.an, k.bn);
        for (int i = 0; i < common; i++) {
            DiffRow r{k.a0 + i, k.b0 + i, DK_MOD, (int)h, 0, 0, 0, 0};
            const std::string &sa = m.a[(size_t)r.a], &sb = m.b[(size_t)r.b];
            size_t p = 0;
            while (p < sa.size() && p < sb.size() && sa[p] == sb[p]) p++;
            size_t s = 0;
            while (s < sa.size() - p && s < sb.size() - p && sa[sa.size() - 1 - s] == sb[sb.size() - 1 - s]) s++;
            while (p > 0 && ((unsigned char)sa[p] & 0xC0) == 0x80) p--;   /* land on a character boundary */
            while (s > 0 && p + s < sa.size() && ((unsigned char)sa[sa.size() - s] & 0xC0) == 0x80) s--;
            r.ia0 = (int)p; r.ia1 = (int)(sa.size() - s);
            r.ib0 = (int)p; r.ib1 = (int)(sb.size() - s);
            if (r.ia1 < r.ia0) r.ia1 = r.ia0;
            if (r.ib1 < r.ib0) r.ib1 = r.ib0;
            m.rows.push_back(r);
        }
        for (int i = common; i < k.an; i++) m.rows.push_back({k.a0 + i, -1, DK_DEL, (int)h, 0, 0, 0, 0});
        for (int i = common; i < k.bn; i++) m.rows.push_back({-1, k.b0 + i, DK_ADD, (int)h, 0, 0, 0, 0});
        ai = k.a0 + k.an; bi = k.b0 + k.bn;
    }
    while (ai < (int)m.a.size() && bi < (int)m.b.size()) m.rows.push_back({ai++, bi++, DK_SAME, -1, 0, 0, 0, 0});
}

void diff_layout(DiffView &v, bool inline_mode) {
    v.vrows.clear();
    v.change_v.assign(v.m.hunks.size(), -1);
    v.vinline = inline_mode;
    v.vdirty = false;
    const int CTX = 3, MIN_HIDE = 4;
    const std::vector<DiffRow> &rows = v.m.rows;
    auto opened = [&](int start) { for (int o : v.open_folds) if (o == start) return true; return false; };
    auto note = [&](const DiffRow &r) { if (r.hunk >= 0 && v.change_v[(size_t)r.hunk] < 0) v.change_v[(size_t)r.hunk] = (int)v.vrows.size(); };
    for (size_t i = 0; i < rows.size();) {
        const DiffRow &r = rows[i];
        if (r.kind == DK_SAME) {
            size_t j = i;
            while (j < rows.size() && rows[j].kind == DK_SAME) j++;
            size_t run = j - i;
            bool at_start = i == 0, at_end = j == rows.size();
            size_t keep_head = at_start ? 0 : (size_t)CTX, keep_tail = at_end ? 0 : (size_t)CTX;
            if (!v.all_open && run >= keep_head + keep_tail + (size_t)MIN_HIDE && !opened((int)(i + keep_head))) {
                for (size_t k = 0; k < keep_head; k++) v.vrows.push_back({(int)(i + k), 0, 0});
                v.vrows.push_back({(int)(i + keep_head), 3, (int)(run - keep_head - keep_tail)});
                for (size_t k = run - keep_tail; k < run; k++) v.vrows.push_back({(int)(i + k), 0, 0});
            } else
                for (size_t k = 0; k < run; k++) v.vrows.push_back({(int)(i + k), 0, 0});
            i = j;
            continue;
        }
        note(r);
        if (!inline_mode) v.vrows.push_back({(int)i, 0, 0});
        else {
            if (r.kind == DK_MOD) {   /* the removed line, then the added one */
                v.vrows.push_back({(int)i, 1, 0});
                v.vrows.push_back({(int)i, 2, 0});
            } else v.vrows.push_back({(int)i, r.kind == DK_DEL ? (uint8_t)1 : (uint8_t)2, 0});
        }
        i++;
    }
}
