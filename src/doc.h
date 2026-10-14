/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_DOC_H
#define HE_DOC_H
#include <stdint.h>
#include <string>
#include <vector>

#include "buffer.h"
#include "syntax.h"

/* One open file: buffer, cursor and selection, undo history, language and the cached lexer state per line. */
class Doc {
public:
    bool open(const char *path);              /* empty path = untitled */
    bool save(const char *path_or_null);      /* null = own path; on success adopts the path and re-detects the language */

    Buffer buf;
    std::string path;
    int untitled = 0;                         /* number for "Untitled-N" when path is empty */
    size_t cur = 0, anchor = 0;               /* anchor == cur: no selection */
    int want_col = 0;
    size_t top = 0;
    int left = 0;
    const Lang *lang = lang_plain();
    bool lang_forced = false;
    std::string eol = "LF";                   /* "LF" or "CRLF", from the first line break when the file was opened */
    std::string indent = "Spaces: 4";         /* status-bar label guessed from the first indented lines */

    std::string title;                        /* shown instead of the file name when set (diff views) */
    void load_text(const std::string &text);  /* replace everything, clean state, no undo history */
    std::string name() const;
    bool has_sel() const { return anchor != cur; }
    size_t sel_lo() const { return anchor < cur ? anchor : cur; }
    size_t sel_hi() const { return anchor < cur ? cur : anchor; }
    bool modified() const { return undo_.size() != save_point_; }
    bool pristine() const { return path.empty() && buf.size() == 0 && undo_.empty() && redo_.empty(); }

    /* every edit goes through replace(): one undo record, cursor placed after the new text */
    void replace(size_t pos, size_t del, const char *ins, size_t n);
    void insert_text(const char *s, size_t n);          /* replaces the selection if any */
    void erase_back();                                  /* backspace: selection or previous character */
    void erase_fwd();
    bool undo();
    bool redo();
    void break_group() { group_open_ = false; }          /* next edit starts a new undo step */
    void move_to(size_t pos, bool extend, bool keep_want);
    void select_all() { anchor = 0; cur = buf.size(); want_col = 0; }
    std::string selection() const;

    /* lexer state at the start of `line` (valid for lines already reached), computed forward on demand */
    uint8_t line_state(size_t line);
    void set_language(const Lang *l, bool forced);
    void redetect();

    size_t prev_char(size_t pos) const;
    size_t next_char(size_t pos) const;
    int col_of_byte(size_t line, size_t byte) const;

private:
    struct Op { size_t pos; std::string del, ins; size_t cur_before, cur_after; };
    void apply(size_t pos, size_t del, const char *ins, size_t n);
    void invalidate(size_t pos);
    void push(Op &&op);

    std::vector<Op> undo_, redo_;
    bool group_open_ = false;
    size_t save_point_ = 0;
    std::vector<uint8_t> states_ = {0};                  /* states_[l] = lexer state at start of line l, for l < states_.size() */
    std::string scratch_;
    std::vector<uint8_t> tok_;
};
#endif
