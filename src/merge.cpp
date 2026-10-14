/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "merge.h"

#include <algorithm>

namespace {
bool marker(const std::string &l, char c, bool allow_text) {
    if (l.size() < 7) return false;
    for (int i = 0; i < 7; i++) if (l[(size_t)i] != c) return false;
    if (l.size() == 7) return true;
    return allow_text && l[7] == ' ';
}
}  // namespace

bool merge_parse(const std::string &text, MergeView &v) {
    v.segs.clear();
    v.nconf = 0;
    v.crlf = false;
    v.final_nl = !text.empty() && text.back() == '\n';
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(pos, e - pos);
        if (!l.empty() && l.back() == '\r') { l.pop_back(); v.crlf = true; }
        lines.push_back(l);
        pos = e + 1;
    }
    MergeSeg common;
    MergeSeg cf;
    int state = 0;   /* 0 common, 1 current, 2 base, 3 incoming */
    auto flush = [&] { if (!common.common.empty()) v.segs.push_back(common); common = MergeSeg(); };
    for (const std::string &l : lines) {
        if (state == 0 && marker(l, '<', true)) { flush(); cf = MergeSeg(); cf.conflict = true; state = 1; if (v.cur_label.empty()) v.cur_label = l.size() > 8 ? l.substr(8) : std::string(); }
        else if (state == 1 && marker(l, '|', true)) { state = 2; cf.has_base = true; }
        else if ((state == 1 || state == 2) && l == "=======") state = 3;
        else if (state == 3 && marker(l, '>', true)) { v.segs.push_back(cf); v.nconf++; state = 0; if (v.inc_label.empty()) v.inc_label = l.size() > 8 ? l.substr(8) : std::string(); }
        else if (state == 0) common.common.push_back(l);
        else if (state == 1) cf.ours.push_back(l);
        else if (state == 2) cf.base.push_back(l);
        else cf.theirs.push_back(l);
    }
    if (state != 0) { v.segs.clear(); v.nconf = 0; return false; }   /* a block that never ends: leave the file to the plain editor */
    flush();
    return v.nconf > 0;
}

std::vector<std::string> merge_result_lines(const MergeView &v, int *unresolved) {
    std::vector<std::string> out;
    int un = 0;
    for (const MergeSeg &s : v.segs) {
        if (!s.conflict) { out.insert(out.end(), s.common.begin(), s.common.end()); continue; }
        switch (s.choice) {
        case 1: out.insert(out.end(), s.ours.begin(), s.ours.end()); break;
        case 2: out.insert(out.end(), s.theirs.begin(), s.theirs.end()); break;
        case 3: out.insert(out.end(), s.ours.begin(), s.ours.end()); out.insert(out.end(), s.theirs.begin(), s.theirs.end()); break;
        case 4: out.insert(out.end(), s.theirs.begin(), s.theirs.end()); out.insert(out.end(), s.ours.begin(), s.ours.end()); break;
        case 5: out.insert(out.end(), s.base.begin(), s.base.end()); break;
        default: un++; break;
        }
    }
    if (unresolved) *unresolved = un;
    return out;
}

std::string merge_result_text(const MergeView &v) {
    std::string o;
    std::vector<std::string> l = merge_result_lines(v, nullptr);
    for (size_t i = 0; i < l.size(); i++) {
        o += l[i];
        if (i + 1 < l.size() || v.final_nl) o += v.crlf ? "\r\n" : "\n";
    }
    return o;
}

void merge_layout(MergeView &v) {
    v.rows.clear(); v.res.clear(); v.conf_seg.clear(); v.conf_row.clear();
    v.unresolved = 0;
    const int keep = 3;   /* lines of context kept on each side of a conflict out of a long common run */
    for (size_t si = 0; si < v.segs.size(); si++) {
        const MergeSeg &s = v.segs[si];
        if (!s.conflict) {
            int n = (int)s.common.size();
            bool before = si > 0, after = si + 1 < v.segs.size();
            for (int i = 0; i < n; i++) {
                bool shown;
                if (n <= 2 * keep + 1 || (!before && !after)) shown = true;
                else if (!before) shown = i >= n - keep;
                else if (!after) shown = i < keep;
                else shown = i < keep || i >= n - keep;
                if (shown) { MRow r; r.seg = (int)si; r.kind = 0; r.a = i; v.rows.push_back(r); }
                else if (v.rows.empty() || v.rows.back().kind != 3 || v.rows.back().seg != (int)si) { MRow r; r.seg = (int)si; r.kind = 3; r.a = 1; v.rows.push_back(r); }
                else v.rows.back().a++;
                v.res.push_back({s.common[(size_t)i], 0, (int)si});
            }
            continue;
        }
        v.conf_seg.push_back((int)si);
        v.conf_row.push_back((int)v.rows.size());
        { MRow r; r.seg = (int)si; r.kind = 1; v.rows.push_back(r); }
        int n = (int)std::max(s.ours.size(), s.theirs.size());
        for (int i = 0; i < n; i++) { MRow r; r.seg = (int)si; r.kind = 2; r.a = i < (int)s.ours.size() ? i : -1; r.b = i < (int)s.theirs.size() ? i : -1; v.rows.push_back(r); }
        MergeView one;
        one.segs.push_back(s);
        int un = 0;
        std::vector<std::string> l = merge_result_lines(one, &un);
        if (un) { v.unresolved++; v.res.push_back({"<<< conflict " + std::to_string(v.conf_seg.size()) + " is not resolved yet >>>", 1, (int)si}); }
        else for (const std::string &t : l) v.res.push_back({t, 2, (int)si});
    }
    v.dirty = false;
}
