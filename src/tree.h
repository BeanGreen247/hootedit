/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_TREE_H
#define HE_TREE_H
#include <string>
#include <vector>

/* The explorer: a flat list of the visible rows. Directories are read only when they are first expanded, so opening
 * a huge folder costs one readdir. Hidden like VS Code in the owner's settings: .git, node_modules, vendor, dist. */
class Tree {
public:
    struct Row { std::string name, path; int depth; bool dir, expanded; bool ghost = false; };   /* ghost: deleted on disk but git still tracks it */

    void set_root(const std::string &path);
    const std::string &root() const { return root_; }
    std::string root_name() const;
    const std::vector<Row> &rows() const { return rows_; }
    void toggle(size_t i);                    /* expand or collapse directory row i */
    void reveal(const std::string &path);     /* expand parents so `path` is visible */
    void set_ghosts(std::vector<std::string> abs_paths) { ghosts_ = std::move(abs_paths); }   /* call refresh() after */
    const std::vector<std::string> &ghosts() const { return ghosts_; }
    void refresh();                           /* re-read every expanded directory */
    long find(const std::string &path) const; /* row index or -1 */

    /* every file below the root (for quick open), capped; skips the hidden directories */
    std::vector<std::string> all_files(size_t cap = 50000) const;

private:
    void load_dir(const std::string &dir, int depth, std::vector<Row> &out, bool follow_links) const;
    void expand_at(size_t i);
    std::string root_;
    std::vector<Row> rows_;
    std::vector<std::string> ghosts_;
};
#endif
