/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_SEARCH_H
#define HE_SEARCH_H
#include <stdint.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* Workspace text search. Files are scanned in parallel on background threads (read for small files, mmap for large ones, binary
 * files skipped), matching runs through our own Regex engine, and the UI is woken when results arrive. Starting a new search
 * cancels the old one without waiting for it. */
struct SearchQuery {
    std::string text;
    bool regex = false;        /* the text is a regular expression, otherwise a plain string */
    bool word = false;         /* whole words only */
    enum Case { SMART, SENSITIVE, INSENSITIVE } casing = SMART;   /* smart: lower-case query ignores case, an upper-case letter makes it exact */
    size_t max_results = 5000;
};

struct SearchHit {
    uint32_t file;             /* index into the file list given to start() */
    uint32_t line;             /* 1-based */
    uint32_t ms, me;           /* match span inside `text` */
    uint32_t off;              /* byte offset of text[0] within the real line (non-zero only when a very long line was cut) */
    std::string text;          /* the line, cut to a window around the match when it is very long */
};

struct SearchStats {
    uint64_t files_total = 0, files_scanned = 0, files_with_hits = 0, hits = 0, bytes = 0;
    double ms = 0;             /* wall time so far, or in total once done */
    bool done = false, truncated = false;
    std::string error;         /* pattern error, empty if fine */
};

class Searcher {
public:
    Searcher();
    ~Searcher();
    /* `wake` runs on a worker thread when new results are ready; keep it cheap and thread safe (SDL_PushEvent is) */
    void start(std::shared_ptr<const std::vector<std::string>> files, const SearchQuery &q, std::function<void()> wake);
    void cancel();
    SearchStats stats() const;
    void results(std::vector<SearchHit> &out) const;     /* ordered by file index, then line */
    const std::vector<std::string> &files() const;
    static std::string escape_literal(const std::string &s);
    struct Job;
private:
    std::shared_ptr<Job> job_;
};

/* every file of the workspace: `git ls-files` (tracked and untracked, honouring .gitignore) when it is a repository, else a directory walk */
std::vector<std::string> list_workspace_files(const std::string &root, bool git_repo);
#endif
