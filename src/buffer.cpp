/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "buffer.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

Buffer::~Buffer() { if (orig_) munmap(const_cast<char *>(orig_), map_len_); }

bool Buffer::open(const char *path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return false; }
    if (st.st_size > 0) {
        void *m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) { close(fd); return false; }
        madvise(m, (size_t)st.st_size, MADV_SEQUENTIAL);
        orig_ = static_cast<const char *>(m);
        orig_len_ = map_len_ = (size_t)st.st_size;
        flat_rev_ = ~0ull;
        pieces_.push_back({0, 0, orig_len_});
        size_ = orig_len_;
        std::vector<size_t> starts = {0};
        starts.reserve(orig_len_ / 40 + 16);
        for (const char *p = orig_, *e = orig_ + orig_len_; (p = static_cast<const char *>(memchr(p, '\n', (size_t)(e - p))));) starts.push_back((size_t)(++p - orig_));
        idx_.reset(starts);
        madvise(m, (size_t)st.st_size, MADV_NORMAL);
    }
    close(fd);
    return true;
}

bool Buffer::save(const char *path) {
    std::string tmp = std::string(path) + ".hootedit-tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    struct stat old;
    if (stat(path, &old) == 0) fchmod(fd, old.st_mode & 07777);
    bool ok = true;
    for (const Piece &p : pieces_) {
        const char *d = base(p);
        size_t left = p.len;
        while (left && ok) {
            ssize_t w = write(fd, d, left);
            if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
            d += w; left -= (size_t)w;
        }
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp.c_str(), path) != 0) ok = false;
    if (!ok) { unlink(tmp.c_str()); return false; }
    saved_rev_ = rev_;
    return true;
}

size_t Buffer::line_len(size_t line) const {
    size_t s = idx_.at(line), end = line + 1 < idx_.count() ? idx_.at(line + 1) - 1 : size_;
    return end - s;
}

size_t Buffer::line_of(size_t pos) const {
    return idx_.line_of(pos);
}

size_t Buffer::find(size_t pos, size_t *within) const {
    size_t i = 0;
    for (; i < pieces_.size(); i++) {
        if (pos < pieces_[i].len) break;
        pos -= pieces_[i].len;
    }
    *within = pos;
    return i;
}

void Buffer::copy(size_t pos, size_t len, std::string &out) const {
    out.clear();
    if (pos >= size_) return;
    if (len > size_ - pos) len = size_ - pos;
    size_t in;
    for (size_t i = find(pos, &in); i < pieces_.size() && len; i++, in = 0) {
        size_t n = std::min(len, pieces_[i].len - in);
        out.append(base(pieces_[i]) + in, n);
        len -= n;
    }
}

const char *Buffer::flat(size_t *n) const {
    *n = size_;
    if (size_ == 0) return "";
    if (pieces_.size() == 1 && !pieces_[0].add && pieces_[0].off == 0 && pieces_[0].len == size_) return orig_;
    if (flat_rev_ != rev_) { copy(0, size_, flat_); flat_rev_ = rev_; }
    return flat_.data();
}

void Buffer::insert(size_t pos, const char *s, size_t n) {
    if (!n) return;
    if (pos > size_) pos = size_;
    size_t at = add_.size();
    add_.append(s, n);
    size_t in, i = find(pos, &in);
    Piece np = {1, at, n};
    if (i == pieces_.size()) {
        if (!pieces_.empty() && pieces_.back().add && pieces_.back().off + pieces_.back().len == at) pieces_.back().len += n;
        else pieces_.push_back(np);
    } else if (in == 0) {
        if (i > 0 && pieces_[i - 1].add && pieces_[i - 1].off + pieces_[i - 1].len == at) pieces_[i - 1].len += n;
        else pieces_.insert(pieces_.begin() + (long)i, np);
    } else {
        Piece right = {pieces_[i].add, pieces_[i].off + in, pieces_[i].len - in};
        pieces_[i].len = in;
        pieces_.insert(pieces_.begin() + (long)i + 1, {np, right});
    }
    /* line index: starts after pos shift by n, plus one start per newline inside the new text */
    size_t first = line_of(pos) + 1;
    idx_.shift_from(first, n);
    std::vector<size_t> nl;
    for (size_t k = 0; k < n; k++) if (s[k] == '\n') nl.push_back(pos + k + 1);
    idx_.insert(first, nl);
    size_ += n;
    rev_++;
}

void Buffer::erase(size_t pos, size_t n) {
    if (pos >= size_ || !n) return;
    if (n > size_ - pos) n = size_ - pos;
    /* line index first, while the bytes are still readable */
    size_t l0 = line_of(pos), l1 = line_of(pos + n);          /* line starts in (pos, pos+n] disappear */
    size_t gone_from = l0 + 1, gone_to = l1 + 1;
    if (gone_to > gone_from) idx_.erase(gone_from, gone_to);
    idx_.shift_from(gone_from, (size_t)0 - n);

    size_t in, i = find(pos, &in);
    size_t left = n, k = i, skip = in;
    Piece keep[2];                                              /* what is left of the first and the last piece touched */
    int nk = 0;
    if (in > 0) { keep[nk] = pieces_[i]; keep[nk++].len = in; }
    while (k < pieces_.size() && left) {
        size_t avail = pieces_[k].len - skip;
        if (avail > left) { keep[nk++] = {pieces_[k].add, pieces_[k].off + skip + left, avail - left}; left = 0; k++; break; }
        left -= avail; skip = 0; k++;
    }
    pieces_.erase(pieces_.begin() + (long)i, pieces_.begin() + (long)k);   /* in place: no copy of the whole piece list per backspace */
    if (nk) pieces_.insert(pieces_.begin() + (long)i, keep, keep + nk);
    size_ -= n;
    rev_++;
}
