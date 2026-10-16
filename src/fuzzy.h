/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_FUZZY_H
#define HE_FUZZY_H
#include <string>
#include <vector>

/* fzf-style fuzzy matching, written from the published description of its scoring (not copied code): every pattern character
 * must appear in order; the score rewards matches at word starts (after space, '/', '_', '-', '.'), at camelCase humps and right
 * after the previous match, and penalises gaps. Smart case: an upper-case letter in the pattern makes it case sensitive.
 *
 * Extended syntax, like fzf: terms separated by spaces must all match;  'abc  exact substring;  ^abc  prefix;  abc$  suffix;
 * !abc  must not contain;  !^abc and !abc$ negate prefix and suffix. */
struct FuzzyQuery {
    struct Term { std::string text; enum Kind { FUZZY, EXACT, PREFIX, SUFFIX, INVERSE, INV_PREFIX, INV_SUFFIX } kind = FUZZY; bool icase = true; };
    std::vector<Term> terms;
    void parse(const std::string &q);                      /* empty query: no terms, everything matches with score 0 */
    bool empty() const { return terms.empty(); }
    /* false if the text does not match; otherwise score (higher is better) and the matched byte positions for highlighting */
    bool match(const char *text, size_t n, int *score, std::vector<int> *positions) const;
};

/* one fuzzy term against one text; exposed for tests */
bool fuzzy_term(const char *pat, size_t m, const char *text, size_t n, bool icase, int *score, std::vector<int> *positions);
#endif
