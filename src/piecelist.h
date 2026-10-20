/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_PIECELIST_H
#define HE_PIECELIST_H
#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <vector>

/* One run of text: which buffer it lives in (the mapped file or the add buffer), where it starts and how long it is. The buffer flag
 * rides in the top bit of the offset, so a piece is 16 bytes instead of 24. */
struct Piece {
    static constexpr size_t ADD = (size_t)1 << (sizeof(size_t) * 8 - 1);
    size_t tag, len;
    Piece() : tag(0), len(0) {}
    Piece(bool add, size_t off, size_t n) : tag(off | (add ? ADD : 0)), len(n) {}
    bool add() const { return (tag & ADD) != 0; }
    size_t off() const { return tag & ~ADD; }
};

/* The pieces of a text buffer in chunks of up to MAXN, each chunk with the total length of its pieces. Finding the piece that holds a byte offset
 * walks the chunks (about pieces / 190 of them) and then one chunk; an insert or an erase moves at most one chunk. A flat vector made both
 * O(pieces): 20,000 scattered edits in a 16 MB file cost 19 us each, mostly memmove and a linear scan. (Prefix sums per chunk, to search by
 * bisection, were tried: fewer instructions but slower, 35 ms instead of 30 ms for those edits, because every edit has to rebuild the sums.) */
class PieceList {
public:
    static constexpr size_t MAXN = 256;
    struct Cur { size_t c, i; };   /* chunk and index inside it; {chunks, 0} is the end */

    PieceList() { chunks_.push_back({}); }
    void clear() { chunks_.assign(1, Chunk()); count_ = 0; }
    size_t count() const { return count_; }
    Cur begin() const { return count_ ? Cur{0, 0} : end(); }
    Cur end() const { return {chunks_.size(), 0}; }
    bool at_end(Cur p) const { return p.c >= chunks_.size(); }
    const Piece &get(Cur p) const { return chunks_[p.c].v[p.i]; }
    bool only(Piece *out) const { if (count_ != 1) return false; *out = chunks_[0].v[0]; return true; }

    /* the piece holding byte `pos` and the offset inside it; the end when pos is at or past the total */
    Cur locate(size_t pos, size_t *within) const {
        size_t c = 0;
        while (c < chunks_.size() && pos >= chunks_[c].len) pos -= chunks_[c++].len;
        if (c >= chunks_.size()) { *within = 0; return end(); }
        const std::vector<Piece> &v = chunks_[c].v;
        size_t i = 0;
        while (pos >= v[i].len) pos -= v[i++].len;
        *within = pos;
        return {c, i};
    }
    Cur next(Cur p) const {
        if (++p.i >= chunks_[p.c].v.size()) { p.i = 0; p.c++; }
        return p;
    }
    bool prev(Cur p, Cur *out) const {   /* false at the very start */
        if (p.i > 0) { *out = {p.c, p.i - 1}; return true; }
        if (p.c == 0) return false;
        size_t c = p.c - 1;
        if (c >= chunks_.size() || chunks_[c].v.empty()) return false;
        *out = {c, chunks_[c].v.size() - 1};
        return true;
    }
    Cur last() const {   /* the final piece; the end when there are none */
        if (count_ == 0) return end();
        return {chunks_.size() - 1, chunks_.back().v.size() - 1};
    }
    void set_len(Cur p, size_t n) {
        Chunk &c = chunks_[p.c];
        c.len = c.len - c.v[p.i].len + n;
        c.v[p.i].len = n;
    }
    /* put np before p (at the end: after the last piece); returns where np ended up */
    Cur insert(Cur p, const Piece &np) {
        if (p.c >= chunks_.size()) { p.c = chunks_.size() - 1; p.i = chunks_[p.c].v.size(); }
        Chunk &c = chunks_[p.c];
        c.v.insert(c.v.begin() + (long)p.i, np);
        c.len += np.len;
        count_++;
        if (c.v.size() > MAXN) {
            size_t half = c.v.size() / 2;
            Chunk n;
            n.v.assign(c.v.begin() + (long)half, c.v.end());
            c.v.resize(half);
            for (const Piece &q : n.v) n.len += q.len;
            c.len -= n.len;
            chunks_.insert(chunks_.begin() + (long)p.c + 1, std::move(n));
            if (p.i >= half) return {p.c + 1, p.i - half};
        }
        return p;
    }
    /* remove `n` pieces from p on */
    void erase(Cur p, size_t n) {
        while (n && p.c < chunks_.size()) {
            Chunk &c = chunks_[p.c];
            size_t k = std::min(n, c.v.size() - p.i);
            for (size_t j = 0; j < k; j++) c.len -= c.v[p.i + j].len;
            c.v.erase(c.v.begin() + (long)p.i, c.v.begin() + (long)(p.i + k));
            count_ -= k;
            n -= k;
            if (c.v.empty() && chunks_.size() > 1) chunks_.erase(chunks_.begin() + (long)p.c);   /* the next chunk slides into p.c */
            else { p.c++; }
            p.i = 0;
        }
    }

private:
    struct Chunk { std::vector<Piece> v; size_t len = 0; };
    std::vector<Chunk> chunks_;
    size_t count_ = 0;
};
#endif
