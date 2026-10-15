/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_VIM_H
#define HE_VIM_H
#include <functional>
#include <string>
#include <vector>

#include "doc.h"

/* Modal editing on top of Doc. The host feeds it the text of every key typed outside insert mode; it edits the Doc
 * itself. No UI here: the host draws the mode label, the block cursor and the command line. */
class Vim {
public:
    enum Mode { INSERT, NORMAL, VISUAL, VLINE };
    Mode mode = NORMAL;

    /* host callbacks */
    std::function<void(const std::string &)> set_clip;
    std::function<std::string()> get_clip;
    std::function<void(char kind, const std::string &text)> exec;   /* Enter on the command line: kind ':' or '/' */
    std::function<void()> search_next;                               /* n */
    std::function<int()> page_rows;

    /* command line: kind 0 = closed */
    char cmd_kind = 0;
    std::string cmd;

    void reset(bool enabled) { mode = enabled ? NORMAL : INSERT; clear_pending(); cmd_kind = 0; cmd.clear(); }
    void text(Doc &d, const std::string &s);   /* one typed key (or utf-8 char), not in insert mode */
    void escape(Doc &d);                       /* Esc / Ctrl+[ in any mode */
    void typed(const std::string &s) { if (recording) ins_rec += s; }   /* insert-mode text, remembered for "." */
    void erased() { if (recording && !ins_rec.empty()) ins_rec.pop_back(); }
    void after_history(Doc &d);                 /* after undo / redo from outside */
    void half_page(Doc &d, int dir);
    void cmd_text(const std::string &s) { cmd += s; }
    void cmd_back() { if (cmd.empty()) cmd_kind = 0; else cmd.pop_back(); }
    void cmd_enter(Doc &d);
    void cmd_cancel() { cmd_kind = 0; cmd.clear(); }

    std::string label() const;                  /* "-- NORMAL --", with the pending keys */
    bool block_cursor() const { return mode != INSERT; }

private:
    struct R { size_t a, b; bool line; };
    struct Mv { size_t pos = 0; bool ok = false, line = false, incl = false, vert = false; };

    void clear_pending() { count = opcount = 0; op = 0; pend.clear(); }
    bool idle() const { return !op && !count && pend.empty() && mode < VISUAL; }
    void process(Doc &d, const std::string &k);
    void normal(Doc &d, const std::string &k);
    void visual(Doc &d, const std::string &k);
    Mv motion(Doc &d, const std::string &k, int n, bool has_count, bool opmode);
    bool object(Doc &d, const std::string &k, R &r);
    void apply(Doc &d, char o, R r);
    void yank(const std::string &t, bool line);
    void paste(Doc &d, bool after, int n);
    void enter_insert(Doc &d);
    void done_change();
    void setc(Doc &d, size_t p, bool keep_want = false);
    void clamp(Doc &d);
    void vline_fix(Doc &d);
    void dot(Doc &d);
    size_t vert(Doc &d, size_t from, long delta);

    int count = 0, opcount = 0;
    char op = 0;
    std::string pend;
    std::string reg, reg_sys;
    bool reg_line = false;
    char fch_kind = 0;
    std::string fch;
    std::vector<std::string> keys, last_keys;
    std::string ins_rec, last_ins;
    bool recording = false, replaying = false;
    size_t vline0 = 0;
    int vwant = 0;
};
#endif
