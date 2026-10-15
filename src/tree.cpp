/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "tree.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <set>

namespace {

bool hidden_name(const char *n) { return !strcmp(n, ".git") || !strcmp(n, "node_modules") || !strcmp(n, "vendor") || !strcmp(n, "dist"); }

bool is_dir(const std::string &p, const dirent *e, bool follow_links) {
    if (e->d_type == DT_DIR) return true;
    if (e->d_type == DT_LNK && !follow_links) return false;     /* a walk for search never follows links: no cycles, no duplicates (like grep -r) */
    if (e->d_type != DT_LNK && e->d_type != DT_UNKNOWN) return false;
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string lower(std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }

/* the only visible entry of `dir` when it is a directory, else empty: VS Code's compact folders (a/b/c on one row) */
std::string sole_subdir(const std::string &dir) {
    DIR *d = opendir(dir.c_str());
    if (!d) return std::string();
    std::string only;
    int n = 0;
    while (dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || hidden_name(e->d_name)) continue;
        if (++n > 1) break;
        std::string p = dir + (dir.size() && dir.back() == '/' ? "" : "/") + e->d_name;
        if (is_dir(p, e, true)) only = e->d_name;
    }
    closedir(d);
    return n == 1 ? only : std::string();
}

}  // namespace

void Tree::load_dir(const std::string &dir, int depth, std::vector<Row> &out, bool follow_links) const {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<Row> v;
    while (dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || hidden_name(e->d_name)) continue;
        std::string p = dir + (dir.size() && dir.back() == '/' ? "" : "/") + e->d_name;
        bool dir = is_dir(p, e, follow_links);
        std::string name = e->d_name;
        if (dir && follow_links) {   /* the tree view, not the search walk */
            for (int guard = 0; guard < 12; guard++) { std::string sub = sole_subdir(p); if (sub.empty()) break; name += "/" + sub; p += "/" + sub; }
        }
        v.push_back({name, p, depth, dir, false});
    }
    closedir(d);
    if (follow_links) {   /* tree view only: files git tracks as deleted still show, struck through */
        for (const std::string &g : ghosts_) {
            size_t sl = g.rfind('/');
            if (sl == std::string::npos || g.compare(0, sl, dir) != 0 || sl != dir.size()) continue;
            std::string nm = g.substr(sl + 1);
            bool dup = false;
            for (const Row &r : v) if (r.name == nm) { dup = true; break; }
            if (!dup) v.push_back({nm, g, depth, false, false, true});
        }
    }
    std::sort(v.begin(), v.end(), [](const Row &a, const Row &b) { return a.dir != b.dir ? a.dir : lower(a.name) < lower(b.name); });
    out.insert(out.end(), v.begin(), v.end());
}

void Tree::set_root(const std::string &path) {
    root_ = path;
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    rows_.clear();
    load_dir(root_, 0, rows_, true);
}

std::string Tree::root_name() const {
    size_t s = root_.rfind('/');
    return s == std::string::npos || root_.size() == 1 ? root_ : root_.substr(s + 1);
}

void Tree::expand_at(size_t i) {
    std::vector<Row> kids;
    load_dir(rows_[i].path, rows_[i].depth + 1, kids, true);
    rows_[i].expanded = true;
    rows_.insert(rows_.begin() + (long)i + 1, kids.begin(), kids.end());
}

void Tree::toggle(size_t i) {
    if (i >= rows_.size() || !rows_[i].dir) return;
    if (!rows_[i].expanded) { expand_at(i); return; }
    size_t j = i + 1;
    while (j < rows_.size() && rows_[j].depth > rows_[i].depth) j++;
    rows_.erase(rows_.begin() + (long)i + 1, rows_.begin() + (long)j);
    rows_[i].expanded = false;
}

long Tree::find(const std::string &path) const {
    for (size_t i = 0; i < rows_.size(); i++) if (rows_[i].path == path) return (long)i;
    return -1;
}

void Tree::reveal(const std::string &path) {
    if (path.compare(0, root_.size(), root_) != 0) return;
    size_t pos = root_.size();
    while (pos < path.size()) {
        size_t slash = path.find('/', pos + 1);
        if (slash == std::string::npos) break;
        long i = find(path.substr(0, slash));
        if (i >= 0 && !rows_[(size_t)i].expanded) expand_at((size_t)i);
        pos = slash;
    }
}

void Tree::refresh() {
    std::set<std::string> open;
    for (const Row &r : rows_) if (r.dir && r.expanded) open.insert(r.path);
    rows_.clear();
    load_dir(root_, 0, rows_, true);
    for (size_t i = 0; i < rows_.size(); i++)
        if (rows_[i].dir && open.count(rows_[i].path)) expand_at(i);   /* children are appended after i, so the loop reaches them next */
}

std::vector<std::string> Tree::all_files(size_t cap) const {
    std::vector<std::string> out;
    std::vector<std::string> todo = {root_};
    while (!todo.empty() && out.size() < cap) {
        std::string dir = todo.back();
        todo.pop_back();
        std::vector<Row> v;
        load_dir(dir, 0, v, false);
        for (const Row &r : v) {
            if (r.dir) todo.push_back(r.path);
            else if (out.size() < cap) out.push_back(r.path);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
