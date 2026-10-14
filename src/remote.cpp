/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "askpass.h"

namespace {

std::string q(const std::string &s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

int run(const std::string &cmd, std::string *out, bool null_stdin = true) {
    std::string o;
    FILE *p = popen((cmd + (null_stdin ? " 2>&1 </dev/null" : " 2>&1")).c_str(), "r");
    if (!p) { if (out) *out = "cannot run the command"; return -1; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) o.append(buf, n);
    int st = pclose(p);
    if (out) *out = o;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool have(const char *tool) {
    const char *path = getenv("PATH");
    if (!path) return false;
    std::string p = path;
    for (size_t s = 0; s <= p.size();) {
        size_t e = p.find(':', s);
        if (e == std::string::npos) e = p.size();
        if (access((p.substr(s, e - s) + "/" + tool).c_str(), X_OK) == 0) return true;
        s = e + 1;
    }
    return false;
}

std::string config_dir() {
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    std::string d = x && *x ? x : std::string(h ? h : ".") + "/.config";
    mkdir(d.c_str(), 0755);
    d += "/hootedit";
    mkdir(d.c_str(), 0755);
    return d;
}

std::string ctl_dir() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    std::string d = (rt && *rt ? std::string(rt) : "/tmp") + "/hootedit-" + std::to_string((long)getuid());
    mkdir(d.c_str(), 0700);
    d += "/ssh";
    mkdir(d.c_str(), 0700);
    return d;
}

/* ssh options shared by every call: one master connection per host, so only the first call asks for a password */
std::string ssh_opts(const RemoteProfile &p, bool for_sftp) {
    std::string o = "-o ControlMaster=auto -o ControlPersist=20m -o ControlPath=" + q(ctl_dir() + "/%C") + " -o ConnectTimeout=12 -o ServerAliveInterval=20 -o NumberOfPasswordPrompts=3";
    if (p.port) o += std::string(for_sftp ? " -P " : " -p ") + std::to_string(p.port);
    if (!p.key.empty()) o += " -o IdentitiesOnly=yes -i " + q(p.key);
    return o;
}
std::string target(const RemoteProfile &p) { return (p.user.empty() ? "" : p.user + "@") + p.host; }

/* an `ls -l` style listing (ftp servers, sftp, smb translated below): "drwxr-xr-x 2 user group 4096 Oct 14 12:00 name" */
void parse_long(const std::string &txt, std::vector<RemoteEntry> &out) {
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t nl = txt.find('\n', pos);
        if (nl == std::string::npos) nl = txt.size();
        std::string l = txt.substr(pos, nl - pos);
        pos = nl + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.size() < 10 || (l[0] != 'd' && l[0] != '-' && l[0] != 'l')) continue;
        size_t p = 0;
        for (int f = 0; f < 8; f++) {   /* skip eight fields: perms links owner group size month day time */
            while (p < l.size() && l[p] != ' ') p++;
            while (p < l.size() && l[p] == ' ') p++;
        }
        if (p >= l.size()) continue;
        std::string name = l.substr(p);
        size_t arrow = name.find(" -> ");
        if (l[0] == 'l' && arrow != std::string::npos) name = name.substr(0, arrow);
        size_t sl = name.rfind('/');
        if (sl != std::string::npos && sl + 1 < name.size()) name = name.substr(sl + 1);   /* sftp prints the path it was given in front of each name */
        if (name == "." || name == "..") continue;
        out.push_back({name, l[0] == 'd'});
    }
}

std::string join(const std::string &a, const std::string &b) {
    if (a.empty() || a == "/") return "/" + b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

/* private file for credentials: 0600 in the per-user runtime directory (tmpfs), removed again by the caller */
std::string secret_file(const std::string &body) {
    std::string t = ctl_dir() + "/auth-XXXXXX";
    int fd = mkstemp(&t[0]);
    if (fd < 0) return "";
    ssize_t w = write(fd, body.data(), body.size());
    (void)w;
    close(fd);
    return t;
}

/* curl config with the credentials; ftps is explicit TLS (AUTH TLS) unless the port is 990 */
std::string curl_cfg(const RemoteProfile &p, const std::string &pw) {
    std::string esc = "user = \"";
    for (char c : p.user + ":" + pw) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
    esc += "\"\n";
    if (p.proto == "ftps") esc += "ssl-reqd\n";
    if (p.proto == "ftps" && p.insecure) esc += "insecure\n";
    return secret_file(esc);
}

std::string url(const RemoteProfile &p, const std::string &path) {
    std::string pt = path;
    if (pt.empty() || pt[0] != '/') pt = "/" + pt;
    std::string enc;
    for (unsigned char c : pt) { if (isalnum(c) || strchr("/-_.~", c)) enc += (char)c; else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); enc += b; } }
    return std::string(p.proto == "ftps" && p.port == 990 ? "ftps" : "ftp") + "://" + p.host + (p.port ? ":" + std::to_string(p.port) : "") + enc;
}

/* smb: the first path component is the share */
bool smb_split(const std::string &path, std::string *share, std::string *rest) {
    std::string p = path;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    if (p.empty()) return false;
    size_t s = p.find('/');
    *share = p.substr(0, s);
    *rest = s == std::string::npos ? "" : p.substr(s + 1);
    return true;
}

/* the credentials go in a private auth file (smbclient -A), never on a command line; the caller removes it */
std::string smb_auth(const RemoteProfile &p, const std::string &pw) { return pw.empty() ? std::string() : secret_file("username = " + (p.user.empty() ? std::string("guest") : p.user) + "\npassword = " + pw + "\n"); }

std::string smb_cmd(const RemoteProfile &p, const std::string &auth, const std::string &share, const std::string &cmds) {
    return "smbclient " + q("//" + p.host + "/" + share) + (auth.empty() ? " -N" : " -A " + q(auth)) + (p.port ? " -p " + std::to_string(p.port) : "") + " -c " + q(cmds) + "; rc=$?; " + (auth.empty() ? "" : "rm -f " + q(auth) + "; ") + "exit $rc";
}

}  // namespace

std::vector<RemoteProfile> remote_load() {
    std::vector<RemoteProfile> v;
    FILE *f = fopen((config_dir() + "/remotes.ini").c_str(), "r");
    if (!f) return v;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, val = eq + 1;
        if (k == "name") v.push_back(RemoteProfile());
        if (v.empty()) continue;
        RemoteProfile &p = v.back();
        if (k == "name") p.name = val; else if (k == "proto") p.proto = val; else if (k == "host") p.host = val;
        else if (k == "user") p.user = val; else if (k == "path") p.path = val; else if (k == "key") p.key = val; else if (k == "port") p.port = atoi(val.c_str()); else if (k == "insecure") p.insecure = val == "1";
    }
    fclose(f);
    return v;
}

void remote_save(const std::vector<RemoteProfile> &v) {
    FILE *f = fopen((config_dir() + "/remotes.ini").c_str(), "w");
    if (!f) return;
    for (const RemoteProfile &p : v) fprintf(f, "name=%s\nproto=%s\nhost=%s\nuser=%s\npath=%s\nport=%d\nkey=%s\ninsecure=%d\n", p.name.c_str(), p.proto.c_str(), p.host.c_str(), p.user.c_str(), p.path.c_str(), p.port, p.key.c_str(), p.insecure ? 1 : 0);   /* no passwords are ever stored */
    fclose(f);
}

bool remote_tools(const RemoteProfile &p, std::string *missing) {
    const char *t = p.proto == "ssh" ? "ssh" : p.proto == "sftp" ? "sftp" : p.proto == "smb" ? "smbclient" : "curl";
    if (!have(t)) { if (missing) *missing = t; return false; }
    if (p.proto == "sftp" && !have("ssh")) { if (missing) *missing = "ssh"; return false; }
    return true;
}

bool remote_needs_password(const RemoteProfile &p) { return p.proto == "ftp" || p.proto == "ftps" || p.proto == "smb"; }

std::string remote_label(const RemoteProfile &p) { return (p.user.empty() ? "" : p.user + "@") + p.host + (p.port ? ":" + std::to_string(p.port) : ""); }
std::string remote_ssh_line(const RemoteProfile &p) { return "ssh " + std::string(p.port ? "-p " + std::to_string(p.port) + " " : "") + target(p); }

int remote_connect(const RemoteProfile &p, const std::string &pw, std::string *err) {
    std::string o;
    int c;
    if (p.proto == "ssh" || p.proto == "sftp") {
        c = run(askpass_env() + "ssh " + ssh_opts(p, false) + " " + q(target(p)) + " true", &o);
    } else {
        std::vector<RemoteEntry> e;
        c = remote_list(p, pw, p.path.empty() ? "/" : p.path, e, &o);
    }
    if (c != 0 && err) { size_t nl = o.find('\n'); *err = o.empty() ? "connection failed" : o.substr(0, nl); }
    return c;
}

void remote_disconnect(const RemoteProfile &p) {
    if (p.proto == "ssh" || p.proto == "sftp") { std::string o; run("ssh " + ssh_opts(p, false) + " -O exit " + q(target(p)), &o); }
}

int remote_list(const RemoteProfile &p, const std::string &pw, const std::string &dir, std::vector<RemoteEntry> &out, std::string *err) {
    out.clear();
    std::string o;
    int c;
    if (p.proto == "ssh") {
        c = run(askpass_env() + "ssh " + ssh_opts(p, false) + " " + q(target(p)) + " " + q("ls -1Ap -- " + q(dir)), &o);
        if (c == 0) {
            size_t pos = 0;
            while (pos < o.size()) {
                size_t nl = o.find('\n', pos);
                if (nl == std::string::npos) nl = o.size();
                std::string l = o.substr(pos, nl - pos);
                pos = nl + 1;
                if (l.empty()) continue;
                bool d = l.back() == '/';
                if (d) l.pop_back();
                out.push_back({l, d});
            }
        }
    } else if (p.proto == "sftp") {
        std::string cmd = "printf '%s\\n' " + q("ls -la " + q(dir)) + " | " + askpass_env() + "sftp -q -b - " + ssh_opts(p, true) + " " + q(target(p));
        c = run(cmd, &o, false);
        if (c == 0) parse_long(o, out);
    } else if (p.proto == "smb") {
        std::string share, rest;
        if (!smb_split(dir, &share, &rest)) {
            std::string auth = smb_auth(p, pw);
            c = run("smbclient -L " + q("//" + p.host) + (auth.empty() ? " -N" : " -A " + q(auth)) + (p.port ? " -p " + std::to_string(p.port) : "") + " -g; rc=$?; " + (auth.empty() ? "" : "rm -f " + q(auth) + "; ") + "exit $rc", &o);
            if (c == 0) {
                size_t pos = 0;
                while (pos < o.size()) {
                    size_t nl = o.find('\n', pos);
                    if (nl == std::string::npos) nl = o.size();
                    std::string l = o.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (l.compare(0, 5, "Disk|") == 0) { std::string n = l.substr(5, l.find('|', 5) - 5); if (!n.empty() && n.back() != '$') out.push_back({n, true}); }
                }
            }
        } else {
            c = run(smb_cmd(p, smb_auth(p, pw), share, "ls \"" + (rest.empty() ? "" : rest + "/") + "*\""), &o);
            if (c == 0) {
                size_t pos = 0;
                while (pos < o.size()) {   /* "  name      D  0  Tue Oct 14 12:00:00 2026" */
                    size_t nl = o.find('\n', pos);
                    if (nl == std::string::npos) nl = o.size();
                    std::string l = o.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (l.size() < 8 || l[0] != ' ' || l[1] != ' ') continue;
                    size_t d = l.find("  ", 2);
                    for (; d != std::string::npos; d = l.find("  ", d + 1)) {   /* the attribute letters follow the name after two or more spaces */
                        size_t a = d; while (a < l.size() && l[a] == ' ') a++;
                        size_t b = a; while (b < l.size() && isalpha((unsigned char)l[b])) b++;
                        if (b > a && b < l.size() && l[b] == ' ' && b - a <= 6 && l.find_first_not_of(" 0123456789", b) != std::string::npos) {
                            std::string name = l.substr(2, d - 2), attrs = l.substr(a, b - a);
                            if (name != "." && name != "..") out.push_back({name, attrs.find('D') != std::string::npos});
                            break;
                        }
                    }
                }
            }
        }
    } else {   /* ftp, ftps */
        std::string cfg = curl_cfg(p, pw);
        std::string u = url(p, dir);
        if (u.back() != '/') u += "/";
        c = run("curl -sS --connect-timeout 12 --max-time 40 -K " + q(cfg) + " " + q(u), &o);
        unlink(cfg.c_str());
        if (c == 0) parse_long(o, out);
    }
    if (c != 0 && err) { size_t nl = o.find('\n'); *err = o.empty() ? "listing failed" : o.substr(0, nl); }
    return c;
}

int remote_get(const RemoteProfile &p, const std::string &pw, const std::string &remote, const std::string &local, std::string *err) {
    std::string o;
    int c;
    if (p.proto == "ssh") c = run(askpass_env() + "ssh " + ssh_opts(p, false) + " " + q(target(p)) + " " + q("cat -- " + q(remote)) + " > " + q(local), &o);
    else if (p.proto == "sftp") c = run("printf '%s\\n' " + q("get " + q(remote) + " " + q(local)) + " | " + askpass_env() + "sftp -q -b - " + ssh_opts(p, true) + " " + q(target(p)), &o, false);
    else if (p.proto == "smb") {
        std::string share, rest;
        if (!smb_split(remote, &share, &rest)) return -1;
        c = run(smb_cmd(p, smb_auth(p, pw), share, "get " + q(rest) + " " + q(local)), &o);
    } else {
        std::string cfg = curl_cfg(p, pw);
        c = run("curl -sS --connect-timeout 12 --max-time 120 -K " + q(cfg) + " -o " + q(local) + " " + q(url(p, remote)), &o);
        unlink(cfg.c_str());
    }
    if (c != 0 && err) { size_t nl = o.find('\n'); *err = o.empty() ? "download failed" : o.substr(0, nl); }
    return c;
}

int remote_put(const RemoteProfile &p, const std::string &pw, const std::string &local, const std::string &remote, std::string *err) {
    std::string o;
    int c;
    if (p.proto == "ssh") c = run(askpass_env() + "ssh " + ssh_opts(p, false) + " " + q(target(p)) + " " + q("cat > " + q(remote)) + " < " + q(local), &o, false);
    else if (p.proto == "sftp") c = run("printf '%s\\n' " + q("put " + q(local) + " " + q(remote)) + " | " + askpass_env() + "sftp -q -b - " + ssh_opts(p, true) + " " + q(target(p)), &o, false);
    else if (p.proto == "smb") {
        std::string share, rest;
        if (!smb_split(remote, &share, &rest)) return -1;
        c = run(smb_cmd(p, smb_auth(p, pw), share, "put " + q(local) + " " + q(rest)), &o);
    } else {
        std::string cfg = curl_cfg(p, pw);
        c = run("curl -sS --connect-timeout 12 --max-time 120 -K " + q(cfg) + " -T " + q(local) + " " + q(url(p, remote)), &o);
        unlink(cfg.c_str());
    }
    if (c != 0 && err) { size_t nl = o.find('\n'); *err = o.empty() ? "upload failed" : o.substr(0, nl); }
    return c;
}
