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
};

bool git_installed();
GitInfo git_query(const std::string &dir);            /* runs `git status`; call on open, save and refresh, not every frame */
std::string git_diff(const std::string &root, const std::string &path, bool staged);   /* unified diff text for one file */
#endif
