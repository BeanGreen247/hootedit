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

}  // namespace

std::string Vim::label() const {
    const char *m = mode == INSERT ? "-- INSERT --" : mode == NORMAL ? "-- NORMAL --" : mode == VISUAL ? "-- VISUAL --" : "-- VISUAL LINE --";
    std::string s = m;
    std::string p;
    if (opcount) p += std::to_string(opcount);
    if (op) p += op;
    if (count) p += std::to_string(count);
    p += pend;
    if (!p.empty()) s += "   " + p;
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
    reg = t; reg_line = line; reg_sys = t;
    if (set_clip) set_clip(t);
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
    std::string text;
    if (r.line) {
        r.b = std::min(r.b, d.buf.lines() - 1);
        from = ls(d, r.a);
        to = r.b + 1 < d.buf.lines() ? ls(d, r.b + 1) : d.buf.size();
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
        if (r.line && to >= d.buf.size() && from > 0) a = from - 1;
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
    if (get_clip) { std::string s = get_clip(); if (!s.empty() && s != reg_sys) { t = s; line = s.back() == '\n'; } }
    if (t.empty()) return;
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
    clear_pending();
    cmd_kind = 0;
    cmd.clear();
    if (mode == INSERT) {
        d.break_group();
        if (recording && !replaying) last_ins = ins_rec;
        recording = false;
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
    if (exec) exec(kind, s);
}

void Vim::dot(Doc &d) {
    if (last_keys.empty()) return;
    replaying = true;
    std::vector<std::string> ks = last_keys;
    std::string ins = last_ins;
    for (const std::string &k : ks) text(d, k);
    if (mode == INSERT) {
        if (!ins.empty()) d.insert_text(ins.data(), ins.size());
        escape(d);
    }
    replaying = false;
}

void Vim::text(Doc &d, const std::string &s) {
    if (mode == INSERT || s.empty()) return;
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
    if (s == "." && idle()) { dot(d); return; }
    std::string k = s;
    if (!pend.empty()) { k = pend + s; pend.clear(); }
    if (!replaying && idle() && k.size() == s.size()) keys.clear();
    if (!replaying) keys.push_back(s);
    if (k.size() == s.size()) {
        char c = k[0];
        if (k.size() == 1 && ((c >= '1' && c <= '9') || (c == '0' && count > 0))) { count = count * 10 + (c - '0'); return; }
        bool objpfx = (c == 'i' || c == 'a') && (op || mode >= VISUAL);
        if (k.size() == 1 && (c == 'g' || c == 'f' || c == 'F' || c == 't' || c == 'T' || c == 'r' || objpfx)) { pend = k; return; }
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
    if (op) {
        int total = (opcount ? opcount : 1) * n;
        bool thc = opcount || hc;
        char o = op;
        clear_pending();
        R r{0, 0, false};
        if (k.size() == 1 && k[0] == o) {
            r = {l, std::min(l + (size_t)total - 1, d.buf.lines() - 1), true};
            apply(d, o, r);
            if (o != 'y') done_change();
            return;
        }
        if (k.size() == 2 && (k[0] == 'i' || k[0] == 'a')) {
            if (object(d, k, r)) { apply(d, o, r); if (o != 'y') done_change(); }
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
        apply(d, o, r);
        if (o != 'y') done_change();
        return;
    }
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
    case 'V': reset(); mode = VLINE; vline0 = l; vwant = d.want_col; vline_fix(d); return;
    case ':': reset(); cmd_kind = ':'; cmd.clear(); return;
    case '/': reset(); cmd_kind = '/'; cmd.clear(); return;
    case 'n': reset(); if (search_next) search_next(); return;
    default: break;
    }
    Mv m = motion(d, k, n, hc, false);
    reset();
    if (!m.ok) return;
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
        clear_pending();
        mode = NORMAL;
        d.anchor = d.cur;
        apply(d, o, r);
    };
    if (k.size() == 2 && (k[0] == 'i' || k[0] == 'a')) {
        R r{0, 0, false};
        clear_pending();
        if (object(d, k, r) && r.b > r.a) {
            if (mode == VLINE) mode = VISUAL;
            d.anchor = r.a;
            d.cur = d.prev_char(r.b);
        }
        return;
    }
    switch (c) {
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
    case ':': clear_pending(); cmd_kind = ':'; cmd.clear(); return;
    case '/': clear_pending(); cmd_kind = '/'; cmd.clear(); return;
    case 'n': clear_pending(); if (search_next) search_next(); return;
    default: break;
    }
    Mv m = motion(d, k, n, hc, false);
    clear_pending();
    if (!m.ok) return;
    if (mode == VLINE) { d.cur = m.pos; if (!m.vert) vwant = d.want_col; vline_fix(d); if (!m.vert) { size_t l = d.buf.line_of(d.cur); d.want_col = d.col_of_byte(l, m.pos - ls(d, l)); vwant = d.want_col; } return; }
    setc(d, m.pos, m.vert);
}
