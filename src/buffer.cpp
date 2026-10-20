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
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

#include <atomic>
#include <unistd.h>

/* A mapped file that another program truncates would kill us with SIGBUS the moment a page past the new end is read. Mapped buffers are
 * registered here, and the handler puts a page of zeros over the missing one so reading goes on (the editor reloads the file a moment later). */
namespace {
struct MapRange { std::atomic<uintptr_t> start{0}; std::atomic<size_t> len{0}; };
MapRange g_maps[512];
long g_page = 4096;

void on_sigbus(int, siginfo_t *si, void *) {
    uintptr_t a = (uintptr_t)si->si_addr;
    for (MapRange &m : g_maps) {
        uintptr_t s = m.start.load(std::memory_order_relaxed);
        if (s && a >= s && a < s + m.len.load(std::memory_order_relaxed)) {
            void *page = (void *)(a & ~(uintptr_t)(g_page - 1));
            if (mmap(page, (size_t)g_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED) return;
        }
    }
    signal(SIGBUS, SIG_DFL);   /* not ours: crash the usual way */
}

void register_map(const void *p, size_t len) {
    static bool installed = false;
    if (!installed) {
        installed = true;
        g_page = sysconf(_SC_PAGESIZE);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_sigbus;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGBUS, &sa, nullptr);
    }
    for (MapRange &m : g_maps) { uintptr_t z = 0; if (m.start.compare_exchange_strong(z, (uintptr_t)p)) { m.len = len; return; } }
}

void unregister_map(const void *p) {
    for (MapRange &m : g_maps) if (m.start.load() == (uintptr_t)p) { m.start = 0; m.len = 0; return; }
}

const size_t READ_INTO_MEMORY = 16u << 20;   /* up to this size the file is read, so a program that rewrites it in place cannot change the text under us */
}  // namespace

Buffer::~Buffer() { if (orig_ && map_len_) { unregister_map(orig_); munmap(const_cast<char *>(orig_), map_len_); } }

bool Buffer::open(const char *path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return false; }
    if (st.st_size > 0) {
        void *m = MAP_FAILED;
        if ((size_t)st.st_size <= READ_INTO_MEMORY) {
            own_.resize((size_t)st.st_size);
            size_t got = 0;
            while (got < own_.size()) { ssize_t r = read(fd, &own_[got], own_.size() - got); if (r <= 0) break; got += (size_t)r; }
            own_.resize(got);
            if (own_.empty()) { close(fd); return true; }
            orig_ = own_.data();
            orig_len_ = own_.size();
            map_len_ = 0;
        } else {
            m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m == MAP_FAILED) { close(fd); return false; }
            madvise(m, (size_t)st.st_size, MADV_SEQUENTIAL);
            orig_ = static_cast<const char *>(m);
            orig_len_ = map_len_ = (size_t)st.st_size;
            register_map(orig_, orig_len_);
        }
        flat_rev_ = ~0ull;
        pieces_.insert(pieces_.end(), Piece(false, 0, orig_len_));
        size_ = orig_len_;
        std::vector<size_t> starts = {0};
        starts.reserve(orig_len_ / 40 + 16);
        for (const char *p = orig_, *e = orig_ + orig_len_; (p = static_cast<const char *>(memchr(p, '\n', (size_t)(e - p))));) starts.push_back((size_t)(++p - orig_));
        idx_.reset(starts);
        if (m != MAP_FAILED) madvise(m, (size_t)st.st_size, MADV_NORMAL);
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
    for (PieceList::Cur it = pieces_.begin(); !pieces_.at_end(it); it = pieces_.next(it)) {
        const Piece &p = pieces_.get(it);
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

void Buffer::copy(size_t pos, size_t len, std::string &out) const {
    out.clear();
    if (pos >= size_) return;
    if (len > size_ - pos) len = size_ - pos;
    size_t in;
    for (PieceList::Cur it = pieces_.locate(pos, &in); !pieces_.at_end(it) && len; it = pieces_.next(it), in = 0) {
        const Piece &p = pieces_.get(it);
        size_t n = std::min(len, p.len - in);
        out.append(base(p) + in, n);
        len -= n;
    }
}

void Buffer::copy_to(size_t pos, size_t len, char *out) const {
    size_t in;
    for (PieceList::Cur it = pieces_.locate(pos, &in); !pieces_.at_end(it) && len; it = pieces_.next(it), in = 0) {
        const Piece &p = pieces_.get(it);
        size_t n = std::min(len, p.len - in);
        memcpy(out, base(p) + in, n);
        out += n;
        len -= n;
    }
}

const char *Buffer::flat(size_t *n) const {
    *n = size_;
    if (size_ == 0) return "";
    Piece only;
    if (pieces_.only(&only) && !only.add() && only.off() == 0 && only.len == size_) return orig_;
    if (flat_rev_ != rev_) { copy(0, size_, flat_); flat_rev_ = rev_; }
    return flat_.data();
}

void Buffer::insert(size_t pos, const char *s, size_t n) {
    if (!n) return;
    if (pos > size_) pos = size_;
    size_t at = add_.size();
    add_.append(s, n);
    size_t in;
    PieceList::Cur cur = pieces_.locate(pos, &in), prev;
    Piece np(true, at, n);
    if (pieces_.at_end(cur)) {
        if (pieces_.prev(cur, &prev) && pieces_.get(prev).add() && pieces_.get(prev).off() + pieces_.get(prev).len == at) pieces_.set_len(prev, pieces_.get(prev).len + n);
        else pieces_.insert(cur, np);
    } else if (in == 0) {
        if (pieces_.prev(cur, &prev) && pieces_.get(prev).add() && pieces_.get(prev).off() + pieces_.get(prev).len == at) pieces_.set_len(prev, pieces_.get(prev).len + n);
        else pieces_.insert(cur, np);
    } else {
        Piece right(pieces_.get(cur).add(), pieces_.get(cur).off() + in, pieces_.get(cur).len - in);
        pieces_.set_len(cur, in);
        PieceList::Cur at_right = pieces_.insert(pieces_.next(cur), right);
        pieces_.insert(at_right, np);   /* before the right half */
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

    size_t in;
    PieceList::Cur first = pieces_.locate(pos, &in), it = first;
    size_t left = n, skip = in, touched = 0;
    Piece keep[2];                                              /* what is left of the first and the last piece touched */
    int nk = 0;
    if (in > 0) keep[nk++] = Piece(pieces_.get(first).add(), pieces_.get(first).off(), in);
    while (!pieces_.at_end(it) && left) {
        const Piece &p = pieces_.get(it);
        size_t avail = p.len - skip;
        touched++;
        if (avail > left) { keep[nk++] = Piece(p.add(), p.off() + skip + left, avail - left); left = 0; break; }
        left -= avail; skip = 0; it = pieces_.next(it);
    }
    pieces_.erase(first, touched);   /* the pieces after the cut slide into place, one chunk at most */
    if (nk) {
        PieceList::Cur at = pieces_.insert(first, keep[0]);
        if (nk == 2) pieces_.insert(pieces_.next(at), keep[1]);
    }
    size_ -= n;
    rev_++;
}
