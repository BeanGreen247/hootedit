/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "vim.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>

namespace {

size_t ls(const Doc &d, size_t l) { return d.buf.line_start(l); }
size_t lend(const Doc &d, size_t l) { return d.buf.line_start(l) + d.buf.line_len(l); }
unsigned char at(const Doc &d, size_t p) {
    if (p >= d.buf.size()) return 0;
    std::string t;
    d.buf.copy(p, 1, t);
    return t.empty() ? 0 : (unsigned char)t[0];
}
std::string slice(const Doc &d, size_t a, size_t b) {
    std::string t;
    if (b > a) d.buf.copy(a, b - a, t);
    return t;
}
size_t first_nb(const Doc &d, size_t l) {
    std::string t;
    d.buf.line_text(l, t);
    size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
    return ls(d, l) + i;
}
int cls(unsigned char c, bool big) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0) return 0;
    if (big) return 1;
    return (isalnum(c) || c == '_' || c >= 0x80) ? 1 : 2;
}
size_t wfwd(const Doc &d, size_t p, bool big) {
    size_t N = d.buf.size();
    if (p >= N) return p;
    int c = cls(at(d, p), big);
    if (c) while (p < N && cls(at(d, p), big) == c) p++;
    while (p < N && cls(at(d, p), big) == 0) p++;
    return p;
}
size_t wback(const Doc &d, size_t p, bool big) {
    if (p == 0) return 0;
    p--;
    while (p > 0 && cls(at(d, p), big) == 0) p--;
    int c = cls(at(d, p), big);
    while (p > 0 && cls(at(d, p - 1), big) == c) p--;
    return p;
}
size_t wend(const Doc &d, size_t p, bool big) {
    size_t N = d.buf.size();
    if (p + 1 >= N) return p;
    p++;
    while (p < N && cls(at(d, p), big) == 0) p++;
    if (p >= N) return N ? N - 1 : 0;
    int c = cls(at(d, p), big);
    while (p + 1 < N && cls(at(d, p + 1), big) == c) p++;
    return p;
}
std::string indent_unit(const Doc &d) {
    if (d.indent.compare(0, 3, "Tab") == 0) return "\t";
    const char *c = strchr(d.indent.c_str(), ':');
    int n = c ? atoi(c + 1) : 4;
    return std::string((size_t)std::max(1, std::min(8, n)), ' ');
}
char swapcase(char c) { return isupper((unsigned char)c) ? (char)tolower((unsigned char)c) : (char)toupper((unsigned char)c); }

int col_at(const Doc &d, size_t pos) { size_t l = d.buf.line_of(pos); return d.col_of_byte(l, pos - ls(d, l)); }

/* the position in line l at display column col; short = the line ends before it */
size_t pos_at_col(const Doc &d, size_t l, int col, bool *short_line = nullptr) {
    size_t p = ls(d, l), e = lend(d, l);
    while (p < e && d.col_of_byte(l, p - ls(d, l)) < col) p = d.next_char(p);
    if (short_line) *short_line = p >= e && d.col_of_byte(l, e - ls(d, l)) < col;
    return p;
}

void geom(const Doc &d, size_t anchor, size_t cur, bool eol, size_t *l0, size_t *l1, int *c0, int *c1) {
    size_t la = d.buf.line_of(anchor), lc = d.buf.line_of(cur);
    int ca = col_at(d, anchor), cc = col_at(d, cur);
    *l0 = std::min(la, lc); *l1 = std::max(la, lc);
    *c0 = std::min(ca, cc); *c1 = eol ? 1 << 28 : std::max(ca, cc);
}

/* bytes of line l inside columns c0..c1: [a, b); false when the line ends before c0 */
bool brange(const Doc &d, size_t l, int c0, int c1, size_t *a, size_t *b) {
    size_t e = lend(d, l);
    size_t p = pos_at_col(d, l, c0);
    if (p >= e) return false;
    size_t q = p;
    while (q < e && d.col_of_byte(l, q - ls(d, l)) <= c1) q = d.next_char(q);
    *a = p; *b = q;
    return true;
}

}  // namespace

std::string Vim::label() const {
    const char *m = mode == INSERT ? "-- INSERT --" : mode == NORMAL ? "-- NORMAL --" : mode == REPLACE ? "-- REPLACE --" : mode == VISUAL ? "-- VISUAL --" : mode == VLINE ? "-- VISUAL LINE --" : "-- VISUAL BLOCK --";
    std::string s = m;
    std::string p;
    if (opcount) p += std::to_string(opcount);
    if (op) p += op;
    if (count) p += std::to_string(count);
    p += pend;
    if (!p.empty()) s += "   " + p;
    if (macro_rec) s += std::string("   recording @") + macro_rec;
    return s;
}

void Vim::setc(Doc &d, size_t p, bool kw) {
    while (p > 0 && p < d.buf.size() && (at(d, p) & 0xC0) == 0x80) p--;
    d.cur = p;
    if (mode < VISUAL) d.anchor = p;
    if (!kw) { size_t l = d.buf.line_of(p); d.want_col = d.col_of_byte(l, p - ls(d, l)); }
}

void Vim::clamp(Doc &d) {
    if (mode != NORMAL) return;
    size_t l = d.buf.line_of(d.cur), e = lend(d, l);
    if (d.cur >= e && e > ls(d, l)) setc(d, d.prev_char(e));
}

size_t Vim::vert(Doc &d, size_t from, long delta) {
    long l = (long)d.buf.line_of(from) + delta;
    l = std::max(0L, std::min(l, (long)d.buf.lines() - 1));
    size_t p = ls(d, (size_t)l), e = lend(d, (size_t)l);
    while (p < e && d.col_of_byte((size_t)l, p - ls(d, (size_t)l)) < d.want_col) p = d.next_char(p);
    return p;
}

void Vim::yank(const std::string &t, bool line) {
    char rn = reg_name;
    reg_name = 0;
    if (rn == '_') return;   /* the black hole register */
    reg = t; reg_line = line; reg_sys = t; reg_block = false;
    if (rn >= 'a' && rn <= 'z') regs[rn] = {t, line};
    else if (rn >= 'A' && rn <= 'Z') { auto &r = regs[(char)tolower(rn)]; r.first += t; r.second = r.second || line; }
    if (set_clip && (rn == 0 || rn == '+' || rn == '*')) set_clip(t);
}

void Vim::enter_insert(Doc &d) {
    mode = INSERT;
    d.break_group();
    recording = !replaying;
    ins_rec.clear();
}

void Vim::done_change() {
    if (replaying) return;
    last_keys = keys;
    last_ins.clear();
}

Vim::Mv Vim::motion(Doc &d, const std::string &k, int n, bool has_count, bool opmode) {
    Mv m;
    size_t cur = d.cur, l = d.buf.line_of(cur), N = d.buf.size();
    m.pos = cur;
    m.ok = true;
    const std::string &s = k;
    if (s == "h" || s == "\x08") {
        for (int i = 0; i < n && m.pos > ls(d, l); i++) m.pos = d.prev_char(m.pos);
    } else if (s == "l" || s == " ") {
        size_t e = lend(d, l);
        for (int i = 0; i < n; i++) {
            size_t nx = d.next_char(m.pos);
            if (m.pos >= e || (!opmode && nx >= e) || nx > e) break;
            m.pos = nx;
        }
    } else if (s == "j" || s == "k" || s == "+" || s == "-" || s == "\r") {
        long delta = (s == "k" || s == "-") ? -(long)n : (long)n;
        long tl = (long)l + delta;
        if (tl < 0 || tl >= (long)d.buf.lines()) { if (opmode) { m.ok = false; return m; } tl = std::max(0L, std::min(tl, (long)d.buf.lines() - 1)); delta = tl - (long)l; }
        m.pos = vert(d, cur, delta);
        if (s == "+" || s == "-" || s == "\r") m.pos = first_nb(d, (size_t)tl);
        m.line = true;
        m.vert = s == "j" || s == "k";
    } else if (s == "w" || s == "W") {
        for (int i = 0; i < n; i++) m.pos = wfwd(d, m.pos, s == "W");
        if (opmode && m.pos > lend(d, l) && cur < lend(d, l)) m.pos = lend(d, l);
        else if (!opmode && m.pos >= N && N) m.pos = d.prev_char(N);
    } else if (s == "b" || s == "B") {
        for (int i = 0; i < n; i++) m.pos = wback(d, m.pos, s == "B");
    } else if (s == "e" || s == "E") {
        for (int i = 0; i < n; i++) m.pos = wend(d, m.pos, s == "E");
        m.incl = true;
    } else if (s == "0") {
        m.pos = ls(d, l);
    } else if (s == "^") {
        m.pos = first_nb(d, l);
    } else if (s == "$") {
        size_t tl = std::min(l + (size_t)n - 1, d.buf.lines() - 1);
        size_t e = lend(d, tl);
        m.pos = e > ls(d, tl) ? d.prev_char(e) : ls(d, tl);
        m.incl = true;
        if (tl != l && !opmode) m.vert = false;
    } else if (s == "gg" || s == "G") {
        size_t tl = has_count ? std::min((size_t)n - 1, d.buf.lines() - 1) : (s == "gg" ? 0 : d.buf.lines() - 1);
        m.pos = first_nb(d, tl);
        m.line = true;
    } else if (s.size() >= 2 && (s[0] == 'f' || s[0] == 'F' || s[0] == 't' || s[0] == 'T' || s[0] == ';' || s[0] == ',')) {
        char kind = s[0];
        std::string ch = s.substr(1);
        if (kind == ';' || kind == ',') { m.ok = false; return m; }   /* handled below with the stored char */
        fch_kind = kind; fch = ch;
        goto find_char;
    } else if (s == ";" || s == ",") {
        if (!fch_kind) { m.ok = false; return m; }
        {
            char kind = fch_kind;
            if (s == ",") kind = isupper((unsigned char)kind) ? (char)tolower((unsigned char)kind) : (char)toupper((unsigned char)kind);
            std::string t;
            d.buf.line_text(l, t);
            size_t col = cur - ls(d, l), pos = col;
            bool fwd = kind == 'f' || kind == 't';
            for (int i = 0; i < n; i++) {
                size_t from = fwd ? pos + 1 : pos;
                if (kind == 't' && i == 0 && pos + 1 < t.size() && t.compare(pos + 1, fch.size(), fch) == 0) from = pos + 2;
                if (kind == 'T' && i == 0 && pos > 0 && t.compare(pos - 1, fch.size(), fch) == 0 && pos >= 2) from = pos - 1;
                size_t f = fwd ? t.find(fch, from) : (from == 0 ? std::string::npos : t.rfind(fch, from - 1));
                if (f == std::string::npos) { m.ok = false; return m; }
                pos = f;
            }
            if (kind == 't') pos = pos > 0 ? pos - 1 : 0;
            if (kind == 'T') pos = pos + fch.size();
            m.pos = ls(d, l) + pos;
            m.incl = fwd;
        }
    } else if (s == "%") {
        std::string t;
        d.buf.line_text(l, t);
        size_t i = cur - ls(d, l);
        while (i < t.size() && !strchr("()[]{}", t[i])) i++;
        if (i >= t.size()) { m.ok = false; return m; }
        size_t p = ls(d, l) + i;
        char c = t[i];
        const char *o = "([{", *cl = ")]}";
        bool fwd = strchr(o, c) != nullptr;
        char want = fwd ? cl[strchr(o, c) - o] : o[strchr(cl, c) - cl];
        int depth = 0;
        size_t budget = 400000;
        for (size_t q = p; budget-- > 0;) {
            unsigned char ch = at(d, q);
            if (ch == (unsigned char)c) depth++;
            else if (ch == (unsigned char)want && --depth == 0) { m.pos = q; break; }
            if (fwd) { if (++q >= N) { m.ok = false; return m; } } else { if (q == 0) { m.ok = false; return m; } q--; }
        }
        if (m.pos == cur) m.ok = false;
        m.incl = true;
    } else if (s == "n" || s == "N") {
        bool f = (s == "n") == last_fwd;
        size_t pos = cur;
        for (int i = 0; i < n; i++) if (!search_go(d, f, f ? d.next_char(pos) : pos, &pos)) { m.ok = false; return m; }
        m.pos = pos;
    } else if (s == "*" || s == "#") {
        size_t le = lend(d, l), a = cur;
        if (le == ls(d, l)) { m.ok = false; return m; }
        if (a >= le) a = le - 1;
        while (a < le && cls(at(d, a), false) != 1) a++;   /* the word under or after the cursor */
        if (a >= le) { m.ok = false; return m; }
        size_t b = a;
        while (a > ls(d, l) && cls(at(d, a - 1), false) == 1) a--;
        while (b < le && cls(at(d, b), false) == 1) b++;
        last_pat = "\\b" + slice(d, a, b) + "\\b";
        last_fwd = s == "*";
        size_t pos = a;
        for (int i = 0; i < n; i++) if (!search_go(d, last_fwd, last_fwd ? d.next_char(pos) : pos, &pos)) { m.ok = false; return m; }
        if (on_search) on_search(last_pat);
        m.pos = pos;
    } else if (s == "H" || s == "M" || s == "L") {
        long rows = page_rows ? page_rows() : 20, last = (long)d.buf.lines() - 1, top = std::min((long)d.top, last), tl;
        if (s == "H") tl = std::min(top + n - 1, last);
        else if (s == "M") tl = std::min(top + rows / 2, last);
        else tl = std::max(0L, std::min(top + rows - 1, last) - (n - 1));
        m.pos = first_nb(d, (size_t)tl);
        m.line = true;
    } else if (s == "ge" || s == "gE") {
        size_t pos = cur;
        for (int i = 0; i < n && pos > 0; i++) {
            int c = cls(at(d, pos), s == "gE");
            while (pos > 0 && c != 0 && cls(at(d, pos), s == "gE") == c) pos--;
            while (pos > 0 && cls(at(d, pos), s == "gE") == 0) pos--;
        }
        m.pos = pos;
        m.incl = true;
    } else if (s.size() == 2 && (s[0] == '\'' || s[0] == '`')) {
        auto it = marks.find(s[1]);
        if (it == marks.end()) { m.ok = false; return m; }
        m.pos = std::min(it->second, d.buf.size());
        if (s[0] == '\'') { m.pos = first_nb(d, d.buf.line_of(m.pos)); m.line = true; }
    } else if (s == "}" || s == "{") {
        long tl = (long)l;
        long step = s == "}" ? 1 : -1;
        for (int i = 0; i < n; i++) {
            tl += step;
            while (tl >= 0 && tl < (long)d.buf.lines() && d.buf.line_len((size_t)tl) == 0) tl += step;   /* skip the blank run we are in */
            while (tl >= 0 && tl < (long)d.buf.lines() && d.buf.line_len((size_t)tl) != 0) tl += step;
        }
        tl = std::max(0L, std::min(tl, (long)d.buf.lines() - 1));
        m.pos = ls(d, (size_t)tl);
        if (tl == (long)d.buf.lines() - 1 && d.buf.line_len((size_t)tl) != 0) m.pos = lend(d, (size_t)tl);
    } else {
        m.ok = false;
    }
    return m;

find_char : {
    std::string t;
    d.buf.line_text(l, t);
    size_t pos = cur - ls(d, l);
    bool fwd = fch_kind == 'f' || fch_kind == 't';
    for (int i = 0; i < n; i++) {
        size_t from = fwd ? pos + 1 : pos;
        size_t f = fwd ? t.find(fch, from) : (from == 0 ? std::string::npos : t.rfind(fch, from - 1));
        if (f == std::string::npos) { m.ok = false; return m; }
        pos = f;
    }
    if (fch_kind == 't') pos = pos > 0 ? pos - 1 : 0;
    if (fch_kind == 'T') pos = pos + fch.size();
    m.pos = ls(d, l) + pos;
    m.incl = fwd;
    return m;
}
}

bool Vim::object(Doc &d, const std::string &k, R &r) {
    bool in = k[0] == 'i';
    char c = k.size() > 1 ? k[1] : 0;
    size_t cur = d.cur, l = d.buf.line_of(cur), a0 = ls(d, l), e0 = lend(d, l);
    if (c == 'w' || c == 'W') {
        bool big = c == 'W';
        if (e0 == a0) return false;
        size_t p = std::min(cur, e0 - 1);
        int cl = cls(at(d, p), big);
        size_t a = p, b = p + 1;
        while (a > a0 && cls(at(d, a - 1), big) == cl) a--;
        while (b < e0 && cls(at(d, b), big) == cl) b++;
        if (!in) {
            if (b < e0 && cls(at(d, b), big) == 0 && cl != 0) { while (b < e0 && cls(at(d, b), big) == 0) b++; }
            else if (cl != 0) { while (a > a0 && cls(at(d, a - 1), big) == 0) a--; }
        }
        r = {a, b, false};
        return true;
    }
    if (c == 'p') {
        auto blank = [&](size_t x) { return d.buf.line_len(x) == 0; };
        size_t a = l, b = l, nl = d.buf.lines();
        bool bl = blank(l);
        while (a > 0 && blank(a - 1) == bl) a--;
        while (b + 1 < nl && blank(b + 1) == bl) b++;
        if (!in) {
            if (b + 1 < nl) { bool nb = blank(b + 1); size_t e = b + 1; while (e + 1 < nl && blank(e + 1) == nb) e++; b = e; }
            else if (a > 0) { bool nb = blank(a - 1); size_t e = a - 1; while (e > 0 && blank(e - 1) == nb) e--; a = e; }
        }
        r = {a, b, true};
        return true;
    }
    if (c == '"' || c == '\'' || c == '`') {
        std::string t = slice(d, a0, e0);
        std::vector<size_t> q;
        for (size_t i = 0; i < t.size(); i++) { if (t[i] == '\\') { i++; continue; } if (t[i] == c) q.push_back(i); }
        size_t col = cur - a0;
        for (size_t i = 0; i + 1 < q.size(); i += 2) {
            if (q[i + 1] >= col) {
                if (q[i] > col && false) return false;
                r = in ? R{a0 + q[i] + 1, a0 + q[i + 1], false} : R{a0 + q[i], a0 + q[i + 1] + 1, false};
                return true;
            }
        }
        return false;
    }
    char o = 0, cl = 0;
    switch (c) {
    case '(': case ')': case 'b': o = '('; cl = ')'; break;
    case '{': case '}': case 'B': o = '{'; cl = '}'; break;
    case '[': case ']': o = '['; cl = ']'; break;
    case '<': case '>': o = '<'; cl = '>'; break;
    default: return false;
    }
    size_t open = std::string::npos;
    if (at(d, cur) == (unsigned char)o) open = cur;
    else {
        int depth = at(d, cur) == (unsigned char)cl ? 1 : 0;
        size_t budget = 400000;
        for (size_t q = cur; q-- > 0 && budget-- > 0;) {
            unsigned char ch = at(d, q);
            if (ch == (unsigned char)cl) depth++;
            else if (ch == (unsigned char)o) { if (depth == 0) { open = q; break; } depth--; }
        }
    }
    if (open == std::string::npos) return false;
    size_t close = std::string::npos;
    {
        int depth = 0;
        size_t budget = 400000;
        for (size_t q = open + 1; q < d.buf.size() && budget-- > 0; q++) {
            unsigned char ch = at(d, q);
            if (ch == (unsigned char)o) depth++;
            else if (ch == (unsigned char)cl) { if (depth == 0) { close = q; break; } depth--; }
        }
    }
    if (close == std::string::npos) return false;
    if (!in) { r = {open, close + 1, false}; return true; }
    size_t a = open + 1, b = close;
    if (at(d, a) == '\n') a++;
    size_t cl_line = d.buf.line_of(close), lb = ls(d, cl_line);
    if (lb > a && slice(d, lb, close).find_first_not_of(" \t") == std::string::npos) b = lb;
    if (b < a) b = a;
    r = {a, b, false};
    return true;
}

void Vim::apply(Doc &d, char o, R r) {
    d.break_group();
    size_t from, to;
    bool last_line = false;
    std::string text;
    if (r.line) {
        r.b = std::min(r.b, d.buf.lines() - 1);
        from = ls(d, r.a);
        to = r.b + 1 < d.buf.lines() ? ls(d, r.b + 1) : d.buf.size();
        last_line = r.b + 1 >= d.buf.lines();
        text = slice(d, from, to);
        if (text.empty() || text.back() != '\n') text += "\n";
    } else {
        from = r.a; to = r.b;
        text = slice(d, from, to);
    }
    if (o == 'y') {
        yank(text, r.line);
        if (!r.line) setc(d, from); else if (mode >= VISUAL) setc(d, from);
        clamp(d);
        return;
    }
    if (o == 'd') {
        yank(text, r.line);
        size_t a = from;
        if (r.line && last_line && from > 0) a = from - 1;
        d.replace(a, to - a, "", 0);
        if (r.line) { size_t l = std::min(r.a, d.buf.lines() - 1); setc(d, first_nb(d, l)); }
        else setc(d, from);
        clamp(d);
        d.break_group();
        return;
    }
    if (o == 'c') {
        yank(text, r.line);
        if (r.line) {
            size_t cs = ls(d, r.a), ce = lend(d, std::min(r.b, d.buf.lines() - 1));
            std::string ind = slice(d, cs, first_nb(d, r.a));
            d.replace(cs, ce - cs, ind.data(), ind.size());
            enter_insert(d);
            setc(d, cs + ind.size());
        } else {
            d.replace(from, to - from, "", 0);
            enter_insert(d);
            setc(d, from);
        }
        return;
    }
    if (o == '>' || o == '<') {
        size_t l0 = r.line ? r.a : d.buf.line_of(from), l1 = r.line ? std::min(r.b, d.buf.lines() - 1) : d.buf.line_of(to > from ? to - 1 : to);
        std::string unit = indent_unit(d), out, t;
        for (size_t l = l0; l <= l1; l++) {
            d.buf.line_text(l, t);
            if (o == '>') { if (!t.empty()) out += unit; out += t; }
            else {
                size_t i = 0;
                if (!t.empty() && t[0] == '\t') i = 1;
                else while (i < t.size() && i < unit.size() && t[i] == ' ') i++;
                out += t.substr(i);
            }
            if (l < l1) out += '\n';
        }
        size_t s0 = ls(d, l0), e0 = lend(d, l1);
        d.replace(s0, e0 - s0, out.data(), out.size());
        setc(d, first_nb(d, l0));
        d.break_group();
        return;
    }
    if (o == 'u' || o == 'U' || o == '~') {
        std::string t = text;
        for (char &c : t) c = o == 'u' ? (char)tolower((unsigned char)c) : o == 'U' ? (char)toupper((unsigned char)c) : swapcase(c);
        if (r.line) { size_t e = to; if (e > from && at(d, e - 1) == '\n') e--; t.resize(e - from); to = e; }
        d.replace(from, to - from, t.data(), t.size());
        setc(d, from);
        d.break_group();
    }
}

void Vim::paste(Doc &d, bool after, int n) {
    std::string t = reg;
    bool line = reg_line;
    char rn = reg_name;
    reg_name = 0;
    if (rn == '_') return;
    if (isalpha((unsigned char)rn)) { auto it = regs.find((char)tolower(rn)); if (it == regs.end()) return; t = it->second.first; line = it->second.second; }
    else if ((rn == '+' || rn == '*') && get_clip) { t = get_clip(); line = !t.empty() && t.back() == '\n'; }
    else if (get_clip) { std::string s = get_clip(); if (!s.empty() && s != reg_sys) { t = s; line = s.back() == '\n'; } }
    if (t.empty()) return;
    if (reg_block && rn == 0 && t == reg) { paste_block(d, after, n); return; }
    d.break_group();
    size_t l = d.buf.line_of(d.cur);
    if (line) {
        std::string body = t.back() == '\n' ? t.substr(0, t.size() - 1) : t, all;
        for (int i = 0; i < n; i++) { if (i) all += "\n"; all += body; }
        if (after) { std::string ins = "\n" + all; d.replace(lend(d, l), 0, ins.data(), ins.size()); setc(d, first_nb(d, l + 1)); }
        else { std::string ins = all + "\n"; d.replace(ls(d, l), 0, ins.data(), ins.size()); setc(d, first_nb(d, l)); }
    } else {
        std::string all;
        for (int i = 0; i < n; i++) all += t;
        size_t pos = after && lend(d, l) > ls(d, l) ? d.next_char(d.cur) : d.cur;
        d.replace(pos, 0, all.data(), all.size());
        setc(d, d.prev_char(pos + all.size()));
    }
    clamp(d);
    d.break_group();
}

bool Vim::search_go(Doc &d, bool fwd, size_t from, size_t *pos) {
    if (!find || last_pat.empty()) return false;
    return find(last_pat, from, fwd, pos);
}

void Vim::save_vis(Doc &d) {
    if (mode == VBLOCK) { vis_a = bl_anchor; vis_c = d.cur; vis_mode = VISUAL; vis_l0 = d.buf.line_of(std::min(bl_anchor, d.cur)); vis_l1 = d.buf.line_of(std::max(bl_anchor, d.cur)); return; }
    vis_a = d.anchor; vis_c = d.cur; vis_mode = mode;
    vis_l0 = d.buf.line_of(std::min(d.anchor, d.cur));
    vis_l1 = d.buf.line_of(std::max(d.anchor, d.cur));
}

void Vim::increment(Doc &d, int delta) {
    size_t l = d.buf.line_of(d.cur), s0 = ls(d, l);
    std::string t;
    d.buf.line_text(l, t);
    size_t i = d.cur - s0;
    while (i > 0 && i <= t.size() && isdigit((unsigned char)t[i - 1]) && (i == t.size() || isdigit((unsigned char)t[i]))) i--;
    if (i < t.size() && !isdigit((unsigned char)t[i])) { while (i < t.size() && !isdigit((unsigned char)t[i])) i++; }
    if (i >= t.size()) return;
    size_t b = i, e = i;
    while (b > 0 && isdigit((unsigned char)t[b - 1])) b--;
    while (e < t.size() && isdigit((unsigned char)t[e])) e++;
    bool neg = b > 0 && t[b - 1] == '-';
    long long v = atoll(t.substr(b, e - b).c_str());
    if (neg) v = -v;
    v += delta;
    std::string out = std::to_string(v);
    size_t from = neg ? b - 1 : b;
    d.break_group();
    d.replace(s0 + from, e - from, out.data(), out.size());
    setc(d, s0 + from + out.size() - 1);
    d.break_group();
}

namespace {
void pair_of(const std::string &c, std::string &o, std::string &cl) {
    switch (c.empty() ? 0 : c[0]) {
    case '(': o = "( "; cl = " )"; break;
    case ')': case 'b': o = "("; cl = ")"; break;
    case '{': o = "{ "; cl = " }"; break;
    case '}': case 'B': o = "{"; cl = "}"; break;
    case '[': o = "[ "; cl = " ]"; break;
    case ']': o = "["; cl = "]"; break;
    case '<': case '>': o = "<"; cl = ">"; break;
    default: o = cl = c; break;
    }
}
}  // namespace

void Vim::sur_wrap(Doc &d, R r, const std::string &c) {
    size_t a, b;
    if (r.line) { a = first_nb(d, r.a); b = lend(d, std::min(r.b, d.buf.lines() - 1)); }
    else { a = r.a; b = r.b; }
    std::string o, cl;
    pair_of(c, o, cl);
    std::string t = o + slice(d, a, b) + cl;
    d.break_group();
    d.replace(a, b - a, t.data(), t.size());
    setc(d, a);
    d.break_group();
}

void Vim::sur_key(Doc &d, const std::string &k) {
    if (sur == 'Y') { sur = 0; sur_wrap(d, sur_r, k); return; }
    if (sur == 'c' && sur_a.empty()) { sur_a = k; return; }
    char kind = sur;
    std::string from = kind == 'c' ? sur_a : k;
    sur = 0;
    sur_a.clear();
    R r{0, 0, false};
    if (!object(d, "a" + from, r) || r.b < r.a + 2) return;
    std::string inner = slice(d, r.a + 1, r.b - 1), o, cl;
    if (kind == 'c') { pair_of(k, o, cl); inner = o + inner + cl; }
    d.break_group();
    d.replace(r.a, r.b - r.a, inner.data(), inner.size());
    setc(d, r.a);
    d.break_group();
}

void Vim::play_macro(Doc &d, char name, int n) {
    if (macro_play) return;
    auto it = macros.find(name);
    if (it == macros.end()) return;
    last_macro = name;
    std::vector<Ev> evs = it->second;
    macro_play = true;
    for (int i = 0; i < n; i++)
        for (const Ev &e : evs) {
            if (e.t == 0) text(d, e.s);
            else if (e.t == 1) type_char(d, e.s);
            else if (e.t == 2) escape(d);
            else if (e.t == 3) { if (!backspace(d)) { erased(); d.erase_back(); } }
        }
    macro_play = false;
}


void Vim::type_char(Doc &d, const std::string &ch) {
    typed(ch);
    if (mode == REPLACE) {
        size_t l = d.buf.line_of(d.cur);
        if (d.cur >= lend(d, l)) { d.insert_text(ch.data(), ch.size()); repl_stack.push_back(std::string()); }
        else {
            size_t n = d.next_char(d.cur);
            std::string old = slice(d, d.cur, n);
            d.replace(d.cur, n - d.cur, ch.data(), ch.size());
            repl_stack.push_back(old);
        }
        return;
    }
    d.insert_text(ch.data(), ch.size());
}

bool Vim::backspace(Doc &d) {
    if (mode != REPLACE) return false;
    erased();
    size_t l = d.buf.line_of(d.cur);
    if (d.cur <= ls(d, l)) return true;
    size_t p = d.prev_char(d.cur);
    if (!repl_stack.empty()) {
        std::string old = repl_stack.back();
        repl_stack.pop_back();
        d.replace(p, d.cur - p, old.data(), old.size());
        d.cur = d.anchor = p;
    } else d.cur = d.anchor = p;
    return true;
}

bool Vim::block_rect(const Doc &d, size_t *l0, size_t *l1, int *c0, int *c1) const {
    if (mode != VBLOCK) return false;
    geom(d, bl_anchor, d.cur, bl_eol, l0, l1, c0, c1);
    return true;
}

void Vim::paste_block(Doc &d, bool after, int n) {
    std::vector<std::string> pieces;
    size_t from = 0;
    while (true) { size_t nl = reg.find('\n', from); pieces.push_back(reg.substr(from, nl == std::string::npos ? std::string::npos : nl - from)); if (nl == std::string::npos) break; from = nl + 1; }
    size_t l = d.buf.line_of(d.cur);
    int col = col_at(d, d.cur);
    if (after && lend(d, l) > ls(d, l)) col += 1;
    int w = 0;
    for (const std::string &p : pieces) w = std::max(w, (int)p.size());
    size_t last = l + pieces.size() - 1, lines = d.buf.lines();
    std::string out, t;
    for (size_t i = l; i <= last; i++) {
        if (i < lines) d.buf.line_text(i, t); else t.clear();
        const std::string &pc = pieces[i - l];
        bool sh;
        size_t p = i < lines ? pos_at_col(d, i, col, &sh) : ls(d, lines - 1);
        size_t bi = i < lines ? p - ls(d, i) : 0;
        if (i >= lines || sh) { int have = i < lines ? d.col_of_byte(i, t.size()) : 0; t.append((size_t)std::max(0, col - have), ' '); bi = t.size(); }
        std::string ins;
        for (int k = 0; k < n; k++) { ins += pc; if (k + 1 < n) ins.append((size_t)std::max(0, w - (int)pc.size()), ' '); }
        bool tail = bi < t.size();
        if (tail) ins.append((size_t)std::max(0, w - (int)pc.size()) * 0, ' ');
        t.insert(bi, ins);
        out += t;
        if (i < last) out += '\n';
    }
    size_t s0 = ls(d, l), e0 = last < lines ? lend(d, last) : d.buf.size();
    std::string prefix;
    if (last >= lines) {   /* the block is taller than the file: new lines at the end */
        size_t extra = last - (lines - 1);
        (void)extra;
    }
    d.break_group();
    d.replace(s0, e0 - s0, out.data(), out.size());
    d.break_group();
    d.cur = d.anchor = pos_at_col(d, l, col);
    clamp(d);
}

bool Vim::block(Doc &d, const std::string &k) {
    size_t l0, l1;
    int c0, c1;
    geom(d, bl_anchor, d.cur, bl_eol, &l0, &l1, &c0, &c1);
    auto leave = [&] { clear_pending(); mode = NORMAL; d.anchor = d.cur; };
    auto rebuild = [&](const std::function<void(size_t, std::string &, size_t, size_t, bool)> &f) {   /* f(line, text, byte a, byte b, in range) edits the line text in place */
        std::string out, t;
        for (size_t l = l0; l <= l1; l++) {
            d.buf.line_text(l, t);
            size_t a, b;
            bool in = brange(d, l, c0, c1, &a, &b);
            f(l, t, in ? a - ls(d, l) : 0, in ? b - ls(d, l) : 0, in);
            out += t;
            if (l < l1) out += '\n';
        }
        size_t s0 = ls(d, l0), e0 = lend(d, l1);
        d.break_group();
        d.replace(s0, e0 - s0, out.data(), out.size());
        d.break_group();
    };
    auto pieces = [&]() {
        std::string out;
        for (size_t l = l0; l <= l1; l++) { size_t a, b; if (brange(d, l, c0, c1, &a, &b)) out += slice(d, a, b); if (l < l1) out += '\n'; }
        return out;
    };
    auto cut = [&]() {
        std::string p = pieces();
        rebuild([](size_t, std::string &t, size_t a, size_t b, bool in) { if (in) t.erase(a, b - a); });
        yank(p, false);
        reg_block = true;
    };
    auto start_insert = [&](char kind) {
        blk = {true, l0, l1, c0, c1, kind, bl_eol};
        clear_pending();
        mode = NORMAL;   /* enter_insert wants a non-visual mode for the cursor sync */
        d.anchor = d.cur;
        if (kind == 'A') {
            if (bl_eol) setc(d, lend(d, l0));
            else {
                bool sh;
                size_t p = pos_at_col(d, l0, c1 + 1, &sh);
                if (sh) { std::string pad((size_t)(c1 + 1 - d.col_of_byte(l0, lend(d, l0) - ls(d, l0))), ' '); d.replace(lend(d, l0), 0, pad.data(), pad.size()); p = lend(d, l0); }
                setc(d, p);
            }
        } else setc(d, pos_at_col(d, l0, c0));
        enter_insert(d);
    };
    if (k == "\x16") { leave(); return true; }
    if (k == "v") { clear_pending(); mode = VISUAL; d.anchor = bl_anchor; return true; }
    if (k == "V") { clear_pending(); mode = VLINE; vline0 = d.buf.line_of(bl_anchor); vwant = d.want_col; vline_fix(d); return true; }
    if (k == "o") { std::swap(bl_anchor, d.cur); d.anchor = d.cur; return true; }
    if (k == "O") {
        int ca = col_at(d, bl_anchor), cc = col_at(d, d.cur);
        size_t la = d.buf.line_of(bl_anchor), lc = d.buf.line_of(d.cur);
        bl_anchor = pos_at_col(d, la, cc);
        d.cur = d.anchor = pos_at_col(d, lc, ca);
        return true;
    }
    if (k == "d" || k == "x" || k == "X" || k == "D") { size_t keepc = c0; cut(); leave(); setc(d, pos_at_col(d, l0, (int)keepc)); clamp(d); return true; }
    if (k == "y") { std::string p = pieces(); yank(p, false); reg_block = true; leave(); setc(d, pos_at_col(d, l0, c0)); return true; }
    if (k == "c" || k == "s") { cut(); start_insert('c'); return true; }
    if (k == "I") { start_insert('I'); return true; }
    if (k == "A") { start_insert('A'); return true; }
    if (k.size() == 2 && k[0] == 'r') {
        std::string ch = k.substr(1);
        rebuild([&](size_t, std::string &t, size_t a, size_t b, bool in) {
            if (!in) return;
            size_t cnt = 0;
            for (size_t i = a; i < b;) { size_t n = 1; while (i + n < b && ((unsigned char)t[i + n] & 0xC0) == 0x80) n++; i += n; cnt++; }
            std::string rep;
            for (size_t i = 0; i < cnt; i++) rep += ch;
            t.replace(a, b - a, rep);
        });
        leave(); setc(d, pos_at_col(d, l0, c0));
        return true;
    }
    if (k == "~" || k == "u" || k == "U") {
        char o = k[0];
        rebuild([&](size_t, std::string &t, size_t a, size_t b, bool in) { if (in) for (size_t i = a; i < b; i++) t[i] = o == 'u' ? (char)tolower((unsigned char)t[i]) : o == 'U' ? (char)toupper((unsigned char)t[i]) : swapcase(t[i]); });
        leave(); setc(d, pos_at_col(d, l0, c0));
        return true;
    }
    if (k == ">" || k == "<") { leave(); apply(d, k[0], {l0, l1, true}); return true; }
    if (k == ":") { save_vis(d); leave(); cmd_kind = ':'; cmd = "'<,'>"; return true; }
    return false;
}

void Vim::vline_fix(Doc &d) {
    size_t cl = d.buf.line_of(d.cur);
    if (cl >= vline0) { d.anchor = ls(d, vline0); d.cur = lend(d, cl); }
    else { d.anchor = lend(d, vline0); d.cur = ls(d, cl); }
    d.want_col = vwant;
}

void Vim::after_history(Doc &d) {
    d.anchor = d.cur;
    if (mode >= VISUAL) mode = NORMAL;
    size_t l = d.buf.line_of(d.cur);
    d.want_col = d.col_of_byte(l, d.cur - ls(d, l));
    clamp(d);
}

void Vim::half_page(Doc &d, int dir) {
    int n = std::max(1, (page_rows ? page_rows() : 20) / 2);
    size_t p = vert(d, d.cur, (long)dir * n);
    setc(d, p, true);
    if (mode == VLINE) vline_fix(d);
    clamp(d);
}

void Vim::escape(Doc &d) {
    if (macro_rec && !macro_play) macro_buf.push_back({2, std::string()});
    if (mode >= VISUAL) save_vis(d);
    sur = 0;
    sur_a.clear();
    reg_name = 0;
    clear_pending();
    cmd_kind = 0;
    cmd.clear();
    if (mode == INSERT || mode == REPLACE) {
        d.break_group();
        if (recording && !replaying) last_ins = ins_rec;
        recording = false;
        repl_stack.clear();
        if (blk.on) {   /* the text typed on the first line of a block insert goes into every other line */
            blk.on = false;
            size_t l0 = blk.l0, l1 = blk.l1;
            if (l1 > l0 && !ins_rec.empty() && ins_rec.find('\n') == std::string::npos) {
                std::string out, t;
                for (size_t l = l0 + 1; l <= l1; l++) {
                    d.buf.line_text(l, t);
                    size_t e = lend(d, l) - ls(d, l), at_b = e;
                    if (blk.kind == 'A' && !blk.eol) {
                        bool sh;
                        size_t p = pos_at_col(d, l, blk.c1 + 1, &sh);
                        if (sh) t.append((size_t)(blk.c1 + 1 - d.col_of_byte(l, e)), ' ');
                        at_b = sh ? t.size() : p - ls(d, l);
                    } else if (blk.kind != 'A') {
                        bool sh;
                        size_t p = pos_at_col(d, l, blk.c0, &sh);
                        if (sh) { out += t; if (l < l1) out += '\n'; continue; }
                        at_b = p - ls(d, l);
                    }
                    t.insert(at_b, ins_rec);
                    out += t;
                    if (l < l1) out += '\n';
                }
                size_t s0 = ls(d, l0 + 1), e0 = lend(d, l1);
                d.replace(s0, e0 - s0, out.data(), out.size());
                d.break_group();
            }
            d.cur = d.anchor = pos_at_col(d, l0, blk.c0);
        }
        mode = NORMAL;
        size_t l = d.buf.line_of(d.cur);
        if (d.cur > ls(d, l)) d.cur = d.anchor = d.prev_char(d.cur);
        d.anchor = d.cur;
        d.want_col = d.col_of_byte(l, d.cur - ls(d, l));
    } else if (mode >= VISUAL) {
        mode = NORMAL;
        d.anchor = d.cur;
        clamp(d);
    }
}

void Vim::cmd_enter(Doc &d) {
    char kind = cmd_kind;
    std::string s = cmd;
    cmd_kind = 0;
    cmd.clear();
    if (kind == '/' || kind == '?') {
        if (!s.empty()) last_pat = s;
        last_fwd = kind == '/';
        if (last_pat.empty()) return;
        size_t pos;
        if (search_go(d, last_fwd, last_fwd ? d.next_char(d.cur) : d.cur, &pos)) {
            marks['\''] = d.cur;
            setc(d, pos);
            if (mode == VLINE) vline_fix(d);
            clamp(d);
            if (on_search) on_search(last_pat);
        } else if (exec) exec('!', "E486: Pattern not found: " + last_pat);
        return;
    }
    if (exec) exec(kind, s);
}

void Vim::dot(Doc &d) {
    if (last_keys.empty()) return;
    replaying = true;
    std::vector<std::string> ks = last_keys;
    std::string ins = last_ins;
    for (const std::string &k : ks) text(d, k);
    if (typing()) {
        for (size_t i = 0; i < ins.size();) { size_t n = 1; while (i + n < ins.size() && ((unsigned char)ins[i + n] & 0xC0) == 0x80) n++; type_char(d, ins.substr(i, n)); i += n; }
        escape(d);
    }
    replaying = false;
}

void Vim::text(Doc &d, const std::string &s) {
    if (typing() || s.empty()) return;
    size_t first = 1;
    while (first < s.size() && ((unsigned char)s[first] & 0xC0) == 0x80) first++;
    if (first < s.size()) {   /* a paste or an input method can deliver several characters at once */
        for (size_t i = 0; i < s.size();) {
            size_t n = 1;
            while (i + n < s.size() && ((unsigned char)s[i + n] & 0xC0) == 0x80) n++;
            text(d, s.substr(i, n));
            i += n;
        }
        return;
    }
    if (sur == 'd' || sur == 'c' || sur == 'Y') { sur_key(d, s); return; }
    if (macro_rec && !macro_play) {
        if (idle() && s == "q") { macros[macro_rec] = macro_buf; macro_rec = 0; return; }
        macro_buf.push_back({0, s});
    }
    if (s == "." && idle()) { dot(d); return; }
    std::string k = s;
    if (!pend.empty()) { k = pend + s; pend.clear(); }
    if (!replaying && idle() && k.size() == s.size()) keys.clear();
    if (!replaying) keys.push_back(s);
    if (k.size() == s.size()) {
        char c = k[0];
        if (k.size() == 1 && ((c >= '1' && c <= '9') || (c == '0' && count > 0))) { count = count * 10 + (c - '0'); return; }
        bool objpfx = (c == 'i' || c == 'a') && (op || mode >= VISUAL);
        if (k.size() == 1 && (c == 'g' || c == 'f' || c == 'F' || c == 't' || c == 'T' || c == 'r' || c == 'z' || c == 'Z' || c == 'm' || c == '\'' || c == '`' || c == '@' || c == '"' || (c == 'q' && !macro_rec && idle()) || objpfx)) { pend = k; return; }
    }
    process(d, k);
}

void Vim::process(Doc &d, const std::string &k) {
    if (mode >= VISUAL) visual(d, k); else normal(d, k);
}

void Vim::normal(Doc &d, const std::string &k) {
    int n = count ? count : 1;
    bool hc = count > 0;
    size_t cur = d.cur, l = d.buf.line_of(cur), e = lend(d, l);
    if (k.size() == 2 && k[0] == '"') { reg_name = k[1]; return; }
    if (op) {
        int total = (opcount ? opcount : 1) * n;
        bool thc = opcount || hc;
        char o = op;
        clear_pending();
        R r{0, 0, false};
        auto finish_op = [&](R rr) {
            if (o == 'S') { sur_r = rr; sur = 'Y'; return; }   /* ys: now wait for the surrounding character */
            apply(d, o, rr);
            if (o != 'y') done_change();
        };
        if (k == "s" && (o == 'd' || o == 'c')) { sur = o; sur_a.clear(); return; }
        if (k == "s" && o == 'y') { op = 'S'; return; }
        if ((k.size() == 1 && k[0] == o) || (o == 'S' && k == "s") || (k.size() == 2 && k[0] == 'g' && k[1] == o)) {
            finish_op({l, std::min(l + (size_t)total - 1, d.buf.lines() - 1), true});
            return;
        }
        if (k.size() == 2 && (k[0] == 'i' || k[0] == 'a')) {
            if (object(d, k, r)) finish_op(r);
            return;
        }
        Mv m;
        if (o == 'c' && (k == "w" || k == "W") && cls(at(d, cur), k == "W") != 0) {   /* cw changes to the end of the word, like ce */
            size_t p = cur, N = d.buf.size();
            int c = cls(at(d, p), k == "W");
            for (int i = 0; i < total; i++) {
                if (i) { p = wfwd(d, p, k == "W"); if (p >= N) break; c = cls(at(d, p), k == "W"); }
                while (p + 1 < N && cls(at(d, p + 1), k == "W") == c) p++;
            }
            m.ok = true; m.pos = p; m.incl = true;
        } else m = motion(d, k, total, thc, true);
        if (!m.ok) return;
        if (m.line) { r = {std::min(l, d.buf.line_of(m.pos)), std::max(l, d.buf.line_of(m.pos)), true}; }
        else {
            size_t a = std::min(cur, m.pos), b = std::max(cur, m.pos);
            if (m.incl && b < d.buf.size()) b = d.next_char(b);
            r = {a, b, false};
        }
        if (m.ok && (k == "G" || k == "gg" || k == "%" || k == "n" || k == "N")) marks['\''] = cur;
        finish_op(r);
        return;
    }
    if (k == "gu" || k == "gU" || k == "g~") { op = k[1]; opcount = count; count = 0; return; }
    if (k == "gv") { clear_pending(); mode = vis_mode; d.anchor = std::min(vis_a, d.buf.size()); d.cur = std::min(vis_c, d.buf.size()); if (mode == VLINE) { vline0 = d.buf.line_of(std::min(d.anchor, d.cur)); vwant = d.want_col; vline_fix(d); } return; }
    if (k == "ZZ") { clear_pending(); if (exec) exec(':', "wq"); return; }
    if (k == "ZQ") { clear_pending(); if (exec) exec(':', "q!"); return; }
    if (k.size() == 2 && k[0] == 'z') {
        clear_pending();
        long rows = page_rows ? page_rows() : 20, ln = (long)l;
        if (k[1] == 'z' || k[1] == '.') d.top = (size_t)std::max(0L, ln - rows / 2);
        else if (k[1] == 't' || k[1] == '\r') d.top = (size_t)ln;
        else if (k[1] == 'b' || k[1] == '-') d.top = (size_t)std::max(0L, ln - rows + 1);
        return;
    }
    if (k.size() == 2 && k[0] == 'm') { clear_pending(); marks[k[1]] = cur; return; }
    if (k.size() == 2 && k[0] == 'q') { clear_pending(); if (isalpha((unsigned char)k[1])) { macro_rec = k[1]; macro_buf.clear(); } return; }
    if (k.size() == 2 && k[0] == '@') { int cnt = count ? count : 1; clear_pending(); char nm = k[1] == '@' ? last_macro : k[1]; if (nm) play_macro(d, nm, cnt); return; }
    char c = k[0];
    auto reset = [&] { clear_pending(); };
    switch (c) {
    case 'd': case 'y': case 'c': case '>': case '<':
        if (k.size() == 1) { op = c; opcount = count; count = 0; return; }
        break;
    case 'i': done_change(); reset(); enter_insert(d); return;
    case 'a': done_change(); reset(); if (e > ls(d, l)) setc(d, d.next_char(cur)); enter_insert(d); return;
    case 'I': done_change(); reset(); setc(d, first_nb(d, l)); enter_insert(d); return;
    case 'A': done_change(); reset(); setc(d, e); enter_insert(d); return;
    case 'o': case 'O': {
        done_change(); reset();
        d.break_group();
        std::string ind = slice(d, ls(d, l), first_nb(d, l));
        if (c == 'o') { std::string ins = "\n" + ind; d.replace(e, 0, ins.data(), ins.size()); setc(d, e + ins.size()); }
        else { std::string ins = ind + "\n"; d.replace(ls(d, l), 0, ins.data(), ins.size()); setc(d, ls(d, l) + ind.size()); }
        enter_insert(d);
        return;
    }
    case 'x': case 's': case 'X': {
        reset();
        if (e == ls(d, l)) { if (c == 's') { done_change(); enter_insert(d); } return; }
        size_t a = cur, b = cur;
        if (c == 'X') { for (int i = 0; i < n && a > ls(d, l); i++) a = d.prev_char(a); }
        else for (int i = 0; i < n && b < e; i++) b = d.next_char(b);
        done_change();
        apply(d, c == 's' ? 'c' : 'd', {a, b, false});
        return;
    }
    case 'D': case 'C': {
        reset();
        size_t tl = std::min(l + (size_t)n - 1, d.buf.lines() - 1);
        done_change();
        apply(d, c == 'D' ? 'd' : 'c', {cur, lend(d, tl), false});
        return;
    }
    case 'S': reset(); done_change(); apply(d, 'c', {l, std::min(l + (size_t)n - 1, d.buf.lines() - 1), true}); return;
    case 'Y': reset(); apply(d, 'y', {l, std::min(l + (size_t)n - 1, d.buf.lines() - 1), true}); return;
    case 'r':
        if (k.size() > 1) {
            std::string ch = k.substr(1);
            size_t b = cur;
            int got = 0;
            for (int i = 0; i < n && b < e; i++) { b = d.next_char(b); got++; }
            reset();
            if (got < n) return;
            std::string rep;
            for (int i = 0; i < n; i++) rep += ch;
            done_change();
            d.break_group();
            d.replace(cur, b - cur, rep.data(), rep.size());
            setc(d, cur + rep.size() - ch.size());
            d.break_group();
            return;
        }
        break;
    case 'J': {
        reset();
        size_t last = std::min(l + (size_t)std::max(1, n - 1), d.buf.lines() - 1);
        if (last == l) return;
        std::string out, t;
        size_t pos = 0;
        for (size_t i = l; i <= last; i++) {
            d.buf.line_text(i, t);
            if (i > l) { size_t k2 = t.find_first_not_of(" \t"); t = k2 == std::string::npos ? "" : t.substr(k2); if (!t.empty() && t[0] != ')' && !out.empty() && out.back() != ' ') out += ' '; pos = out.size(); if (!t.empty() && t[0] != ')' && pos) pos--; }
            out += t;
        }
        done_change();
        d.break_group();
        size_t s0 = ls(d, l), e0 = lend(d, last);
        d.replace(s0, e0 - s0, out.data(), out.size());
        setc(d, s0 + pos);
        d.break_group();
        return;
    }
    case '~': {
        reset();
        if (e == ls(d, l)) return;
        size_t b = cur;
        for (int i = 0; i < n && b < e; i++) b = d.next_char(b);
        done_change();
        std::string t = slice(d, cur, b);
        for (char &ch : t) ch = swapcase(ch);
        d.break_group();
        d.replace(cur, b - cur, t.data(), t.size());
        setc(d, b);
        clamp(d);
        d.break_group();
        return;
    }
    case 'p': case 'P': done_change(); reset(); paste(d, c == 'p', n); return;
    case 'u': reset(); d.undo(); after_history(d); return;
    case 'v': reset(); mode = VISUAL; d.anchor = d.cur; return;
    case '\x16': reset(); mode = VBLOCK; bl_anchor = cur; bl_eol = false; d.anchor = d.cur; return;
    case 'R': done_change(); reset(); mode = REPLACE; d.break_group(); recording = !replaying; ins_rec.clear(); repl_stack.clear(); return;
    case 'V': reset(); mode = VLINE; vline0 = l; vwant = d.want_col; vline_fix(d); return;
    case ':': reset(); cmd_kind = ':'; cmd.clear(); return;
    case '/': reset(); cmd_kind = '/'; cmd.clear(); return;
    case '?': reset(); cmd_kind = '?'; cmd.clear(); return;
    default: break;
    }
    Mv m = motion(d, k, n, hc, false);
    reset();
    if (!m.ok) return;
    if (k == "G" || k == "gg" || k == "%" || k == "n" || k == "N" || k == "*" || k == "#" || k == "{" || k == "}" || k[0] == '\'' || k[0] == '`') marks['\''] = cur;
    setc(d, m.pos, m.vert);
    clamp(d);
}

void Vim::visual(Doc &d, const std::string &k) {
    int n = count ? count : 1;
    bool hc = count > 0;
    char c = k[0];
    auto sel = [&]() -> R {
        size_t a = std::min(d.anchor, d.cur), b = std::max(d.anchor, d.cur);
        if (mode == VLINE) return {d.buf.line_of(a), d.buf.line_of(b), true};
        if (b < d.buf.size() && at(d, b) != '\n') b = d.next_char(b);
        return {a, b, false};
    };
    auto finish = [&](char o) {
        R r = sel();
        save_vis(d);
        clear_pending();
        mode = NORMAL;
        d.anchor = d.cur;
        apply(d, o, r);
    };
    if (k == "\x16" && mode != VBLOCK) { bl_anchor = d.anchor; mode = VBLOCK; clear_pending(); d.anchor = d.cur; bl_eol = false; return; }
    if (mode == VBLOCK && block(d, k)) return;
    if (k == "gu" || k == "gU" || k == "g~") { if (mode != VBLOCK) finish(k[1]); return; }
    if (k.size() == 2 && (k[0] == 'i' || k[0] == 'a')) {
        R r{0, 0, false};
        clear_pending();
        if (object(d, k, r) && r.line) {
            mode = VLINE;
            vline0 = r.a;
            vwant = d.want_col;
            d.cur = ls(d, std::min(r.b, d.buf.lines() - 1));
            vline_fix(d);
        } else if (r.b > r.a) {
            if (mode == VLINE) mode = VISUAL;
            d.anchor = r.a;
            d.cur = d.prev_char(r.b);
        }
        return;
    }
    if (mode != VBLOCK) switch (c) {
    case 'v': clear_pending(); if (mode == VISUAL) { mode = NORMAL; d.anchor = d.cur; clamp(d); } else { mode = VISUAL; } return;
    case 'V': clear_pending(); if (mode == VLINE) { mode = NORMAL; d.anchor = d.cur; clamp(d); } else { mode = VLINE; vline0 = d.buf.line_of(d.cur); vwant = d.want_col; vline_fix(d); } return;
    case 'o': clear_pending(); std::swap(d.anchor, d.cur); return;
    case 'd': case 'x': case 'X': finish('d'); done_change(); return;
    case 'y': finish('y'); return;
    case 'c': case 's': finish('c'); return;
    case '>': case '<': finish(c); return;
    case 'u': finish('u'); return;
    case 'U': finish('U'); return;
    case '~': finish('~'); return;
    case 'J': {
        R r = sel();
        clear_pending();
        mode = NORMAL;
        d.anchor = d.cur;
        size_t l0 = r.line ? r.a : d.buf.line_of(r.a), l1 = r.line ? r.b : d.buf.line_of(r.b);
        if (l1 == l0) l1 = std::min(l0 + 1, d.buf.lines() - 1);
        setc(d, ls(d, l0));
        for (size_t i = l0; i < l1; i++) text(d, "J");
        return;
    }
    case 'p': case 'P': {
        R r = sel();
        std::string saved = reg;
        bool sl = reg_line;
        if (get_clip) { std::string s = get_clip(); if (!s.empty() && s != reg_sys) { saved = s; sl = s.back() == '\n'; } }
        clear_pending();
        mode = NORMAL;
        d.anchor = d.cur;
        apply(d, 'd', r);
        reg = saved; reg_line = sl; reg_sys = get_clip ? get_clip() : saved;
        bool after = r.line ? d.buf.line_of(d.cur) < r.a : d.cur < r.a;
        paste(d, after, 1);
        return;
    }
    case 'S': { R r = sel(); save_vis(d); clear_pending(); mode = NORMAL; d.anchor = d.cur; sur_r = r; sur = 'Y'; return; }
    case ':': save_vis(d); clear_pending(); mode = NORMAL; d.anchor = d.cur; cmd_kind = ':'; cmd = "'<,'>"; return;
    case '/': clear_pending(); cmd_kind = '/'; cmd.clear(); return;
    case '?': clear_pending(); cmd_kind = '?'; cmd.clear(); return;
    default: break;
    }
    Mv m = motion(d, k, n, hc, false);
    clear_pending();
    if (!m.ok) return;
    if (mode == VBLOCK) { setc(d, m.pos, m.vert); d.anchor = d.cur; if (!m.vert) bl_eol = k == "$"; return; }
    if (mode == VLINE) { d.cur = m.pos; if (!m.vert) vwant = d.want_col; vline_fix(d); if (!m.vert) { size_t l = d.buf.line_of(d.cur); d.want_col = d.col_of_byte(l, m.pos - ls(d, l)); vwant = d.want_col; } return; }
    setc(d, m.pos, m.vert);
}
