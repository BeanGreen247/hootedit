/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_LINEINDEX_H
#define HE_LINEINDEX_H
#include <stddef.h>
#include <algorithm>
#include <vector>

/* Line start offsets in chunks, each with its own pending delta. A flat vector costs O(lines) per edit (measured
 * 5 ms per keystroke in the middle of a 14.5M line file); here an edit patches one chunk and bumps the deltas of
 * the later ones, O(chunk + chunks). Unsigned wrap-around arithmetic makes negative deltas work. */
class LineIndex {
public:
    static constexpr size_t CHUNK = 2048, MAXCHUNK = 4096;

    void reset(const std::vector<size_t> &starts) {
        chunks_.clear();
        for (size_t i = 0; i < starts.size(); i += CHUNK) {
            Chunk c;
            c.v.assign(starts.begin() + (long)i, starts.begin() + (long)std::min(starts.size(), i + CHUNK));
            c.delta = 0;
            chunks_.push_back(std::move(c));
        }
        if (chunks_.empty()) chunks_.push_back({{0}, 0, 0});
        renumber(0);
    }
    size_t count() const { return total_; }
    size_t at(size_t line) const {
        size_t ci = chunk_of_line(line);
        return chunks_[ci].v[line - chunks_[ci].first] + chunks_[ci].delta;
    }
    /* index of the last line whose start is <= pos */
    size_t line_of(size_t pos) const {
        size_t lo = 0, hi = chunks_.size();                       /* last chunk whose first start <= pos */
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (chunks_[mid].v[0] + chunks_[mid].delta <= pos) lo = mid; else hi = mid;
        }
        const Chunk &c = chunks_[lo];
        size_t idx = (size_t)(std::upper_bound(c.v.begin(), c.v.end(), pos - c.delta) - c.v.begin());
        return c.first + (idx ? idx - 1 : 0);
    }
    /* add d to every line start from line `from` on */
    void shift_from(size_t from, size_t d) {
        if (from >= total_) return;
        size_t ci = chunk_of_line(from);
        Chunk &c = chunks_[ci];
        for (size_t k = from - c.first; k < c.v.size(); k++) c.v[k] += d;
        for (size_t k = ci + 1; k < chunks_.size(); k++) chunks_[k].delta += d;
    }
    void erase(size_t a, size_t b) {                             /* drop lines [a, b) */
        while (a < b) {
            size_t ci = chunk_of_line(a);
            Chunk &c = chunks_[ci];
            size_t k0 = a - c.first, k1 = std::min(c.v.size(), k0 + (b - a));
            c.v.erase(c.v.begin() + (long)k0, c.v.begin() + (long)k1);
            b -= k1 - k0;
            if (c.v.empty() && chunks_.size() > 1) chunks_.erase(chunks_.begin() + (long)ci);
            renumber(ci ? ci - 1 : 0);
        }
    }
    void insert(size_t at, const std::vector<size_t> &abs) {     /* new lines before line `at`, absolute offsets */
        if (abs.empty()) return;
        size_t ci = at >= total_ ? chunks_.size() - 1 : chunk_of_line(at);
        Chunk &c = chunks_[ci];
        size_t k = at >= total_ ? c.v.size() : at - c.first;
        std::vector<size_t> rel(abs.size());
        for (size_t i = 0; i < abs.size(); i++) rel[i] = abs[i] - c.delta;
        c.v.insert(c.v.begin() + (long)k, rel.begin(), rel.end());
        if (c.v.size() > MAXCHUNK) {
            Chunk n;
            n.delta = c.delta;
            n.v.assign(c.v.begin() + (long)(c.v.size() / 2), c.v.end());
            c.v.resize(c.v.size() / 2);
            chunks_.insert(chunks_.begin() + (long)ci + 1, std::move(n));
        }
        renumber(ci);
    }

private:
    struct Chunk { std::vector<size_t> v; size_t delta; size_t first = 0; };
    size_t chunk_of_line(size_t line) const {
        size_t lo = 0, hi = chunks_.size();
        while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (chunks_[mid].first <= line) lo = mid; else hi = mid; }
        return lo;
    }
    void renumber(size_t from) {
        size_t n = from ? chunks_[from - 1].first + chunks_[from - 1].v.size() : 0;
        for (size_t i = from; i < chunks_.size(); i++) { chunks_[i].first = n; n += chunks_[i].v.size(); }
        total_ = n;
    }
    std::vector<Chunk> chunks_ = {{{0}, 0, 0}};
    size_t total_ = 1;
};
#endif
