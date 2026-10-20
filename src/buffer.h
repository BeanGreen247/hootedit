/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_BUFFER_H
#define HE_BUFFER_H
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "lineindex.h"
#include "piecelist.h"

/* Piece table: the file is mapped read-only (never copied), edits append to one add buffer. A line-start index
 * is built once with memchr and patched per edit. */
class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    bool open(const char *path);   /* false: unreadable; a missing file opens as empty so it can be created on save */
    bool save(const char *path);   /* temp file in the same directory, fsync, rename; the buffer stays untouched on failure */

    size_t size() const { return size_; }
    size_t lines() const { return idx_.count(); }
    size_t line_start(size_t line) const { return idx_.at(line); }
    size_t line_len(size_t line) const;                      /* bytes without the newline */
    size_t line_of(size_t pos) const;                        /* line containing byte offset pos */
    void copy(size_t pos, size_t len, std::string &out) const;
    void copy_to(size_t pos, size_t len, char *out) const;     /* the same into a caller's buffer of at least len bytes; pos + len must be within the text */
    /* the whole text as one contiguous block: the mapped file itself while nothing is edited, else a copy kept until the next edit.
     * Valid until the buffer changes. For scanning big files without a per-line copy. */
    const char *flat(size_t *n) const;
    void line_text(size_t line, std::string &out) const { copy(idx_.at(line), line_len(line), out); }

    void insert(size_t pos, const char *s, size_t n);
    void erase(size_t pos, size_t n);

    uint64_t revision() const { return rev_; }
    bool modified() const { return rev_ != saved_rev_; }
    size_t pieces() const { return pieces_.count(); }

private:
    const char *base(const Piece &p) const { return (p.add() ? add_.data() : orig_) + p.off(); }

    std::string own_;                                        /* the file read into memory (small files); orig_ points into it */
    const char *orig_ = nullptr;
    size_t orig_len_ = 0, map_len_ = 0;
    std::string add_;
    PieceList pieces_;
    LineIndex idx_;
    size_t size_ = 0;
    uint64_t rev_ = 0, saved_rev_ = 0;
    mutable std::string flat_;
    mutable uint64_t flat_rev_ = ~0ull;
};
#endif
