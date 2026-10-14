/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_GIT_H
#define HE_GIT_H
#include <string>
#include <vector>

/* Source control view backed by the git command line; nothing is reimplemented. */
struct GitChange { char x, y; std::string path; };    /* porcelain status columns and the path relative to the repo root */
struct GitInfo {
    bool available = false;      /* git is installed */
    bool repo = false;           /* the workspace is inside a repository */
    std::string root, branch;
    std::vector<GitChange> changes;
    int ahead = 0, behind = 0;
    std::string upstream;        /* "origin/main", empty when the branch tracks nothing */
    bool has_remote = false;     /* at least one remote is configured */
    std::string op;              /* "merge", "rebase", "cherry-pick" or "revert" while one is unfinished, else empty */
};

bool git_installed();
GitInfo git_query(const std::string &dir);            /* runs `git status`; call on open, save and refresh, not every frame */
std::string git_diff(const std::string &root, const std::string &path, bool staged);   /* unified diff text for one file */

/* ---- history, blame and writing ---- */
struct GitCommit { std::string hash, subject, author, refs; long date = 0; int parents = 1; };
struct GitFile { char status; std::string path; };          /* status: M A D R, path is the new name */
struct GitBlame { std::string hash, author, summary; long time = 0; };   /* one entry per line; hash all zeros = not committed yet */

/* the file as it is in a revision: "HEAD", a hash, or ":" for the index; empty when it does not exist there */
std::string git_show(const std::string &root, const std::string &rev, const std::string &path);
std::vector<GitCommit> git_log(const std::string &root, int n, const std::string &path);          /* path empty = whole branch */
std::vector<GitFile> git_commit_files(const std::string &root, const std::string &hash);
std::vector<GitFile> git_compare_files(const std::string &root, const std::string &a, const std::string &b);
bool git_blame(const std::string &root, const std::string &path, std::vector<GitBlame> &out);     /* false: untracked or no history */
bool git_stage(const std::string &root, const std::string &path);
bool git_unstage(const std::string &root, const std::string &path);
bool git_stage_all(const std::string &root);
bool git_commit(const std::string &root, const std::string &message, std::string *err, bool all_if_nothing_staged);
/* Runs `git -C root <args>` with stdout and stderr together; stdin is closed and terminal prompts are off, so it never hangs on a password.
 * Safe to call from a worker thread. Returns the exit code. */
int git_exec(const std::string &root, const std::string &args, std::string *out);
std::string git_quote(const std::string &s);                 /* a shell-safe single argument */
std::string git_first_line(const std::string &s);
std::string git_ago(long t, long now);                        /* "3 days ago" */
#endif
