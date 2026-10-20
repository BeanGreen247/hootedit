/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_LINEINDEX_H
#define HE_LINEINDEX_H
#include <stddef.h>
#include <algorithm>
#include <cmath>
#include <vector>

/* Line start offsets in chunks, each with its own pending delta. A flat vector costs O(lines) per edit (measured
 * 5 ms per keystroke in the middle of a 14.5M line file); here an edit patches one chunk and bumps the deltas of
 * the later ones, O(chunk + chunks). Unsigned wrap-around arithmetic makes negative deltas work. */
class LineIndex {
public:
    void reset(const std::vector<size_t> &starts) {
        /* an edit moves about chunk/2 entries inside its chunk and bumps the delta of every later chunk (lines/chunk of them): the cheapest
         * size is about sqrt(lines), 700 for a 480k-line file, 3,800 for 14.5M lines */
        chunk_ = std::max<size_t>(256, std::min<size_t>(4096, (size_t)std::sqrt((double)starts.size())));
        chunks_.clear();
        for (size_t i = 0; i < starts.size(); i += chunk_) {
            chunks_.emplace_back();
            chunks_.back().assign(starts.begin() + (long)i, starts.begin() + (long)std::min(starts.size(), i + chunk_));
        }
        if (chunks_.empty()) chunks_.push_back({0});
        deltas_.assign(chunks_.size(), 0);
        renumber(0);
    }
    size_t count() const { return total_; }
    size_t at(size_t line) const {
        size_t ci = chunk_of_line(line);
        return chunks_[ci][line - firsts_[ci]] + deltas_[ci];
    }
    /* index of the last line whose start is <= pos */
    size_t line_of(size_t pos) const {
        size_t lo = 0, hi = chunks_.size();                       /* last chunk whose first start <= pos */
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (chunks_[mid][0] + deltas_[mid] <= pos) lo = mid; else hi = mid;
        }
        const std::vector<size_t> &c = chunks_[lo];
        size_t idx = (size_t)(std::upper_bound(c.begin(), c.end(), pos - deltas_[lo]) - c.begin());
        return firsts_[lo] + (idx ? idx - 1 : 0);
    }
    /* add d to every line start from line `from` on */
    void shift_from(size_t from, size_t d) {
        if (from >= total_) return;
        size_t ci = chunk_of_line(from);
        std::vector<size_t> &c = chunks_[ci];
        for (size_t k = from - firsts_[ci]; k < c.size(); k++) c[k] += d;
        size_t *dl = deltas_.data();
        for (size_t k = ci + 1, n = deltas_.size(); k < n; k++) dl[k] += d;   /* one flat array: the loop vectorizes */
    }
    void erase(size_t a, size_t b) {                             /* drop lines [a, b) */
        while (a < b) {
            size_t ci = chunk_of_line(a);
            std::vector<size_t> &c = chunks_[ci];
            size_t k0 = a - firsts_[ci], k1 = std::min(c.size(), k0 + (b - a));
            c.erase(c.begin() + (long)k0, c.begin() + (long)k1);
            b -= k1 - k0;
            if (c.empty() && chunks_.size() > 1) { chunks_.erase(chunks_.begin() + (long)ci); deltas_.erase(deltas_.begin() + (long)ci); }
            renumber(ci ? ci - 1 : 0);
        }
    }
    void insert(size_t at, const std::vector<size_t> &abs) {     /* new lines before line `at`, absolute offsets */
        if (abs.empty()) return;
        size_t ci = at >= total_ ? chunks_.size() - 1 : chunk_of_line(at);
        std::vector<size_t> &c = chunks_[ci];
        size_t k = at >= total_ ? c.size() : at - firsts_[ci];
        std::vector<size_t> rel(abs.size());
        for (size_t i = 0; i < abs.size(); i++) rel[i] = abs[i] - deltas_[ci];
        c.insert(c.begin() + (long)k, rel.begin(), rel.end());
        if (c.size() > 2 * chunk_) {
            std::vector<size_t> n(c.begin() + (long)(c.size() / 2), c.end());
            size_t dl = deltas_[ci];
            c.resize(c.size() / 2);
            chunks_.insert(chunks_.begin() + (long)ci + 1, std::move(n));
            deltas_.insert(deltas_.begin() + (long)ci + 1, dl);
        }
        renumber(ci);
    }

private:
    size_t chunk_of_line(size_t line) const {
        size_t lo = 0, hi = firsts_.size();
        while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (firsts_[mid] <= line) lo = mid; else hi = mid; }
        return lo;
    }
    void renumber(size_t from) {
        firsts_.resize(chunks_.size());
        size_t n = from ? firsts_[from - 1] + chunks_[from - 1].size() : 0;
        for (size_t i = from; i < chunks_.size(); i++) { firsts_[i] = n; n += chunks_[i].size(); }
        total_ = n;
    }
    std::vector<std::vector<size_t>> chunks_ = {{0}};   /* line starts of each chunk, relative to its delta */
    std::vector<size_t> deltas_ = {0};                  /* pending offset per chunk, and the first line number per chunk: flat arrays, so the per-edit sweeps are cheap */
    std::vector<size_t> firsts_ = {0};
    size_t total_ = 1;
    size_t chunk_ = 2048;
};
#endif
