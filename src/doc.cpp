/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "doc.h"

#include <string.h>

#include <algorithm>

static const size_t STATE_JUMP = 200000;   /* farther than this many lines from the known state: assume state 0, correct it when the user scrolls back */

void Doc::load_text(const std::string &text) {
    if (buf.size()) buf.erase(0, buf.size());
    if (!text.empty()) buf.insert(0, text.data(), text.size());
    undo_.clear(); redo_.clear();
    save_point_ = 0;
    group_open_ = false;
    cur = anchor = 0;
    top = 0;
    left = 0;
    states_.assign(1, 0);
}

std::string Doc::name() const {
    if (!title.empty()) return title;
    if (path.empty()) return "Untitled-" + std::to_string(untitled ? untitled : 1);
    size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

void Doc::redetect() {
    if (lang_forced) return;
    std::string first;
    if (buf.size()) buf.copy(0, std::min<size_t>(buf.size(), 160), first);
    size_t nl = first.find('\n');
    if (nl != std::string::npos) first.resize(nl);
    lang = lang_detect(path.c_str(), first.data(), first.size());
    states_.assign(1, 0);
}

bool Doc::open(const char *p) {
    path = p ? p : "";
    if (!path.empty() && !buf.open(path.c_str())) return false;
    redetect();
    if (buf.size()) {                                  /* line ending and indent style from the top of the file */
        std::string line;
        if (buf.lines() > 1) {
            buf.copy(buf.line_start(1) - 2, 2, line);
            if (line.size() == 2 && line[0] == '\r') eol = "CRLF";
        }
        size_t spaces_min = 0;
        bool tab = false;
        for (size_t l = 0; l < buf.lines() && l < 200; l++) {
            buf.line_text(l, line);
            if (line.empty()) continue;
            if (line[0] == '\t') { tab = true; break; }
            size_t n = 0;
            while (n < line.size() && line[n] == ' ') n++;
            if (n >= 2 && n < line.size() && line[n] != '*' && (spaces_min == 0 || n < spaces_min)) spaces_min = n;   /* a lone space is a comment continuation */
        }
        if (tab) indent = "Tab Size: 4"; else if (spaces_min) indent = "Spaces: " + std::to_string(spaces_min);
    }
    return true;
}

bool Doc::save(const char *p) {
    std::string target = p ? p : path;
    if (target.empty()) return false;
    if (!buf.save(target.c_str())) return false;
    bool renamed = target != path;
    path = target;
    break_group();
    save_point_ = undo_.size();
    if (renamed) redetect();
    return true;
}

void Doc::set_language(const Lang *l, bool forced) {
    lang_forced = forced;
    if (forced) lang = l; else { lang_forced = false; redetect(); }
    states_.assign(1, 0);
}

void Doc::invalidate(size_t pos) {
    size_t l = buf.line_of(std::min(pos, buf.size()));
    if (states_.size() > l + 1) states_.resize(l + 1);
}

void Doc::apply(size_t pos, size_t del, const char *ins, size_t n) {
    if (del) buf.erase(pos, del);
    if (n) buf.insert(pos, ins, n);
    invalidate(pos);
}

void Doc::push(Op &&op) {
    redo_.clear();
    if (undo_.size() < save_point_) save_point_ = (size_t)-1;   /* content can never match the saved file again by size alone */
    if (group_open_ && !undo_.empty() && undo_.size() != save_point_) {
        Op &p = undo_.back();
        bool pure_ins = op.del.empty() && p.del.empty();
        bool pure_del = op.ins.empty() && p.ins.empty();
        if (pure_ins && op.pos == p.pos + p.ins.size() && op.ins.find('\n') == std::string::npos) { p.ins += op.ins; p.cur_after = op.cur_after; return; }
        if (pure_del && op.pos + op.del.size() == p.pos) { p.pos = op.pos; p.del = op.del + p.del; p.cur_after = op.cur_after; return; }   /* backspace run */
        if (pure_del && op.pos == p.pos) { p.del += op.del; p.cur_after = op.cur_after; return; }                                           /* delete run */
    }
    undo_.push_back(std::move(op));
    group_open_ = true;
}

void Doc::replace(size_t pos, size_t del, const char *ins, size_t n) {
    if (pos > buf.size()) pos = buf.size();
    if (del > buf.size() - pos) del = buf.size() - pos;
    if (!del && !n) return;
    Op op;
    op.pos = pos;
    op.cur_before = cur;
    if (del) buf.copy(pos, del, op.del);
    op.ins.assign(ins, n);
    apply(pos, del, ins, n);
    cur = anchor = pos + n;
    op.cur_after = cur;
    push(std::move(op));
    want_col = col_of_byte(buf.line_of(cur), cur - buf.line_start(buf.line_of(cur)));
}

void Doc::insert_text(const char *s, size_t n) {
    if (has_sel()) { size_t lo = sel_lo(); replace(lo, sel_hi() - lo, s, n); break_group(); }
    else replace(cur, 0, s, n);
}

void Doc::erase_back() {
    if (has_sel()) { size_t lo = sel_lo(); replace(lo, sel_hi() - lo, "", 0); break_group(); return; }
    size_t p = prev_char(cur);
    if (p < cur) replace(p, cur - p, "", 0);
}

void Doc::erase_fwd() {
    if (has_sel()) { size_t lo = sel_lo(); replace(lo, sel_hi() - lo, "", 0); break_group(); return; }
    size_t n = next_char(cur);
    if (n > cur) { size_t keep = cur; replace(keep, n - keep, "", 0); }
}

bool Doc::undo() {
    if (undo_.empty()) return false;
    Op op = std::move(undo_.back());
    undo_.pop_back();
    apply(op.pos, op.ins.size(), op.del.data(), op.del.size());
    cur = anchor = op.cur_before;
    redo_.push_back(std::move(op));
    group_open_ = false;
    move_to(cur, false, false);
    return true;
}

bool Doc::redo() {
    if (redo_.empty()) return false;
    Op op = std::move(redo_.back());
    redo_.pop_back();
    apply(op.pos, op.del.size(), op.ins.data(), op.ins.size());
    cur = anchor = op.cur_after;
    undo_.push_back(std::move(op));
    group_open_ = false;
    move_to(cur, false, false);
    return true;
}

void Doc::move_to(size_t pos, bool extend, bool keep_want) {
    cur = std::min(pos, buf.size());
    if (!extend) anchor = cur;
    if (!keep_want) {
        size_t l = buf.line_of(cur);
        want_col = col_of_byte(l, cur - buf.line_start(l));
    }
}

std::string Doc::selection() const {
    std::string s;
    if (has_sel()) buf.copy(sel_lo(), sel_hi() - sel_lo(), s);
    return s;
}

size_t Doc::prev_char(size_t pos) const {
    if (!pos) return 0;
    size_t start = pos > 4 ? pos - 4 : 0;
    std::string t;
    buf.copy(start, pos - start, t);
    size_t i = t.size() - 1;
    while (i > 0 && ((unsigned char)t[i] & 0xc0) == 0x80) i--;
    return start + i;
}

size_t Doc::next_char(size_t pos) const {
    if (pos >= buf.size()) return pos;
    std::string t;
    buf.copy(pos, 4, t);
    unsigned char c = (unsigned char)t[0];
    return pos + (c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1);
}

int Doc::col_of_byte(size_t line, size_t byte) const {
    std::string t;
    buf.line_text(line, t);
    int col = 0;
    for (size_t i = 0; i < t.size() && i < byte;) {
        unsigned char c = (unsigned char)t[i];
        size_t a = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        col = c == '\t' ? (col / 4 + 1) * 4 : col + 1;
        i += a;
    }
    return col;
}

uint8_t Doc::line_state(size_t line) {
    if (line >= buf.lines()) line = buf.lines() - 1;
    if (lang == lang_plain()) return 0;
    if (states_.size() > line) return states_[line];
    size_t known = states_.size();                         /* state of line `known` is not computed yet; line known-1 is */
    if (line - (known - 1) > STATE_JUMP) { states_.resize(line + 1, 0); return 0; }
    while (states_.size() <= line) {
        size_t l = states_.size() - 1;
        buf.line_text(l, scratch_);
        if (tok_.size() < scratch_.size()) tok_.resize(scratch_.size() + 64);
        states_.push_back(lex_line(lang, scratch_.data(), scratch_.size(), states_[l], tok_.data()));
    }
    return states_[line];
}
