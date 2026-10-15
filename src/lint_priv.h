/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_LINT_PRIV_H
#define HE_LINT_PRIV_H
/* what the lint passes (lint.cpp, lint_k8s.cpp, lint_formats.cpp) share: the line context, noqa, adding a diagnostic, small string helpers, the YAML line reader */
#include <ctype.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "lint.h"

namespace lintp {

using sv = std::string_view;

struct Ctx {
    std::vector<Diag> &out;
    size_t max;
    uint32_t line = 0;
    sv cur;                 /* the line being checked, for "# noqa" */
    bool full() const { return out.size() >= max; }
};

inline bool noqa(const Ctx &c, const char *code) {
    size_t p = c.cur.find("noqa");
    if (p == sv::npos || c.cur.find('#') == sv::npos) return false;
    p += 4;
    if (p >= c.cur.size() || c.cur[p] != ':') return true;   /* a bare "# noqa" silences everything on the line */
    sv rest = c.cur.substr(p + 1);
    std::string short_code = code;
    size_t br = short_code.find('[');
    if (br != std::string::npos) short_code.resize(br);
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && (rest[i] == ' ' || rest[i] == ',')) i++;
        size_t j = i;
        while (j < rest.size() && rest[j] != ' ' && rest[j] != ',') j++;
        sv tok = rest.substr(i, j - i);
        if (!tok.empty() && (tok == code || tok == short_code)) return true;
        i = j;
    }
    return false;
}

inline Diag &add(Ctx &c, size_t c0, size_t c1, int sev, const char *code, const std::string &msg) {
    static Diag dummy;
    if (c.full() || noqa(c, code)) { dummy = Diag(); return dummy; }
    Diag d;
    d.line = c.line; d.c0 = (uint32_t)c0; d.c1 = (uint32_t)std::max(c1, c0 + 1); d.sev = (uint8_t)sev; d.code = code; d.msg = msg;
    c.out.push_back(std::move(d));
    return c.out.back();
}

inline void fixit(Diag &d, size_t f0, size_t f1, const std::string &text) { d.has_fix = true; d.f0 = (uint32_t)f0; d.f1 = (uint32_t)f1; d.fix = text; }

inline sv trim(sv s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline bool starts(sv s, sv p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
inline bool ends(sv s, sv p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
inline bool idc(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* word at or after position p: the identifier starting there */
inline sv word_at(sv s, size_t p) { size_t e = p; while (e < s.size() && idc(s[e])) e++; return s.substr(p, e - p); }

/* ---------------------------------------------------------------- YAML line reader */

struct YLine {
    size_t indent = 0;          /* first non-space column */
    int dashes = 0;             /* "- " markers before the content */
    size_t kp = 0;              /* column where the content (key) starts */
    bool blank = true;          /* blank or comment only */
    bool is_key = false;
    sv key;
    size_t k0 = 0, k1 = 0;
    sv value;                   /* trimmed, comment removed */
    size_t v0 = 0;
    bool tab_indent = false;
};

inline void parse_yline(sv line, YLine &y) {
    y = YLine();
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') i++;
    if (i < line.size() && line[i] == '\t') { y.tab_indent = true; size_t j = i; while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) j++; i = j; }
    y.indent = i;
    if (i >= line.size() || line[i] == '#' || line[i] == '\r') return;
    y.blank = false;
    size_t p = i;
    while (p < line.size() && line[p] == '-' && (p + 1 == line.size() || line[p + 1] == ' ')) {
        y.dashes++;
        p++;
        while (p < line.size() && line[p] == ' ') p++;
    }
    y.kp = p;
    if (p >= line.size()) return;
    size_t k0 = p, k1;
    char c = line[p];
    if (c == '"' || c == '\'') {
        size_t q = p + 1;
        while (q < line.size() && line[q] != c) { if (line[q] == '\\' && c == '"') q++; q++; }
        if (q >= line.size() || q + 1 >= line.size() + 0 || line[q + 1] != ':') return;
        k0 = p + 1; k1 = q;
        p = q + 2;
    } else {
        if (c == '{' || c == '[' || c == '|' || c == '>' || c == '&' || c == '*' || c == '!' || c == '%' || c == '@' || c == '`') return;
        size_t q = p;
        while (q < line.size()) {
            if (line[q] == ':' && (q + 1 == line.size() || line[q + 1] == ' ' || line[q + 1] == '\r')) break;
            if (line[q] == '#' && q > p && line[q - 1] == ' ') return;
            if (line[q] == '{' && q + 1 < line.size() && line[q + 1] == '{') return;   /* a Jinja expression, not a key */
            q++;
        }
        if (q >= line.size()) return;
        k1 = q;
        while (k1 > k0 && line[k1 - 1] == ' ') k1--;
        p = q + 1;
    }
    y.is_key = true;
    y.key = line.substr(k0, k1 - k0);
    y.k0 = k0; y.k1 = k1;
    while (p < line.size() && line[p] == ' ') p++;
    y.v0 = p;
    sv v = line.substr(p);
    bool q1 = false, q2 = false;
    size_t cut = v.size();
    for (size_t t = 0; t < v.size(); t++) {
        if (v[t] == '\'' && !q2) q1 = !q1;
        else if (v[t] == '"' && !q1) q2 = !q2;
        else if (v[t] == '#' && !q1 && !q2 && (t == 0 || v[t - 1] == ' ')) { cut = t; break; }
    }
    y.value = trim(v.substr(0, cut));
}



inline int lev(sv a, sv b) {   /* edit distance, small strings */
    if (a.size() > 40 || b.size() > 40) return 99;
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
        prev.swap(cur);
    }
    return prev[b.size()];
}

/* passes in other files */
bool k8s_detect(sv text, const std::string &path);
void k8s_pass(Ctx &c, sv text, sv orig, bool helm);
std::string helm_neutralize(sv text);
void k8s_special(Ctx &c, sv text, const std::string &path);   /* kustomization.yaml, Chart.yaml */
void toml_check(Ctx &c, sv text);
void xml_check(Ctx &c, sv text);
void markdown_check(Ctx &c, sv text, const std::string &path);

struct SqlState { std::string stmt; uint32_t start_line = 0; size_t start_col = 0; bool open = false; };
void sql_line(Ctx &c, sv code, SqlState &st);
struct CssState { std::vector<std::vector<std::string>> blocks; };
void css_line(Ctx &c, sv code, CssState &st);
void misc_line(Ctx &c, const char *lang, sv code);

}  // namespace lintp
#endif
