/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "askpass.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

void app_wake(void);

namespace {
std::string g_env, g_sock, g_script;
int g_listen = -1;
std::atomic<bool> g_run{false};
std::thread g_thread;
std::mutex g_mu;
std::deque<AskReq> g_q;

void serve() {
    while (g_run) {
        int fd = accept(g_listen, nullptr, nullptr);
        if (fd < 0) { if (!g_run) break; if (errno == EINTR) continue; break; }
        std::string prompt;
        char buf[512];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) { prompt.append(buf, (size_t)n); if (prompt.size() > 8000) break; }   /* the client closes its write side after the prompt: ssh prompts have several lines */
        { std::lock_guard<std::mutex> g(g_mu); g_q.push_back({fd, prompt}); }
        app_wake();
    }
}

std::string quote(const std::string &s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}
}  // namespace

int askpass_client(const char *prompt) {
    const char *path = getenv("HOOTEDIT_ASKPASS_SOCK");
    if (!path || !*path) return 1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    sockaddr_un a = {};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (connect(fd, (sockaddr *)&a, sizeof a) != 0) return 1;
    std::string p = prompt ? prompt : "";
    if (!p.empty() && write(fd, p.data(), p.size()) != (ssize_t)p.size()) return 1;
    shutdown(fd, SHUT_WR);
    std::string ans;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) ans.append(buf, (size_t)n);
    close(fd);
    if (ans.empty() || ans[0] != 'Y') return 1;     /* cancelled */
    fwrite(ans.data() + 1, 1, ans.size() - 1, stdout);
    fputc('\n', stdout);
    return 0;
}

bool askpass_start() {
    if (g_run) return true;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    std::string dir = rt && *rt ? std::string(rt) : "/tmp";
    if (dir.size() > 60) dir = "/tmp";   /* a unix socket path holds about 100 characters */
    dir += "/hootedit-" + std::to_string((long)getuid());
    mkdir(dir.c_str(), 0700);
    chmod(dir.c_str(), 0700);
    g_sock = dir + "/askpass-" + std::to_string((long)getpid()) + ".sock";
    g_script = dir + "/askpass.sh";
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    FILE *f = fopen(g_script.c_str(), "w");
    if (!f) return false;
    fprintf(f, "#!/bin/sh\nexec %s --askpass \"$@\"\n", quote(exe).c_str());
    fclose(f);
    chmod(g_script.c_str(), 0700);
    g_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen < 0) return false;
    sockaddr_un a = {};
    a.sun_family = AF_UNIX;
    if (g_sock.size() >= sizeof a.sun_path) return false;
    strcpy(a.sun_path, g_sock.c_str());
    unlink(g_sock.c_str());
    if (bind(g_listen, (sockaddr *)&a, sizeof a) != 0 || listen(g_listen, 8) != 0) { close(g_listen); g_listen = -1; return false; }
    chmod(g_sock.c_str(), 0600);
    g_run = true;
    g_thread = std::thread(serve);
    g_env = "GIT_ASKPASS=" + quote(g_script) + " SSH_ASKPASS=" + quote(g_script) + " SSH_ASKPASS_REQUIRE=force HOOTEDIT_ASKPASS_SOCK=" + quote(g_sock) + " ";
    if (!getenv("DISPLAY")) g_env += "DISPLAY=:0 ";   /* older ssh only uses SSH_ASKPASS when DISPLAY is set */
    return true;
}

const std::string &askpass_env() { return g_env; }

bool askpass_take(AskReq &r) {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_q.empty()) return false;
    r = g_q.front();
    g_q.pop_front();
    return true;
}

void askpass_reply(int fd, bool ok, const std::string &text) {
    std::string m = ok ? "Y" + text : "N";
    ssize_t w = write(fd, m.data(), m.size());
    (void)w;
    close(fd);
}

void askpass_stop() {
    if (!g_run) return;
    g_run = false;
    shutdown(g_listen, SHUT_RDWR);
    close(g_listen);
    if (g_thread.joinable()) g_thread.join();
    unlink(g_sock.c_str());
}
