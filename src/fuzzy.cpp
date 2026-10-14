/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "fuzzy.h"

#include <string.h>

#include <algorithm>

namespace {

const int SCORE_MATCH = 16, GAP_START = -3, GAP_EXT = -1;
const int BONUS_BOUNDARY = SCORE_MATCH / 2;                 /* 8 */
const int BONUS_WHITE = BONUS_BOUNDARY + 2, BONUS_DELIM = BONUS_BOUNDARY + 1;
const int BONUS_CAMEL = BONUS_BOUNDARY + GAP_EXT;           /* 7 */
const int BONUS_CONSEC = -(GAP_START + GAP_EXT);            /* 4 */
const int FIRST_MULT = 2;

enum CC { C_WHITE, C_DELIM, C_NONWORD, C_LOWER, C_UPPER, C_NUMBER };

CC cclass(unsigned char c) {
    if (c >= 'a' && c <= 'z') return C_LOWER;
    if (c >= 'A' && c <= 'Z') return C_UPPER;
    if (c >= '0' && c <= '9') return C_NUMBER;
    if (c >= 0x80) return C_LOWER;                           /* UTF-8 bytes count as letters */
    if (c == ' ' || c == '\t') return C_WHITE;
    if (c == '/' || c == ',' || c == ':' || c == ';' || c == '|') return C_DELIM;
    return C_NONWORD;
}

int bonus_for(CC prev, CC cur) {
    if (cur >= C_LOWER) {
        if (prev == C_WHITE) return BONUS_WHITE;
        if (prev == C_DELIM) return BONUS_DELIM;
        if (prev == C_NONWORD) return BONUS_BOUNDARY;
    }
    if ((prev == C_LOWER && cur == C_UPPER) || (prev != C_NUMBER && cur == C_NUMBER)) return BONUS_CAMEL;
    if (cur == C_NONWORD || cur == C_DELIM) return BONUS_BOUNDARY;
    if (cur == C_WHITE) return BONUS_WHITE;
    return 0;
}

inline unsigned char fold(unsigned char c, bool icase) { return icase && c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c; }

}  // namespace

bool fuzzy_term(const char *pat, size_t m, const char *text, size_t n, bool icase, int *score, std::vector<int> *positions) {
    if (m == 0) { *score = 0; if (positions) positions->clear(); return true; }
    if (m > n) return false;
    /* forward scan: does the pattern occur as a subsequence, and where does the earliest match end */
    size_t pi = 0, last = 0;
    for (size_t i = 0; i < n && pi < m; i++) if (fold((unsigned char)text[i], icase) == fold((unsigned char)pat[pi], icase)) { pi++; last = i; }
    if (pi < m) return false;
    /* backward scan from that end to find the latest start: the window the dynamic programme has to look at */
    size_t first = last;
    pi = m;
    for (size_t i = last + 1; i-- > 0 && pi > 0;) if (fold((unsigned char)text[i], icase) == fold((unsigned char)pat[pi - 1], icase)) { pi--; first = i; }
    size_t w = last - first + 1;
    /* per position bonus */
    std::vector<int> bonus(w);
    {
        CC prev = first == 0 ? C_DELIM : cclass((unsigned char)text[first - 1]);
        for (size_t j = 0; j < w; j++) { CC cur = cclass((unsigned char)text[first + j]); bonus[j] = bonus_for(prev, cur); prev = cur; }
    }
    /* H[i][j]: best score with pattern[i] matched exactly at window position j (-1 = impossible).
     * G[i][j]: best score of pattern[0..i] with its last match at or before j, including the gap penalty carried to j.
     * run[i][j]: the bonus the current consecutive run was started with (fzf keeps a boundary bonus alive along a run). */
    const int NEG = -1000000;
    std::vector<int> H(m * w, NEG), G(m * w, NEG), run(m * w, 0);
    std::vector<unsigned char> from_diag(m * w, 0);   /* H[i][j] extended a consecutive match from H[i-1][j-1] */
    for (size_t i = 0; i < m; i++) {
        unsigned char pc = fold((unsigned char)pat[i], icase);
        for (size_t j = i; j < w; j++) {
            bool eq = fold((unsigned char)text[first + j], icase) == pc;
            int h = NEG;
            if (eq) {
                if (i == 0) {
                    h = SCORE_MATCH + bonus[j] * FIRST_MULT;
                    run[i * w + j] = bonus[j];
                } else if (j > 0) {
                    int diag = H[(i - 1) * w + (j - 1)];
                    int via_gap = j >= 2 ? G[(i - 1) * w + (j - 2)] : NEG;     /* skip at least one character since the previous match */
                    int cand_c = NEG, cand_g = NEG, rb_c = 0;
                    if (diag > NEG) {
                        int prb = run[(i - 1) * w + (j - 1)];
                        int b = bonus[j];
                        int rb = prb;
                        if (rb >= BONUS_BOUNDARY && rb > b) b = rb;           /* a run that began on a boundary keeps that bonus */
                        else b = std::max(b, BONUS_CONSEC);
                        cand_c = diag + SCORE_MATCH + b;
                        rb_c = std::max(prb, b);
                    }
                    if (via_gap > NEG) cand_g = via_gap + SCORE_MATCH + bonus[j];
                    if (cand_c >= cand_g) { h = cand_c; run[i * w + j] = rb_c; from_diag[i * w + j] = 1; }
                    else { h = cand_g; run[i * w + j] = bonus[j]; }
                }
            }
            H[i * w + j] = h;
            int g = NEG;
            if (j > 0) {
                int prev_g = G[i * w + (j - 1)];
                if (prev_g > NEG) g = prev_g + (G[i * w + (j - 1)] == H[i * w + (j - 1)] ? GAP_START : GAP_EXT);
            }
            G[i * w + j] = std::max(h, g);
            if (h == NEG && g == NEG) G[i * w + j] = NEG;
        }
    }
    int best = NEG;
    size_t bj = 0;
    for (size_t j = m - 1; j < w; j++) if (H[(m - 1) * w + j] > best) { best = H[(m - 1) * w + j]; bj = j; }
    if (best <= NEG) return false;
    *score = best;
    if (positions) {
        positions->assign(m, 0);
        size_t j = bj;
        for (size_t i = m; i-- > 0;) {
            (*positions)[i] = (int)(first + j);
            if (i == 0) break;
            if (from_diag[i * w + j]) { j--; continue; }
            /* came through a gap: the previous match is the best H[i-1][k] for k <= j-2 that produced via_gap */
            size_t k = j >= 2 ? j - 2 : 0;
            int target = G[(i - 1) * w + k];
            size_t pick = k;
            for (size_t q = k + 1; q-- > 0;) if (H[(i - 1) * w + q] > NEG && (H[(i - 1) * w + q] >= target - (int)(k - q) * 2 - 3 || q == 0)) { pick = q; break; }
            j = pick;
        }
    }
    return true;
}

void FuzzyQuery::parse(const std::string &q) {
    terms.clear();
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && q[i] == ' ') i++;
        size_t e = i;
        while (e < q.size() && q[e] != ' ') e++;
        if (e == i) break;
        std::string t = q.substr(i, e - i);
        i = e;
        Term term;
        bool inv = false;
        if (t.size() > 1 && t[0] == '!') { inv = true; t = t.substr(1); }
        if (!t.empty() && t[0] == '\'') { t = t.substr(1); term.kind = Term::EXACT; }
        else if (!t.empty() && t[0] == '^') { t = t.substr(1); term.kind = Term::PREFIX; }
        else if (t.size() > 1 && t.back() == '$') { t.pop_back(); term.kind = Term::SUFFIX; }
        else if (inv) term.kind = Term::EXACT;
        if (inv) term.kind = term.kind == Term::PREFIX ? Term::INV_PREFIX : term.kind == Term::SUFFIX ? Term::INV_SUFFIX : Term::INVERSE;
        if (t.empty()) continue;
        term.icase = true;
        for (char c : t) if (c >= 'A' && c <= 'Z') term.icase = false;   /* smart case */
        term.text = t;
        terms.push_back(term);
    }
}

bool FuzzyQuery::match(const char *text, size_t n, int *score, std::vector<int> *positions) const {
    int total = 0;
    if (positions) positions->clear();
    for (const Term &t : terms) {
        int sc = 0;
        std::vector<int> pos;
        bool ok = false;
        const size_t m = t.text.size();
        auto eqat = [&](size_t at) {
            for (size_t k = 0; k < m; k++) if (fold((unsigned char)text[at + k], t.icase) != fold((unsigned char)t.text[k], t.icase)) return false;
            return true;
        };
        auto find_exact = [&]() -> long { for (size_t at = 0; at + m <= n; at++) if (eqat(at)) return (long)at; return -1; };
        switch (t.kind) {
        case FuzzyQuery::Term::FUZZY: ok = fuzzy_term(t.text.data(), m, text, n, t.icase, &sc, &pos); break;
        case FuzzyQuery::Term::EXACT: { long at = find_exact(); ok = at >= 0; if (ok) { sc = (int)m * SCORE_MATCH + (at == 0 ? BONUS_BOUNDARY : 0); for (size_t k = 0; k < m; k++) pos.push_back((int)at + (int)k); } break; }
        case FuzzyQuery::Term::PREFIX: ok = m <= n && eqat(0); if (ok) { sc = (int)m * SCORE_MATCH + BONUS_BOUNDARY * FIRST_MULT; for (size_t k = 0; k < m; k++) pos.push_back((int)k); } break;
        case FuzzyQuery::Term::SUFFIX: ok = m <= n && eqat(n - m); if (ok) { sc = (int)m * SCORE_MATCH; for (size_t k = 0; k < m; k++) pos.push_back((int)(n - m + k)); } break;
        case FuzzyQuery::Term::INVERSE: ok = find_exact() < 0; break;
        case FuzzyQuery::Term::INV_PREFIX: ok = !(m <= n && eqat(0)); break;
        case FuzzyQuery::Term::INV_SUFFIX: ok = !(m <= n && eqat(n - m)); break;
        }
        if (!ok) return false;
        total += sc;
        if (positions) positions->insert(positions->end(), pos.begin(), pos.end());
    }
    if (positions) { std::sort(positions->begin(), positions->end()); positions->erase(std::unique(positions->begin(), positions->end()), positions->end()); }
    *score = total;
    return true;
}
