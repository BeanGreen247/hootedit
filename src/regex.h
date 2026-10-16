/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_REGEX_H
#define HE_REGEX_H
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/* Own regular-expression engine for search. No backtracking, so a pattern can never hang the editor.
 *
 * Syntax: literals, . (one UTF-8 character), [a-z] [^...] classes, \d \w \s and their capitals, \b \B, ^ $ (line start/end),
 * | alternation, (...) and (?:...) groups, * + ? {n} {n,} {n,m} with a lazy "?" suffix, \t \n \r \xHH, and a leading (?i).
 * Matching is leftmost-first like Perl/JavaScript. Text is treated as UTF-8 bytes; case folding is ASCII only.
 *
 * Three matchers share one compiled program: a literal prefilter (memmem) that skips straight to candidate lines, a lazy DFA that
 * decides "does this line match" at about one table lookup per byte, and a Pike VM that finds the exact span only on lines that match. */
class Regex {
public:
    bool compile(const std::string &pattern, bool icase, std::string *err);
    bool is_literal() const { return literal_; }                 /* the whole pattern is one plain string (letters may be case-folded, see lit_icase) */
    const std::string &literal() const { return lit_; }          /* that string, or the longest string every match must contain (lower-case when lit_icase) */
    bool lit_icase() const { return lit_icase_; }
    struct Prog;
    std::shared_ptr<const Prog> prog;
    std::shared_ptr<Regex> relaxed;                              /* for patterns with \b: same pattern with the word-boundary tests removed, so the DFA can pre-select lines */
private:
    bool literal_ = false, lit_icase_ = false;
    std::string lit_;
};

/* Per-thread matching state (DFA cache, VM scratch). Cheap to create; do not share one between threads. */
class Matcher {
public:
    explicit Matcher(const Regex &re);
    ~Matcher();
    /* leftmost match in s[0,n) at or after `from`; text is one line (no '\n' handling) */
    bool find(const char *s, size_t n, size_t from, size_t *ms, size_t *me);
    bool test(const char *s, size_t n);
    /* every line of buf[0,n) that contains a match: cb(line_start, line_end) with line_end at the '\n' or n; return false from cb to stop */
    void scan(const char *buf, size_t n, const std::function<bool(size_t, size_t)> &cb);
    uint64_t dfa_states() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
#endif
