/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "search.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

#include "regex.h"
#include "tree.h"

namespace {
double now_s() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
const size_t MAX_FILE = 512u << 20;        /* bigger than this is skipped */
const size_t READ_LIMIT = 128u << 10;      /* up to this size a read() into a reused buffer beats an mmap */
const size_t PER_FILE_HITS = 400;
}  // namespace

struct Searcher::Job {
    std::shared_ptr<const std::vector<std::string>> files;
    SearchQuery q;
    Regex re;
    std::string error;
    std::atomic<bool> cancel{false};
    std::atomic<size_t> next{0}, scanned{0}, hits{0}, with_hits{0}, bytes{0};
    std::atomic<int> workers_left{0};
    std::atomic<bool> done{false}, truncated{false};
    std::atomic<double> end_ms{0};
    double t0 = 0;
    mutable std::mutex mu;
    std::vector<std::vector<SearchHit>> slots;      /* one list per file, filled as files finish */
    std::function<void()> wake;
    std::atomic<double> last_wake{0};
};

Searcher::Searcher() {}
Searcher::~Searcher() { cancel(); }

std::string Searcher::escape_literal(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80)) o += '\\';
        o += (char)c;
    }
    return o;
}

static void scan_file(Searcher::Job &j, size_t idx, Matcher &m, std::vector<char> &rbuf) {
    const std::string &path = (*j.files)[idx];
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > MAX_FILE) { close(fd); return; }
    size_t n = (size_t)st.st_size;
    const char *buf = nullptr;
    void *map = nullptr;
    if (n <= READ_LIMIT) {
        if (rbuf.size() < n) rbuf.resize(n);
        size_t got = 0;
        while (got < n) { ssize_t r = read(fd, rbuf.data() + got, n - got); if (r <= 0) break; got += (size_t)r; }
        n = got;
        buf = rbuf.data();
    } else {
        map = mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { close(fd); return; }
        madvise(map, n, MADV_SEQUENTIAL);
        buf = static_cast<const char *>(map);
    }
    close(fd);
    j.bytes += n;
    if (n && !memchr(buf, 0, std::min<size_t>(n, 8192))) {   /* a NUL in the first 8 KB: binary, skip like grep does */
        std::vector<SearchHit> found;
        size_t counted = 0;
        uint32_t line_no = 1;
        m.scan(buf, n, [&](size_t ls, size_t le) {
            if (j.cancel.load(std::memory_order_relaxed)) return false;
            if (found.size() >= PER_FILE_HITS) return false;
            if (j.hits.load(std::memory_order_relaxed) >= j.q.max_results) { j.truncated = true; return false; }
            line_no += (uint32_t)std::count(buf + counted, buf + ls, '\n');
            counted = ls;
            size_t ms = 0, me = 0;
            if (!m.find(buf + ls, le - ls, 0, &ms, &me)) { ms = 0; me = 0; }
            size_t cut = 0, len = le - ls;
            if (len > 300) { cut = ms > 80 ? ms - 80 : 0; len = std::min<size_t>(len - cut, 300); }
            SearchHit h;
            h.file = (uint32_t)idx;
            h.line = line_no;
            h.off = (uint32_t)cut;
            h.ms = (uint32_t)(ms - cut);
            h.me = (uint32_t)std::min<size_t>(me - cut, len);
            h.text.assign(buf + ls + cut, len);
            found.push_back(std::move(h));
            j.hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
        if (!found.empty()) {
            j.with_hits++;
            std::lock_guard<std::mutex> g(j.mu);
            j.slots[idx] = std::move(found);
        }
    }
    if (map) munmap(map, n);
}

static void worker(std::shared_ptr<Searcher::Job> jp) {
    Searcher::Job &j = *jp;
    Matcher m(j.re);
    std::vector<char> rbuf;
    const size_t total = j.files->size();
    for (;;) {
        if (j.cancel.load(std::memory_order_relaxed)) break;
        size_t i = j.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= total) break;
        scan_file(j, i, m, rbuf);
        j.scanned++;
        double t = now_s();
        double lw = j.last_wake.load(std::memory_order_relaxed);
        if (j.wake && t - lw > 0.03 && j.last_wake.compare_exchange_weak(lw, t)) j.wake();   /* at most ~30 wake-ups per second */
    }
    if (j.workers_left.fetch_sub(1) == 1) {
        j.end_ms = (now_s() - j.t0) * 1000.0;
        j.done = true;
        if (j.wake && !j.cancel.load()) j.wake();
    }
}

void Searcher::start(std::shared_ptr<const std::vector<std::string>> files, const SearchQuery &q, std::function<void()> wake) {
    cancel();
    auto j = std::make_shared<Job>();
    j->files = files;
    j->q = q;
    j->wake = wake;
    j->t0 = now_s();
    j->slots.resize(files->size());
    std::string pat = q.regex ? q.text : escape_literal(q.text);
    if (q.word) pat = "\\b(?:" + pat + ")\\b";
    bool icase = q.casing == SearchQuery::INSENSITIVE;
    if (q.casing == SearchQuery::SMART) { icase = true; for (unsigned char c : q.text) if (c >= 'A' && c <= 'Z') icase = false; }
    if (q.text.empty() || !j->re.compile(pat, icase, &j->error)) {
        if (q.text.empty()) j->error.clear();
        j->done = true;
        job_ = j;
        return;
    }
    size_t nt = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), 16);
    nt = std::min(nt, std::max<size_t>(1, files->size() / 8));
    j->workers_left = (int)nt;
    job_ = j;
    for (size_t t = 0; t < nt; t++) std::thread(worker, j).detach();   /* detached: a cancelled search finishes on its own, nobody waits for it */
}

void Searcher::cancel() { if (job_) job_->cancel = true; }

SearchStats Searcher::stats() const {
    SearchStats s;
    if (!job_) { s.done = true; return s; }
    const Job &j = *job_;
    s.files_total = j.files->size();
    s.files_scanned = j.scanned;
    s.files_with_hits = j.with_hits;
    s.hits = j.hits;
    s.bytes = j.bytes;
    s.done = j.done;
    s.truncated = j.truncated;
    s.error = j.error;
    s.ms = j.done ? (double)j.end_ms.load() : (now_s() - j.t0) * 1000.0;
    return s;
}

void Searcher::results(std::vector<SearchHit> &out) const {
    out.clear();
    if (!job_) return;
    std::lock_guard<std::mutex> g(job_->mu);
    for (const auto &v : job_->slots) out.insert(out.end(), v.begin(), v.end());
}

const std::vector<std::string> &Searcher::files() const { static const std::vector<std::string> none; return job_ ? *job_->files : none; }

std::vector<std::string> list_workspace_files(const std::string &root, bool git_repo) {
    std::vector<std::string> out;
    if (git_repo) {
        std::string cmd = "git -C '" + root + "' ls-files -z --cached --others --exclude-standard 2>/dev/null";
        FILE *p = popen(cmd.c_str(), "r");
        if (p) {
            std::string all;
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, p)) > 0) all.append(buf, n);
            pclose(p);
            size_t pos = 0;
            while (pos < all.size()) {
                size_t e = all.find('\0', pos);
                if (e == std::string::npos) e = all.size();
                if (e > pos) {
                    std::string rel = all.substr(pos, e - pos);
                    struct stat st;
                    std::string full = root + "/" + rel;
                    if (lstat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(std::move(full));   /* drops deleted files and submodule directories */
                }
                pos = e + 1;
            }
            if (!out.empty()) return out;
        }
    }
    Tree t;
    t.set_root(root);
    return t.all_files(2000000);
}
