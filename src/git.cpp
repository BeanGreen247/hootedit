/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <sys/stat.h>
#include "git.h"
#include "askpass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>

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
            if (dots != std::string::npos) { size_t e = b.find(' ', dots); g.upstream = b.substr(dots + 3, e == std::string::npos ? std::string::npos : e - dots - 3); }
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
    if (g.upstream.empty()) g.has_remote = !run("git -C " + shq(top) + " remote").empty();
    else g.has_remote = true;
    std::string gd = run("git -C " + shq(top) + " rev-parse --git-dir");
    while (!gd.empty() && (gd.back() == '\n' || gd.back() == '\r')) gd.pop_back();
    if (!gd.empty()) {
        if (gd[0] != '/') gd = top + "/" + gd;
        struct stat sb;
        auto has = [&](const char *n) { return stat((gd + "/" + n).c_str(), &sb) == 0; };
        if (has("rebase-merge") || has("rebase-apply")) g.op = "rebase";
        else if (has("MERGE_HEAD")) g.op = "merge";
        else if (has("CHERRY_PICK_HEAD")) g.op = "cherry-pick";
        else if (has("REVERT_HEAD")) g.op = "revert";
    }
    return g;
}

std::string git_diff(const std::string &root, const std::string &path, bool staged) {
    return run("git -C " + shq(root) + " diff --no-color " + (staged ? "--cached " : "") + "-- " + shq(path));
}

namespace {
std::string run_both(const std::string &cmd, int *code) {   /* stdout and stderr together, for messages shown to the user */
    std::string out;
    FILE *p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) { if (code) *code = -1; return out; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int st = pclose(p);
    if (code) *code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}
std::vector<std::string> lines_of(const std::string &s, char sep = '\n') {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find(sep, pos);
        if (e == std::string::npos) e = s.size();
        v.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return v;
}
}  // namespace

std::string git_show(const std::string &root, const std::string &rev, const std::string &path) {
    return run("git -C " + shq(root) + " show " + shq((rev == ":" ? std::string(":") : rev + ":") + path));
}

std::vector<GitCommit> git_log(const std::string &root, int n, const std::string &path, bool graph) {
    std::vector<GitCommit> v;
    /* fields split by 0x1f, records by 0x1e; the path form follows renames so a moved file keeps its history */
    std::string cmd = "git -C " + shq(root) + " log -n " + std::to_string(n) + " --decorate=short --format=%H%x1f%an%x1f%at%x1f%P%x1f%D%x1f%s%x1e";
    if (graph) cmd += " --topo-order HEAD --branches --remotes";
    if (!path.empty()) cmd += " --follow -- " + shq(path);
    std::string out = run(cmd);
    for (const std::string &rec : lines_of(out, 0x1e)) {
        std::string r = rec;
        while (!r.empty() && (r[0] == '\n' || r[0] == '\r')) r.erase(0, 1);
        std::vector<std::string> f = lines_of(r, 0x1f);
        if (f.size() < 6) continue;
        GitCommit c;
        c.hash = f[0]; c.author = f[1]; c.date = atol(f[2].c_str());
        c.parents = f[3].empty() ? 0 : 1;
        for (char ch : f[3]) if (ch == ' ') c.parents++;
        c.parent_hashes = lines_of(f[3], ' ');
        c.refs = f[4]; c.subject = f[5];
        v.push_back(c);
    }
    return v;
}

static std::vector<GitFile> parse_name_status(const std::string &out) {
    std::vector<GitFile> v;
    for (const std::string &ln : lines_of(out)) {
        if (ln.size() < 3) continue;
        size_t tab = ln.find('\t');
        if (tab == std::string::npos) continue;
        char st = ln[0];
        std::string rest = ln.substr(tab + 1);
        size_t t2 = rest.find('\t');
        if (t2 != std::string::npos) rest = rest.substr(t2 + 1);   /* renames and copies list old then new */
        v.push_back({st == 'C' ? 'A' : st, rest});
    }
    return v;
}

std::vector<GitFile> git_commit_files(const std::string &root, const std::string &hash) {
    return parse_name_status(run("git -C " + shq(root) + " diff-tree --no-commit-id --name-status -r -M --root -m --first-parent " + shq(hash)));
}

std::vector<GitFile> git_compare_files(const std::string &root, const std::string &a, const std::string &b) {
    return parse_name_status(run("git -C " + shq(root) + " diff --name-status -M " + shq(a) + " " + shq(b)));
}

bool git_blame(const std::string &root, const std::string &path, std::vector<GitBlame> &out) {
    out.clear();
    std::string txt = run("git -C " + shq(root) + " blame --porcelain -- " + shq(path));
    if (txt.empty()) return false;
    std::unordered_map<std::string, GitBlame> seen;
    size_t pos = 0;
    GitBlame cur;
    bool have = false;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        const char *l = txt.c_str() + pos;
        size_t len = nl - pos;
        if (!have && len >= 41 && l[40] == ' ') {   /* "<40 hex> <orig line> <final line> [count]" starts a line record */
            std::string h(l, 40);
            auto it = seen.find(h);
            cur = it != seen.end() ? it->second : GitBlame();
            cur.hash = h;
            have = true;
        } else if (have && l[0] == '\t') {
            seen[cur.hash] = cur;
            out.push_back(cur);
            have = false;
        } else if (have) {
            std::string ln(l, len);
            if (ln.compare(0, 7, "author ") == 0) cur.author = ln.substr(7);
            else if (ln.compare(0, 12, "author-time ") == 0) cur.time = atol(ln.c_str() + 12);
            else if (ln.compare(0, 8, "summary ") == 0) cur.summary = ln.substr(8);
        }
        pos = nl + 1;
    }
    return !out.empty();
}

bool git_stage(const std::string &root, const std::string &path) { int c; run_both("git -C " + shq(root) + " add -- " + shq(path), &c); return c == 0; }
bool git_unstage(const std::string &root, const std::string &path) {
    int c;
    run_both("git -C " + shq(root) + " restore --staged -- " + shq(path), &c);
    if (c != 0) run_both("git -C " + shq(root) + " rm --cached -q -- " + shq(path), &c);   /* a repository with no commit yet has no HEAD to restore from */
    return c == 0;
}
bool git_stage_all(const std::string &root) { int c; run_both("git -C " + shq(root) + " add -A", &c); return c == 0; }

bool git_commit(const std::string &root, const std::string &message, std::string *err, bool all_if_nothing_staged) {
    int c;
    if (all_if_nothing_staged) {
        std::string staged = run("git -C " + shq(root) + " diff --cached --name-only");
        if (staged.empty()) git_stage_all(root);
    }
    std::string out = run_both("git -C " + shq(root) + " commit -m " + shq(message), &c);
    if (c != 0 && err) *err = out.empty() ? "commit failed" : out.substr(0, out.find('\n'));
    return c == 0;
}

std::string git_ago(long t, long now) {
    long d = now - t;
    if (d < 0) d = 0;
    char b[48];
    auto f = [&](long n, const char *u) { snprintf(b, sizeof b, "%ld %s%s ago", n, u, n == 1 ? "" : "s"); return std::string(b); };
    if (d < 60) return "just now";
    if (d < 3600) return f(d / 60, "min");
    if (d < 86400) return f(d / 3600, "hour");
    if (d < 86400 * 30) return f(d / 86400, "day");
    if (d < 86400 * 365) return f(d / (86400 * 30), "month");
    return f(d / (86400 * 365), "year");
}

std::string git_quote(const std::string &s) { return shq(s); }

int git_exec(const std::string &root, const std::string &args, std::string *out) {
    int c = -1;
    std::string o = run_both(askpass_env() + "GIT_TERMINAL_PROMPT=0 GIT_EDITOR=true git -C " + shq(root) + " " + args + " </dev/null", &c);
    if (out) *out = o;
    return c;
}

std::string git_first_line(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == '\n' || s[b] == '\r' || s[b] == ' ')) b++;
    size_t e = s.find('\n', b);
    return s.substr(b, e == std::string::npos ? std::string::npos : e - b);
}
