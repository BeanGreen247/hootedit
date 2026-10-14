/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

std::string shq(const std::string &s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

std::string run(const std::string &cmd) {
    std::string out;
    FILE *p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

}  // namespace

bool git_installed() {
    const char *path = getenv("PATH");
    if (!path) return false;
    std::string p = path;
    for (size_t s = 0; s <= p.size();) {
        size_t e = p.find(':', s);
        if (e == std::string::npos) e = p.size();
        std::string f = p.substr(s, e - s) + "/git";
        if (access(f.c_str(), X_OK) == 0) return true;
        s = e + 1;
    }
    return false;
}

GitInfo git_query(const std::string &dir) {
    GitInfo g;
    g.available = git_installed();
    if (!g.available) return g;
    std::string top = run("git -C " + shq(dir) + " rev-parse --show-toplevel");
    while (!top.empty() && (top.back() == '\n' || top.back() == '\r')) top.pop_back();
    if (top.empty()) return g;
    g.repo = true;
    g.root = top;
    std::string st = run("git -C " + shq(top) + " status --porcelain=v1 -b -unormal");
    size_t pos = 0;
    while (pos < st.size()) {
        size_t nl = st.find('\n', pos);
        if (nl == std::string::npos) nl = st.size();
        std::string line = st.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.compare(0, 3, "## ") == 0) {
            std::string b = line.substr(3);
            size_t dots = b.find("...");
            size_t sp = b.find(' ');
            g.branch = b.substr(0, std::min(dots, sp));
            if (b.compare(0, 20, "No commits yet on ") == 0) g.branch = b.substr(18);
            if (b.compare(0, 4, "HEAD") == 0) g.branch = "HEAD (detached)";
            size_t a = b.find("ahead "), bh = b.find("behind ");
            if (a != std::string::npos) g.ahead = atoi(b.c_str() + a + 6);
            if (bh != std::string::npos) g.behind = atoi(b.c_str() + bh + 7);
        } else if (line.size() > 3) {
            std::string path = line.substr(3);
            size_t arrow = path.find(" -> ");
            if (arrow != std::string::npos) path = path.substr(arrow + 4);   /* rename: show the new name */
            if (path.size() > 1 && path.front() == '"') path = path.substr(1, path.size() - 2);
            g.changes.push_back({line[0], line[1], path});
        }
    }
    return g;
}

std::string git_diff(const std::string &root, const std::string &path, bool staged) {
    return run("git -C " + shq(root) + " diff --no-color " + (staged ? "--cached " : "") + "-- " + shq(path));
}
