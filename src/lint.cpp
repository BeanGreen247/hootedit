/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "lint.h"
#include "lint_priv.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

namespace {

using namespace lintp;

/* every position where `name` stands as a whole word followed by "(" (spaces allowed) */
void each_call(sv code, sv name, const std::function<void(size_t)> &f) {
    size_t p = 0;
    while ((p = code.find(name, p)) != sv::npos) {
        bool left = p == 0 || !(idc(code[p - 1]) || code[p - 1] == '.' || code[p - 1] == ':' || (p > 1 && code[p - 1] == '>' && code[p - 2] == '-'));
        size_t e = p + name.size();
        bool right = e >= code.size() || !idc(code[e]);
        size_t q = e;
        while (q < code.size() && code[q] == ' ') q++;
        if (left && right && q < code.size() && code[q] == '(') f(p);
        p = e;
    }
}

int lev(sv a, sv b) {   /* edit distance, small strings */
    if (a.size() > 40 || b.size() > 40) return 99;
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
        prev.swap(cur);
    }
    return prev[b.size()];
}

/* ---------------------------------------------------------------- Ansible tables */

const std::unordered_map<std::string, std::string> &fqcn_map() {
    static const std::unordered_map<std::string, std::string> m = [] {
        std::unordered_map<std::string, std::string> r;
        for (const char *n : {"add_host", "apt", "apt_key", "apt_repository", "assemble", "assert", "async_status", "blockinfile", "command", "copy", "cron", "deb822_repository",
                              "debconf", "debug", "dnf", "dnf5", "dpkg_selections", "expect", "fail", "fetch", "file", "find", "gather_facts", "get_url", "getent", "git", "group",
                              "group_by", "hostname", "import_playbook", "import_role", "import_tasks", "include_role", "include_tasks", "include_vars", "iptables", "known_hosts",
                              "lineinfile", "meta", "mount_facts", "package", "package_facts", "pause", "ping", "pip", "raw", "reboot", "replace", "rpm_key", "script", "service",
                              "service_facts", "set_fact", "set_stats", "setup", "shell", "slurp", "stat", "subversion", "systemd", "systemd_service", "sysvinit", "tempfile",
                              "template", "unarchive", "uri", "user", "validate_argument_spec", "wait_for", "wait_for_connection", "yum", "yum_repository"})
            r[n] = std::string("ansible.builtin.") + n;
        const std::pair<const char *, const char *> other[] = {
            {"synchronize", "ansible.posix.synchronize"}, {"firewalld", "ansible.posix.firewalld"}, {"mount", "ansible.posix.mount"}, {"sysctl", "ansible.posix.sysctl"},
            {"seboolean", "ansible.posix.seboolean"}, {"selinux", "ansible.posix.selinux"}, {"authorized_key", "ansible.posix.authorized_key"}, {"acl", "ansible.posix.acl"},
            {"patch", "ansible.posix.patch"}, {"ufw", "community.general.ufw"}, {"timezone", "community.general.timezone"}, {"ini_file", "community.general.ini_file"},
            {"modprobe", "community.general.modprobe"}, {"nmcli", "community.general.nmcli"}, {"npm", "community.general.npm"}, {"gem", "community.general.gem"},
            {"make", "community.general.make"}, {"alternatives", "community.general.alternatives"}, {"filesystem", "community.general.filesystem"}, {"lvg", "community.general.lvg"},
            {"lvol", "community.general.lvol"}, {"parted", "community.general.parted"}, {"htpasswd", "community.general.htpasswd"}, {"locale_gen", "community.general.locale_gen"},
            {"git_config", "community.general.git_config"}, {"pacman", "community.general.pacman"}, {"apk", "community.general.apk"}, {"zypper", "community.general.zypper"},
            {"docker_container", "community.docker.docker_container"}, {"docker_image", "community.docker.docker_image"}, {"docker_network", "community.docker.docker_network"},
            {"docker_volume", "community.docker.docker_volume"}, {"docker_compose", "community.docker.docker_compose"}, {"mysql_db", "community.mysql.mysql_db"},
            {"mysql_user", "community.mysql.mysql_user"}, {"postgresql_db", "community.postgresql.postgresql_db"}, {"postgresql_user", "community.postgresql.postgresql_user"},
            {"openssl_privatekey", "community.crypto.openssl_privatekey"}, {"x509_certificate", "community.crypto.x509_certificate"}, {"k8s", "kubernetes.core.k8s"},
            {"helm", "kubernetes.core.helm"}, {"ec2_instance", "amazon.aws.ec2_instance"}, {"s3_bucket", "amazon.aws.s3_bucket"}, {"win_copy", "ansible.windows.win_copy"},
            {"win_service", "ansible.windows.win_service"}, {"win_shell", "ansible.windows.win_shell"}, {"win_command", "ansible.windows.win_command"},
            {"win_file", "ansible.windows.win_file"}, {"win_feature", "ansible.windows.win_feature"}, {"win_package", "ansible.windows.win_package"}};
        for (auto &o : other) r[o.first] = o.second;
        return r;
    }();
    return m;
}

const std::set<std::string> &task_keywords() {
    static const std::set<std::string> s = {
        "name", "when", "register", "tags", "become", "become_user", "become_method", "become_flags", "become_exe", "vars", "loop", "loop_control", "until", "retries", "delay",
        "notify", "changed_when", "failed_when", "ignore_errors", "ignore_unreachable", "no_log", "delegate_to", "delegate_facts", "run_once", "environment", "args", "check_mode",
        "diff", "block", "rescue", "always", "listen", "any_errors_fatal", "throttle", "timeout", "connection", "module_defaults", "collections", "async", "poll", "port",
        "remote_user", "debugger", "local_action", "action", "hosts", "roles", "tasks", "pre_tasks", "post_tasks", "handlers", "gather_facts", "vars_files", "vars_prompt", "serial",
        "strategy", "max_fail_percentage", "force_handlers", "order", "fact_path", "gather_subset", "gather_timeout", "no_log", "noqa", "import_playbook"};
    return s;
}

bool is_task_keyword(const std::string &k) { return task_keywords().count(k) || starts(k, "with_"); }

std::string short_module(const std::string &k) {
    for (const char *pre : {"ansible.builtin.", "ansible.legacy.", "ansible.windows.", "ansible.posix.", "community.general."})
        if (starts(k, pre)) return k.substr(strlen(pre));
    return k;
}

bool known_module(const std::string &k) {
    if (k.find('.') != std::string::npos) return starts(k, "ansible.") || starts(k, "community.") || starts(k, "amazon.") || starts(k, "kubernetes.");
    return fqcn_map().count(k) != 0;
}

/* ---------------------------------------------------------------- Ansible task rules */

struct Entry { std::string key; sv value; uint32_t line; size_t k0, k1, v0; size_t indent; sv text; };

struct Cand {
    size_t item_indent = 0, key_indent = 0;
    uint32_t line0 = 0;
    bool task_ctx = false;
    std::vector<Entry> keys;      /* keys at the item's key indent */
    std::vector<Entry> sub;       /* deeper keys (module arguments, vars, ...) */
};

const Entry *find_key(const std::vector<Entry> &v, const char *k) { for (const Entry &e : v) if (e.key == k) return &e; return nullptr; }

bool truthy_word(sv v) {
    static const char *w[] = {"yes", "no", "on", "off", "Yes", "No", "On", "Off", "YES", "NO", "ON", "OFF", "True", "False", "TRUE", "FALSE", "y", "n"};
    for (const char *x : w) if (v == x) return true;
    return false;
}

struct AnsibleState {
    std::vector<Cand> open;
};

void diag_at(Ctx &c, const Entry &e, size_t c0, size_t c1, int sev, const char *code, const std::string &msg, Diag **out = nullptr) {
    // diagnostics for a candidate are emitted after its lines were read, so the noqa text and the line number come from the entry
    Ctx cc{c.out, c.max};
    cc.line = e.line;
    cc.cur = e.text;
    Diag &d = add(cc, c0, c1, sev, code, msg);
    if (out) *out = &d;
}

void eval_task(Ctx &c, Cand &t) {
    const Entry *hosts = find_key(t.keys, "hosts");
    const Entry *nm = find_key(t.keys, "name");
    if (hosts || find_key(t.keys, "import_playbook")) {
        if (!nm && hosts) diag_at(c, *hosts, hosts->k0, hosts->k1, SEV_HINT, "name[play]", "All plays should be named");
        return;
    }
    if (!t.task_ctx) return;
    std::vector<const Entry *> actions;
    for (const Entry &e : t.keys) if (!is_task_keyword(e.key)) actions.push_back(&e);
    const Entry *blk = find_key(t.keys, "block");
    const Entry *local = find_key(t.keys, "local_action");
    if (local) diag_at(c, *local, local->k0, local->k1, SEV_WARN, "deprecated-local-action", "Do not use 'local_action'; use 'delegate_to: localhost'");
    if (!blk && actions.empty() && !local && !find_key(t.keys, "action")) return;   /* not a task */
    if (actions.size() > 1) diag_at(c, *actions[1], actions[1]->k0, actions[1]->k1, SEV_ERROR, "multiple-actions", "A task can run only one action, but '" + actions[0]->key + "' and '" + actions[1]->key + "' are both set");

    const Entry *first = t.keys.empty() ? nullptr : &t.keys[0];
    if (nm) {
        sv v = nm->value;
        if (!v.empty() && islower((unsigned char)v[0]) && v[0] != '{') diag_at(c, *nm, nm->v0, nm->v0 + 1, SEV_HINT, "name[casing]", "Task names should start with an uppercase letter");
        if (first && first != nm && !blk) diag_at(c, *nm, nm->k0, nm->k1, SEV_HINT, "key-order[task]", "'name' should be the first key of the task");
    }
    if (blk && !nm) { /* blocks are fine unnamed */ }
    if (actions.empty()) {
        for (const char *k : {"ignore_errors"}) { (void)k; }
    }
    const Entry *act = actions.empty() ? nullptr : actions[0];
    if (!act && !blk) return;
    if (act && !nm && known_module(act->key)) diag_at(c, *act, act->k0, act->k1, SEV_WARN, "name[missing]", "All tasks should be named");

    std::string mod;
    if (act) {
        mod = act->key;
        if (mod.find('.') == std::string::npos) {
            auto it = fqcn_map().find(mod);
            if (it != fqcn_map().end()) {
                Diag *d = nullptr;
                bool core = starts(it->second, "ansible.builtin.");
                diag_at(c, *act, act->k0, act->k1, SEV_WARN, core ? "fqcn[action-core]" : "fqcn[action]", "Use the full name '" + it->second + "' instead of '" + mod + "'", &d);
                if (d && d->code.size()) fixit(*d, act->k0, act->k1, it->second);
            } else if (mod.size() >= 4) {   /* a misspelt module or keyword */
                std::string best;
                int bd = 3;
                for (auto &kv : fqcn_map()) { int dd = lev(mod, kv.first); if (dd < bd) { bd = dd; best = kv.first; } }
                for (const std::string &kw : task_keywords()) { int dd = lev(mod, kw); if (dd < bd) { bd = dd; best = kw; } }
                if (!best.empty() && bd <= 2 && best.size() >= 4) {
                    Diag *d = nullptr;
                    diag_at(c, *act, act->k0, act->k1, SEV_WARN, "typo", "Unknown key '" + mod + "'. Did you mean '" + best + "'?", &d);
                    if (d && d->code.size()) fixit(*d, act->k0, act->k1, best);
                }
            }
        }
    }
    std::string sm = short_module(mod);
    auto param = [&](const char *k) -> const Entry * {
        for (const Entry &e : t.sub) if (e.key == k && e.indent == t.sub.front().indent) return &e;
        return nullptr;
    };
    auto freeform_has = [&](const char *k) { if (!act) return false; std::string needle = std::string(k) + "="; return act->value.find(needle) != sv::npos; };

    const bool cmdlike = sm == "command" || sm == "shell" || sm == "raw" || sm == "script" || sm == "win_command" || sm == "win_shell";
    std::string cmd;
    if (act && cmdlike) {
        if (!act->value.empty() && act->value != "|" && act->value != ">" && act->value != "|-" && act->value != ">-") cmd = std::string(act->value);
        else if (const Entry *e = param("cmd")) cmd = std::string(e->value);
        else { /* a block scalar: the text is on the following lines, which we do not keep */ }
        if (starts(cmd, "\"") || starts(cmd, "'")) cmd = cmd.substr(1);
    }
    if (act && cmdlike) {
        bool exempt = find_key(t.keys, "changed_when") || param("creates") || param("removes") || freeform_has("creates") || freeform_has("removes");
        if (!exempt) diag_at(c, *act, act->k0, act->k1, SEV_WARN, "no-changed-when", "Commands should not change things if nothing needs doing: add 'changed_when' (or 'creates'/'removes')");
        if (!cmd.empty()) {
            sv cw = word_at(cmd, 0);
            bool cr_exempt = param("creates") || param("removes") || freeform_has("creates") || freeform_has("removes");
            if (!cr_exempt && (sm == "command" || sm == "shell")) {
                static const std::pair<const char *, const char *> mods[] = {
                    {"git", "git"}, {"curl", "get_url or uri"}, {"wget", "get_url"}, {"apt", "apt"}, {"apt-get", "apt"}, {"yum", "yum"}, {"dnf", "dnf"}, {"systemctl", "systemd"},
                    {"service", "service"}, {"mkdir", "file"}, {"rm", "file"}, {"ln", "file"}, {"chmod", "file"}, {"chown", "file"}, {"tar", "unarchive"}, {"unzip", "unarchive"},
                    {"sed", "replace or lineinfile"}, {"useradd", "user"}, {"adduser", "user"}, {"groupadd", "group"}, {"rsync", "synchronize"}, {"mount", "mount"},
                    {"hostnamectl", "hostname"}, {"pip", "pip"}, {"pip3", "pip"}, {"cp", "copy"}, {"mv", "copy"}, {"touch", "file"}, {"crontab", "cron"}, {"ufw", "ufw"}, {"sysctl", "sysctl"}};
                for (auto &m : mods) if (cw == m.first) { diag_at(c, *act, act->k0, act->k1, SEV_WARN, "command-instead-of-module", std::string("Use the '") + m.second + "' module instead of running '" + std::string(cw) + "'"); break; }
            }
            if (cw == "sudo") diag_at(c, *act, act->k0, act->k1, SEV_WARN, "sudo-in-command", "Use 'become: true' instead of sudo in the command");
            if (sm == "shell") {
                bool metas = cmd.find_first_of("|&;<>`$()*?~!\\\n{}") != std::string::npos;
                if (!metas) diag_at(c, *act, act->k0, act->k1, SEV_WARN, "command-instead-of-shell", "Use 'command' when you do not need shell features");
                size_t pp = cmd.find('|');
                bool pipe = false;
                while (pp != std::string::npos) { if (pp + 1 < cmd.size() && cmd[pp + 1] == '|') { pp = cmd.find('|', pp + 2); continue; } pipe = true; break; }
                if (pipe && cmd.find("pipefail") == std::string::npos) diag_at(c, *act, act->k0, act->k1, SEV_WARN, "risky-shell-pipe", "Use 'set -o pipefail' in a shell command with a pipe, or the pipe hides failures");
            }
            size_t eq = 0;
            while (eq < cw.size() && (isupper((unsigned char)cw[eq]) || cw[eq] == '_' || isdigit((unsigned char)cw[eq]))) eq++;
            if (eq > 0 && eq < cmd.size() && cmd[eq] == '=' && eq == word_at(cmd, 0).size()) diag_at(c, *act, act->k0, act->k1, SEV_WARN, "inline-env-var", "Use the 'environment' keyword instead of VAR=value in the command");
        }
    }
    if (act && !cmdlike && !act->value.empty() && act->value[0] != '{' && act->value[0] != '"' && act->value[0] != '\'' && sm != "include_tasks" && sm != "import_tasks" && sm != "include_role" &&
        sm != "import_role" && sm != "meta" && sm != "debug" && sm != "set_fact" && sm != "include_vars" && sm != "assert") {
        size_t eq = act->value.find('=');
        if (eq != sv::npos && eq > 0 && idc(act->value[0]) && word_at(act->value, 0).size() == eq) diag_at(c, *act, act->v0, act->v0 + act->value.size(), SEV_WARN, "no-free-form", "Avoid free-form 'key=value' arguments; write them as a YAML mapping");
    }
    // permissions
    static const std::set<std::string> needs_mode = {"copy", "template", "file", "assemble", "get_url", "replace", "unarchive", "lineinfile", "blockinfile", "ini_file", "synchronize"};
    static const std::set<std::string> needs_mode_strict = {"copy", "template", "file", "assemble", "get_url", "replace"};
    if (act && needs_mode_strict.count(sm)) {
        const Entry *mode = param("mode");
        const Entry *state = param("state");
        bool skip = state && (state->value == "absent" || state->value == "link" || state->value == "hard");
        if (!mode && !freeform_has("mode") && !skip) diag_at(c, *act, act->k0, act->k1, SEV_WARN, "risky-file-permissions", "File permissions unset or incorrect: set 'mode' (e.g. \"0644\")");
    }
    (void)needs_mode;
    for (const Entry &e : t.sub) {
        if (e.key == "mode" && !e.value.empty()) {
            bool digits = true;
            for (char ch : e.value) if (!isdigit((unsigned char)ch)) digits = false;
            if (digits && ((e.value.size() == 3) || (e.value.size() == 4 && e.value[0] != '0'))) {
                Diag *d = nullptr;
                diag_at(c, e, e.v0, e.v0 + e.value.size(), SEV_WARN, "risky-octal", "Octal file permissions need a leading zero or quotes, or YAML reads them as a decimal number", &d);
                if (d && d->code.size()) fixit(*d, e.v0, e.v0 + e.value.size(), "\"" + std::string(e.value.size() == 3 ? "0" : "") + std::string(e.value) + "\"");
            }
        }
    }
    // packages
    static const std::set<std::string> pkg = {"apt", "yum", "dnf", "package", "pip", "apk", "zypper", "pacman", "npm", "gem", "homebrew", "dnf5"};
    if (act && pkg.count(sm)) {
        const Entry *st = param("state");
        if ((st && st->value == "latest") || freeform_has("state") ) {
            if (st && st->value == "latest") diag_at(c, *st, st->v0, st->v0 + st->value.size(), SEV_WARN, "package-latest", "Package installs should not use 'latest': pin a version or use 'present'");
        }
    }
    // secrets in arguments
    if (act && !find_key(t.keys, "no_log")) {
        for (const Entry &e : t.sub) {
            if ((e.key == "password" || e.key == "passwd" || e.key == "api_key" || e.key == "private_key" || e.key == "secret" || e.key == "token") && !e.value.empty() && e.value != "omit") {
                diag_at(c, *act, act->k0, act->k1, SEV_WARN, "no-log-password", "Tasks with a '" + e.key + "' argument should set 'no_log: true' so the value does not end up in the logs");
                break;
            }
        }
    }
    if (const Entry *ie = find_key(t.keys, "ignore_errors"))
        if (ie->value == "true" || ie->value == "yes" || ie->value == "True") diag_at(c, *ie, ie->k0, ie->k1, SEV_HINT, "ignore-errors", "Use 'failed_when' and 'register' instead of 'ignore_errors'");
    if (const Entry *bu = find_key(t.keys, "become_user"))
        if (!find_key(t.keys, "become")) diag_at(c, *bu, bu->k0, bu->k1, SEV_WARN, "partial-become", "'become_user' without 'become: true' does nothing");
    if (const Entry *w = find_key(t.keys, "when")) {
        sv v = w->value;
        for (const char *lit : {"== True", "== true", "== False", "== false", "!= True", "!= true", "!= False", "!= false", "== yes", "== no"})
            if (v.find(lit) != sv::npos) { diag_at(c, *w, w->v0, w->v0 + v.size(), SEV_HINT, "literal-compare", "Do not compare to a literal boolean: write 'when: x' or 'when: not x'"); break; }
        if (v.find("== \"\"") != sv::npos || v.find("== ''") != sv::npos || v.find("!= \"\"") != sv::npos || v.find("!= ''") != sv::npos)
            diag_at(c, *w, w->v0, w->v0 + v.size(), SEV_HINT, "empty-string-compare", "Use 'when: var | length > 0' (or 'not var') instead of comparing to an empty string");
        if (v.find("{{") != sv::npos) {
            Diag *d = nullptr;
            diag_at(c, *w, w->v0, w->v0 + v.size(), SEV_WARN, "no-jinja-when", "Conditions are Jinja already: remove the '{{ }}' from 'when'", &d);
            sv in = v;
            if (!in.empty() && (in.front() == '"' || in.front() == '\'') && in.back() == in.front()) in = in.substr(1, in.size() - 2);
            in = trim(in);
            if (d && d->code.size() && starts(in, "{{") && ends(in, "}}") && in.find("{{", 2) == sv::npos) fixit(*d, w->v0, w->v0 + v.size(), std::string(trim(in.substr(2, in.size() - 4))));
        }
    }
    for (const Entry &e : t.sub)
        if (e.key == "src" && (starts(e.value, "../") || e.value.find("/../") != sv::npos) && (sm == "copy" || sm == "template"))
            diag_at(c, e, e.v0, e.v0 + e.value.size(), SEV_HINT, "no-relative-paths", "Use the role's own 'files/' or 'templates/' lookup instead of a relative path with '..'");
}

/* ---------------------------------------------------------------- YAML / Ansible pass */

struct MapKeys { size_t indent; std::vector<std::string> keys; };

void check_jinja_spacing(Ctx &c, sv line) {
    size_t p = 0;
    while ((p = line.find("{{", p)) != sv::npos) {
        if (p > 0 && line[p - 1] == '{') { p += 2; continue; }
        size_t e = line.find("}}", p + 2);
        if (e == sv::npos) break;
        bool lo = p + 2 < line.size() && (line[p + 2] == ' ' || line[p + 2] == '-' || line[p + 2] == '+');
        bool hi = e > 0 && (line[e - 1] == ' ' || line[e - 1] == '-' || line[e - 1] == '+');
        if (!lo || !hi) {
            sv inner = line.substr(p + 2, e - p - 2);
            sv t = trim(inner);
            if (!t.empty() && t.find("{{") == sv::npos) {
                Diag &d = add(c, p, e + 2, SEV_HINT, "jinja[spacing]", "Jinja expressions read better with a space inside the braces: {{ expr }}");
                if (!d.code.empty()) fixit(d, p, e + 2, "{{ " + std::string(t) + " }}");
            }
        }
        p = e + 2;
    }
}

void yaml_pass(Ctx &c, sv text, bool ansible, bool tasks_file) {
    std::vector<MapKeys> maps;
    struct Container { size_t indent; bool tasklist; };
    std::vector<Container> ctx;
    std::vector<Cand> open;
    size_t block_skip = (size_t)-1;      /* inside a block scalar whose key sat at this indent */
    YLine y;
    uint32_t ln = 0;
    size_t pos = 0;
    auto close_to = [&](size_t indent, bool dash_line) {
        while (!open.empty() && (dash_line ? open.back().item_indent >= indent : indent <= open.back().item_indent)) {
            if (ansible) eval_task(c, open.back());
            open.pop_back();
        }
    };
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == sv::npos) e = text.size();
        sv line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        c.line = ln;
        c.cur = line;
        if (starts(line, "---") && (line.size() == 3 || line[3] == ' ' || line[3] == '\t')) {   /* a new document: keys of the last one do not count */
            for (size_t k = open.size(); k-- > 0;) { if (ansible) eval_task(c, open[k]); }
            open.clear(); maps.clear(); ctx.clear(); block_skip = (size_t)-1;
            c.cur = sv();
            ln++; pos = e + 1;
            if (e >= text.size()) break;
            continue;
        }
        parse_yline(line, y);
        bool in_block = false;
        if (block_skip != (size_t)-1) {
            if (y.blank && line.find_first_not_of(" \t") == sv::npos) in_block = true;
            else if (!line.empty() && (y.indent > block_skip)) in_block = true;
            else block_skip = (size_t)-1;
        }
        if (!in_block && !y.blank) {
            if (y.tab_indent) add(c, 0, y.indent, SEV_ERROR, "no-tabs", "YAML cannot be indented with tabs; use spaces");
            if (y.is_key) {
                // duplicate keys in one mapping
                size_t ki = y.kp;
                if (y.dashes > 0) { while (!maps.empty() && maps.back().indent >= y.indent) maps.pop_back(); }
                while (!maps.empty() && maps.back().indent > ki) maps.pop_back();
                if (maps.empty() || maps.back().indent < ki) maps.push_back({ki, {}});
                std::string k(y.key);
                auto &ks = maps.back().keys;
                if (std::find(ks.begin(), ks.end(), k) != ks.end() && k != "<<") add(c, y.k0, y.k1, SEV_ERROR, "duplicate-key", "Duplicate key '" + k + "' in this mapping; the last one wins");
                else ks.push_back(k);
                if (ansible) {
                    while (!ctx.empty() && ctx.back().indent >= ki) ctx.pop_back();
                    static const std::set<std::string> lists = {"tasks", "pre_tasks", "post_tasks", "handlers", "block", "rescue", "always"};
                    if (lists.count(k) && y.value.empty()) ctx.push_back({ki, true});
                }
                if (ansible && truthy_word(y.value) && !(y.value == "y" || y.value == "n")) {
                    Diag &d = add(c, y.v0, y.v0 + y.value.size(), SEV_HINT, "yaml[truthy]", "Use 'true' or 'false' instead of '" + std::string(y.value) + "'");
                    if (!d.code.empty()) {
                        bool pos_ = y.value == "yes" || y.value == "Yes" || y.value == "YES" || y.value == "on" || y.value == "On" || y.value == "ON" || y.value == "True" || y.value == "TRUE";
                        fixit(d, y.v0, y.v0 + y.value.size(), pos_ ? "true" : "false");
                    }
                }
                if (!y.value.empty() && (y.value[0] == '|' || y.value[0] == '>')) block_skip = y.indent;
            }
            if (ansible) {
                if (y.dashes > 0) {
                    close_to(y.indent, true);
                    Cand cd;
                    cd.item_indent = y.indent; cd.key_indent = y.kp; cd.line0 = ln;
                    bool in_list = !ctx.empty() && ctx.back().tasklist && y.indent >= ctx.back().indent;
                    cd.task_ctx = in_list || (tasks_file && ctx.empty() && y.indent == 0);
                    open.push_back(std::move(cd));
                } else {
                    close_to(y.indent, false);
                }
                if (!open.empty() && y.is_key) {
                    Cand &cd = open.back();
                    Entry en{std::string(y.key), y.value, ln, y.k0, y.k1, y.v0, y.kp, line};
                    if (y.kp == cd.key_indent) cd.keys.push_back(std::move(en));
                    else if (y.kp > cd.key_indent) {
                        if (cd.sub.empty() || y.kp >= cd.sub.front().indent) { if (cd.sub.empty()) cd.sub.reserve(8); cd.sub.push_back(std::move(en)); }
                    }
                }
                if (line.find("{{") != sv::npos) check_jinja_spacing(c, line);
            }
        }
        if (!y.blank || line.find("{{") == sv::npos) {}
        ln++;
        pos = e + 1;
        if (e >= text.size()) break;
    }
    c.cur = sv();
    for (size_t i = open.size(); i-- > 0;) { if (ansible) eval_task(c, open[i]); }
}

/* ---------------------------------------------------------------- JSON */

struct JsonP {
    const char *p, *end, *base;
    std::string err;
    const char *err_at = nullptr;
    int depth = 0;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool fail(const char *m) { if (err.empty()) { err = m; err_at = p; } return false; }
    bool str() {
        p++;
        while (p < end) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') { p++; return true; }
            if (*p == '\n') return fail("unterminated string");
            p++;
        }
        return fail("unterminated string");
    }
    bool value() {
        ws();
        if (p >= end) return fail("unexpected end of file");
        if (++depth > 512) return fail("nested too deeply");
        bool ok = false;
        char c = *p;
        if (c == '{') {
            p++; ws();
            if (p < end && *p == '}') { p++; ok = true; }
            else for (;;) {
                ws();
                if (p >= end || *p != '"') { ok = fail("expected a string key"); break; }
                if (!str()) break;
                ws();
                if (p >= end || *p != ':') { ok = fail("expected ':' after the key"); break; }
                p++;
                if (!value()) break;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; ok = true; break; }
                ok = fail("expected ',' or '}'"); break;
            }
        } else if (c == '[') {
            p++; ws();
            if (p < end && *p == ']') { p++; ok = true; }
            else for (;;) {
                if (!value()) break;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; ok = true; break; }
                ok = fail("expected ',' or ']'"); break;
            }
        } else if (c == '"') ok = str();
        else if (c == '-' || isdigit((unsigned char)c)) { const char *s = p; if (*p == '-') p++; while (p < end && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++; ok = p > s; if (!ok) fail("bad number"); }
        else if (end - p >= 4 && !memcmp(p, "true", 4)) { p += 4; ok = true; }
        else if (end - p >= 5 && !memcmp(p, "false", 5)) { p += 5; ok = true; }
        else if (end - p >= 4 && !memcmp(p, "null", 4)) { p += 4; ok = true; }
        else ok = fail("unexpected character");
        depth--;
        return ok;
    }
};

void json_check(Ctx &c, const char *text, size_t n) {
    JsonP j{text, text + n, text};
    bool ok = j.value();
    if (ok) { j.ws(); if (j.p < j.end) { ok = false; j.fail("extra text after the JSON value"); } }
    if (ok) return;
    const char *at = j.err_at ? j.err_at : j.p;
    uint32_t line = 0;
    const char *ls = text;
    for (const char *q = text; q < at && q < text + n; q++) if (*q == '\n') { line++; ls = q + 1; }
    Ctx cc{c.out, c.max};
    cc.line = line;
    add(cc, (size_t)(at - ls), (size_t)(at - ls) + 1, SEV_ERROR, "json-syntax", "JSON: " + j.err);
}

/* ---------------------------------------------------------------- per-language line checks */

void check_secrets(Ctx &c, sv line) {
    if (line.size() < 12) return;
    size_t p;
    auto alnum_run = [&](size_t from, size_t want) { size_t k = 0; while (from + k < line.size() && (isalnum((unsigned char)line[from + k]) || line[from + k] == '-' || line[from + k] == '_')) k++; return k >= want; };
    if ((p = line.find("AKIA")) != sv::npos && p + 20 <= line.size()) {
        bool ok = true;
        for (size_t i = 0; i < 16; i++) if (!(isupper((unsigned char)line[p + 4 + i]) || isdigit((unsigned char)line[p + 4 + i]))) ok = false;
        if (ok && (p + 20 == line.size() || !isalnum((unsigned char)line[p + 20]))) add(c, p, p + 20, SEV_ERROR, "secret", "This looks like an AWS access key id; do not commit credentials");
    }
    if ((p = line.find("-----BEGIN ")) != sv::npos) {   /* the whole header: BEGIN, upper-case words, PRIVATE KEY, dashes (a string in code that only mentions it does not match) */
        size_t q = p + 11;
        while (q < line.size() && (isupper((unsigned char)line[q]) || line[q] == ' ')) q++;
        if (line.compare(q, 5, "-----") == 0 && q >= 11 && line.substr(p + 11, q - p - 11).find("PRIVATE KEY") != sv::npos) add(c, p, q + 5, SEV_ERROR, "secret", "A private key is in this file; do not commit it");
    }
    if ((p = line.find("ghp_")) != sv::npos && alnum_run(p + 4, 36)) add(c, p, p + 40, SEV_ERROR, "secret", "This looks like a GitHub token; do not commit credentials");
    for (const char *sl : {"xoxb-", "xoxp-"})
        if ((p = line.find(sl)) != sv::npos && alnum_run(p + 5, 20)) { add(c, p, std::min(line.size(), p + 25), SEV_ERROR, "secret", "This looks like a Slack token; do not commit credentials"); break; }
}

void check_shell(Ctx &c, sv line, bool bash_shebang_sh, bool set_e, bool set_u) {
    sv t = trim(line);
    if (t.empty() || t[0] == '#') return;
    size_t lead = line.find_first_not_of(" \t");
    sv first = word_at(t, 0);
    // backticks
    bool sq = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '\'') sq = !sq;
        else if (!sq && line[i] == '#' && (i == 0 || line[i - 1] == ' ')) break;
        else if (!sq && line[i] == '`') { size_t j = line.find('`', i + 1); if (j != sv::npos) { add(c, i, j + 1, SEV_HINT, "shell-backticks", "Use $( ) instead of backticks: it nests and is easier to read"); } break; }
    }
    if ((first == "rm") && t.find(" -") != sv::npos) {
        size_t d = t.find('$');
        if (d != sv::npos && (t.find("-r") != sv::npos || t.find("-R") != sv::npos)) {
            size_t after = d + 1;
            if (after < t.size() && t[after] == '{') { size_t cb = t.find('}', after); after = cb == sv::npos ? t.size() : cb + 1; }
            else while (after < t.size() && idc(t[after])) after++;
            bool guarded = t.substr(d, after - d).find(":?") != sv::npos;
            bool path_start = (after < t.size() && t[after] == '/') || (d > 0 && t[d - 1] == '/') || (d > 0 && t[d - 1] == '"' && after < t.size() + 0 && (t[after] == '"') && false);
            if (!guarded && !set_u && path_start) add(c, lead + d, lead + after, SEV_WARN, "rm-rf-variable", "If this variable is empty, 'rm -rf' runs on '/': use \"${VAR:?}\" to stop when it is unset");
        }
    }
    static const char *quote_cmds[] = {"rm", "cd", "mv", "cp", "mkdir", "cat", "ls", "chmod", "chown", "kill", "ln", "touch", "source", "."};
    bool want = false;
    for (const char *qc : quote_cmds) if (first == qc) want = true;
    bool in_test = starts(t, "[ ") || starts(t, "if [ ") || starts(t, "while [ ") || starts(t, "test ") || starts(t, "elif [ ");
    if (want || in_test) {
        bool dq = false, sq2 = false;
        for (size_t i = 0; i < t.size(); i++) {
            char ch = t[i];
            if (ch == '"' && !sq2) dq = !dq;
            else if (ch == '\'' && !dq) sq2 = !sq2;
            else if (ch == '#' && !dq && !sq2 && i > 0 && t[i - 1] == ' ') break;
            else if (ch == '$' && i + 1 < t.size() && t[i + 1] == '(') {   /* a command substitution has its own quoting: skip it whole */
                int depth = 0;
                size_t k = i + 1;
                for (; k < t.size(); k++) { if (t[k] == '(') depth++; else if (t[k] == ')' && --depth == 0) break; }
                i = k;
            }
            else if (ch == '$' && !dq && !sq2 && i + 1 < t.size() && (idc(t[i + 1]) || (t[i + 1] == '{' && !(i + 2 < t.size() && t[i + 2] == '#'))) && !isdigit((unsigned char)t[i + 1]) && t[i + 1] != '?') {
                size_t e = i + 1;
                if (t[e] == '{') { size_t cb = t.find('}', e); e = cb == sv::npos ? t.size() : cb + 1; } else while (e < t.size() && idc(t[e])) e++;
                if (in_test && t.find("[[") != sv::npos) break;
                Diag &d = add(c, lead + i, lead + e, SEV_HINT, "shell-unquoted", "Quote this variable (\"" + std::string(t.substr(i, e - i)) + "\"): spaces or * in the value will split or expand it");
                if (!d.code.empty()) fixit(d, lead + i, lead + e, "\"" + std::string(t.substr(i, e - i)) + "\"");
                break;
            }
        }
    }
    if (first == "cd" && !set_e && t.find("||") == sv::npos && t.find("&&") == sv::npos && t.find(';') == sv::npos) add(c, lead, lead + 2, SEV_HINT, "shell-cd", "'cd' can fail; add '|| exit' so the script does not carry on in the wrong directory");
    if ((t.find("curl ") != sv::npos || t.find("wget ") != sv::npos) && (t.find("| sh") != sv::npos || t.find("| bash") != sv::npos || t.find("|sh") != sv::npos || t.find("|bash") != sv::npos || t.find("| sudo") != sv::npos))
        add(c, lead, lead + t.size(), SEV_WARN, "pipe-to-shell", "Piping a download straight into a shell runs unreviewed code");
    if (t.find("chmod 777") != sv::npos || t.find("chmod -R 777") != sv::npos) add(c, lead, lead + t.size(), SEV_WARN, "chmod-777", "World-writable permissions; give only the access that is needed");
    if (starts(t, "for ") && t.find("$(ls") != sv::npos) add(c, lead, lead + t.size(), SEV_WARN, "for-ls", "Do not loop over $(ls ...): names with spaces break it; use a glob");
    if (bash_shebang_sh && (t.find(" == ") != sv::npos && (in_test || t.find("[ ") != sv::npos)) && t.find("[[") == sv::npos) add(c, lead, lead + t.size(), SEV_WARN, "sh-bashism", "'==' in [ ] is a bashism; in sh use '='");
    if (first == "eval") add(c, lead, lead + 4, SEV_INFO, "shell-eval", "eval runs whatever the string contains; check where it comes from");
}

void check_python(Ctx &c, sv code, sv raw) {
    sv t = trim(code);
    if (t.empty()) return;
    size_t lead = code.find_first_not_of(" \t");
    if (starts(t, "def ")) {
        size_t o = t.find('('), cl = t.rfind(')');
        if (o != sv::npos && cl != sv::npos && cl > o) {
            sv args = t.substr(o, cl - o);
            for (const char *m : {"=[]", "={}", "=set()", "=list()", "=dict()", "= []", "= {}", "= set()", "= list()", "= dict()"}) {
                size_t p = args.find(m);
                if (p != sv::npos) { add(c, lead + o + p, lead + o + p + strlen(m), SEV_WARN, "py-mutable-default", "A mutable default argument is created once and shared by every call; use None and build it inside"); break; }
            }
        }
    }
    if (t == "except:" || starts(t, "except:")) add(c, lead, lead + 7, SEV_WARN, "py-bare-except", "A bare 'except:' also catches KeyboardInterrupt and SystemExit; name the exception");
    for (const char *cmp : {"== None", "!= None"}) {
        size_t p = t.find(cmp);
        if (p != sv::npos) { Diag &d = add(c, lead + p, lead + p + 7, SEV_HINT, "py-none-compare", "Compare to None with 'is None' / 'is not None'"); if (!d.code.empty()) fixit(d, lead + p, lead + p + 7, cmp[0] == '=' ? "is None" : "is not None"); break; }
    }
    if (starts(t, "from ") && ends(t, "import *")) add(c, lead, lead + t.size(), SEV_HINT, "py-star-import", "'import *' hides where names come from");
    if (starts(t, "assert (") && t.find(',') != sv::npos && t.back() == ')') add(c, lead, lead + 6, SEV_WARN, "py-assert-tuple", "assert on a tuple is always true; remove the parentheses");
    if (starts(t, "print ") && !starts(t, "print (") && !starts(t, "print(") && t.size() > 6 && t[6] != '=' ) add(c, lead, lead + 5, SEV_ERROR, "py-print-statement", "Python 3 needs print(...)");
    each_call(t, "eval", [&](size_t p) { add(c, lead + p, lead + p + 4, SEV_INFO, "py-eval", "eval runs arbitrary code; ast.literal_eval or a parser is safer"); });
    each_call(t, "exec", [&](size_t p) { add(c, lead + p, lead + p + 4, SEV_INFO, "py-exec", "exec runs arbitrary code"); });
    size_t ind = raw.find_first_not_of(" \t");
    if (ind != sv::npos && raw.substr(0, ind).find('\t') != sv::npos && raw.substr(0, ind).find(' ') != sv::npos) add(c, 0, ind, SEV_WARN, "py-mixed-indent", "Tabs and spaces are mixed in this indentation (a TabError in Python 3)");
}

void check_c(Ctx &c, sv code, bool header) {
    sv t = trim(code);
    if (t.empty() || t[0] == '#') return;
    if (code.find('(') == sv::npos) return;   /* every check below needs a call or a condition */
    (void)header;
    size_t n = code.size();
    for (size_t i = 0; i < n;) {   /* one pass over the identifiers; a name counts when a "(" follows it */
        if (!(isalpha((unsigned char)code[i]) || code[i] == '_')) { i++; continue; }
        size_t s0 = i;
        while (i < n && idc(code[i])) i++;
        size_t len = i - s0;
        if (len < 2 || len > 7) continue;
        if (s0 > 0 && (code[s0 - 1] == '.' || code[s0 - 1] == ':' || (s0 > 1 && code[s0 - 1] == '>' && code[s0 - 2] == '-'))) continue;
        size_t q = i;
        while (q < n && code[q] == ' ') q++;
        if (q >= n || code[q] != '(') continue;
        sv w = code.substr(s0, len);
        if (w == "gets") {
            int depth = 0;
            bool comma = false, decl = s0 > 0 && (idc(code[s0 - 1]) || code[s0 - 1] == ' ' || code[s0 - 1] == '*' || code[s0 - 1] == '&') && !trim(code.substr(0, s0)).empty() && trim(code.substr(0, s0)).back() != '(' && trim(code.substr(0, s0)).back() != '=' && trim(code.substr(0, s0)).back() != ',' && trim(code.substr(0, s0)).find("return") == sv::npos;
            for (size_t k = q; k < n; k++) { if (code[k] == '(') depth++; else if (code[k] == ')') { if (--depth == 0) break; } else if (code[k] == ',' && depth == 1) comma = true; }
            if (!comma && !decl) add(c, s0, i, SEV_ERROR, "c-gets", "gets cannot limit its input and was removed from C11; use fgets");   /* a function of your own named gets takes more arguments or has a return type before it */
        }
        else if (w == "strcpy" || w == "strcat" || w == "sprintf") add(c, s0, i, SEV_WARN, "c-unbounded", std::string(w) + " does not check the size of the destination; use snprintf / a bounded copy");
        else if (w == "vsprintf") add(c, s0, i, SEV_WARN, "c-unbounded", "vsprintf does not check the size of the destination; use vsnprintf");
        else if (w == "mktemp") add(c, s0, i, SEV_WARN, "c-mktemp", "mktemp is racy; use mkstemp");
        else if (w == "system") add(c, s0, i, SEV_INFO, "c-system", "system() runs a shell; check that no part of the command comes from user input");
        else if (w == "if" || w == "while") {   /* assignment inside the condition */
            int depth = 0;
            for (size_t k = q; k < n; k++) {
                char ch = code[k];
                if (ch == '(') depth++;
                else if (ch == ')') { if (--depth == 0) break; }
                else if (ch == '=' && depth == 1) {
                    char pv = code[k - 1], nx = k + 1 < n ? code[k + 1] : ' ';
                    bool decl = false;   /* "if (T *p = f())" declares a variable: the name is preceded by a type */
                    {
                        size_t b = k;
                        while (b > 0 && code[b - 1] == ' ') b--;
                        size_t nameend = b;
                        while (b > 0 && idc(code[b - 1])) b--;
                        if (b < nameend) {
                            size_t t2 = b;
                            while (t2 > 0 && (code[t2 - 1] == ' ' || code[t2 - 1] == '*' || code[t2 - 1] == '&')) t2--;
                            if (t2 < b && t2 > 0 && (idc(code[t2 - 1]) || code[t2 - 1] == '>')) decl = true;
                        }
                    }
                    if (!decl && nx != '=' && !strchr("=!<>+-*/%&|^", pv)) { add(c, k, k + 1, SEV_WARN, "c-assign-in-condition", "Assignment inside a condition: did you mean '=='? (wrap it in a second pair of parentheses if it is intended)"); break; }
                }
            }
        }
    }
}

void check_js(Ctx &c, sv code) {
    sv t = trim(code);
    if (t.empty()) return;
    size_t lead = code.find_first_not_of(" \t");
    for (size_t p = 0; (p = code.find("==", p)) != sv::npos; p += 2) {
        bool triple = p + 2 < code.size() && code[p + 2] == '=';
        bool before = p > 0 && (code[p - 1] == '=' || code[p - 1] == '!' || code[p - 1] == '<' || code[p - 1] == '>');
        if (triple) { p++; continue; }
        if (before && code[p - 1] != '!') continue;
        size_t s = before ? p - 1 : p;
        add(c, s, p + 2, SEV_HINT, "js-loose-equal", "Use === / !== : == converts types before comparing");
        break;
    }
    for (size_t p = 0; (p = code.find("!=", p)) != sv::npos; p += 2) {
        if (p + 2 < code.size() && code[p + 2] == '=') { p++; continue; }
        add(c, p, p + 2, SEV_HINT, "js-loose-equal", "Use !== : != converts types before comparing");
        break;
    }
    if (starts(t, "var ")) add(c, lead, lead + 3, SEV_HINT, "js-var", "Use let or const: var is function-scoped and hoisted");
    if (t == "debugger;" || t == "debugger") add(c, lead, lead + 8, SEV_WARN, "js-debugger", "Leftover debugger statement");
    each_call(code, "eval", [&](size_t p) { add(c, p, p + 4, SEV_WARN, "js-eval", "eval runs arbitrary code"); });
}

void check_docker(Ctx &c, sv line, std::set<std::string> &stages) {
    sv t = trim(line);
    if (t.empty() || t[0] == '#') return;
    size_t lead = line.find_first_not_of(" \t");
    std::string up;
    for (size_t i = 0; i < t.size() && !isspace((unsigned char)t[i]); i++) up += (char)toupper((unsigned char)t[i]);
    sv rest = trim(t.substr(up.size()));
    if (up == "FROM") {
        size_t as = rest.find(" AS ");
        if (as == sv::npos) as = rest.find(" as ");
        if (as != sv::npos) stages.insert(std::string(trim(rest.substr(as + 4))));
        sv img = word_at(rest, 0);
        size_t sp = rest.find(' ');
        img = rest.substr(0, sp == sv::npos ? rest.size() : sp);
        if (img != "scratch" && !stages.count(std::string(img)) && img.find('$') == sv::npos) {
            size_t colon = img.rfind(':');
            size_t slash = img.rfind('/');
            bool tagged = colon != sv::npos && (slash == sv::npos || colon > slash);
            if (img.find('@') != sv::npos) {}
            else if (!tagged) add(c, lead + 5, lead + 5 + img.size(), SEV_WARN, "docker-untagged", "Pin the base image to a version tag: untagged means ':latest'");
            else if (img.substr(colon + 1) == "latest") add(c, lead + 5, lead + 5 + img.size(), SEV_WARN, "docker-latest", "':latest' changes under you; pin a version");
        }
    }
    if (up == "RUN") {
        if (rest.find("apt-get install") != sv::npos && rest.find(" -y") == sv::npos && rest.find("--yes") == sv::npos && rest.find("-qq") == sv::npos)
            add(c, lead, lead + t.size(), SEV_ERROR, "docker-apt-yes", "apt-get install without -y waits for a prompt and the build fails");
        else if (rest.find("apt-get install") != sv::npos && rest.find("--no-install-recommends") == sv::npos)
            add(c, lead, lead + t.size(), SEV_HINT, "docker-apt-recommends", "Add --no-install-recommends to keep the image small");
        if ((rest.find("curl ") != sv::npos || rest.find("wget ") != sv::npos) && (rest.find("| sh") != sv::npos || rest.find("| bash") != sv::npos)) add(c, lead, lead + t.size(), SEV_WARN, "pipe-to-shell", "Piping a download straight into a shell runs unreviewed code");
        if (starts(rest, "cd ")) add(c, lead, lead + t.size(), SEV_HINT, "docker-cd", "Use WORKDIR instead of RUN cd");
        if (rest.find("sudo ") != sv::npos) add(c, lead, lead + t.size(), SEV_WARN, "docker-sudo", "sudo is not needed in a Dockerfile (RUN is already root unless USER is set)");
    }
    if (up == "MAINTAINER") add(c, lead, lead + 10, SEV_WARN, "docker-maintainer", "MAINTAINER is deprecated; use LABEL maintainer=...");
    if (up == "ADD" && rest.find("http") != 0 && rest.find(".tar") == sv::npos && rest.find(".tgz") == sv::npos) add(c, lead, lead + 3, SEV_HINT, "docker-add", "Use COPY unless you need ADD's download or extract behaviour");
}

}  // namespace

bool lint_is_ansible(const std::string &path, const char *text, size_t n) {
    std::string rooted = "/" + path;
    for (const char *seg : {"/tasks/", "/handlers/", "/roles/", "/playbooks/", "/group_vars/", "/host_vars/", "/defaults/", "/meta/"})
        if (rooted.find(seg) != std::string::npos) return true;
    size_t sl = path.rfind('/');
    std::string base = sl == std::string::npos ? path : path.substr(sl + 1);
    if (starts(base, "playbook") || base == "site.yml" || base == "site.yaml" || starts(base, "requirements")) return starts(base, "requirements") ? false : true;
    sv head(text, std::min<size_t>(n, 6000));
    bool hosts = head.find("hosts:") != sv::npos;
    if (hosts && (head.find("tasks:") != sv::npos || head.find("roles:") != sv::npos || head.find("become:") != sv::npos)) return true;
    if (head.find("ansible.builtin.") != sv::npos) return true;
    return false;
}

void lint_run(const LintInput &in, std::vector<Diag> &out, size_t max) {
    out.clear();
    if (!in.text || !in.n) return;
    if (memchr(in.text, 0, std::min<size_t>(in.n, 8192))) return;   /* binary */
    Ctx c{out, max};
    sv text(in.text, in.n);
    const char *lname = in.lang ? lang_name(in.lang) : "";
    std::string ln_(lname);
    std::string base = in.path.substr(in.path.rfind('/') == std::string::npos ? 0 : in.path.rfind('/') + 1);
    bool yaml = ln_ == "YAML";
    bool json = ln_ == "JSON" && ends(in.path, ".json");
    bool shell = ln_ == "Shell" || starts(text, "#!/bin/sh") || starts(text, "#!/bin/bash") || starts(text, "#!/usr/bin/env bash") || starts(text, "#!/usr/bin/env sh");
    bool sh_posix = starts(text, "#!/bin/sh") || starts(text, "#!/usr/bin/env sh");
    bool py = ln_ == "Python";
    bool cfam = ln_ == "C" || ln_ == "C++";
    bool js = ln_ == "JavaScript" || ln_ == "TypeScript";
    bool docker = ln_ == "Dockerfile";
    bool make = ln_ == "Makefile";
    bool md = ln_ == "Markdown";
    bool brackets = cfam || js || ln_ == "Go" || ln_ == "Rust" || ln_ == "Java" || ln_ == "C#" || ln_ == "JSON" || ln_ == "PHP" || ln_ == "Kotlin" || ln_ == "Swift" || ln_ == "CSS" || py;
    bool sql = ln_ == "SQL", css = ln_ == "CSS", toml = ln_ == "TOML", xml = ln_ == "XML";
    bool helm = yaml && ("/" + in.path).find("/templates/") != std::string::npos && text.find("{{") != sv::npos;   /* a Helm template is Go template text, not YAML */
    bool ansible = yaml && !helm && lint_is_ansible(in.path, in.text, in.n);
    bool k8s = yaml && !helm && !ansible && k8s_detect(text, in.path);
    bool tasks_file = ansible && (("/" + in.path).find("/tasks/") != std::string::npos || ("/" + in.path).find("/handlers/") != std::string::npos);
    bool minified = false;
    {   // a bundled or minified file: very long lines, regex literals and template strings the lexer cannot follow
        size_t p = 0;
        while (p < in.n) { const char *nl = (const char *)memchr(in.text + p, '\n', in.n - p); size_t e = nl ? (size_t)(nl - in.text) : in.n; if (e - p > 2000) { minified = true; break; } p = e + 1; }
    }
    bool raw_strings = cfam && text.find("R\"") != sv::npos;   /* C++ raw strings: the lexer does not know them, so brackets inside would be miscounted */
    bool lex = (brackets || sql || ln_ == "Ruby") && (in.n < (24u << 20)) && !minified;
    if (raw_strings) brackets = false;

    if (yaml && !helm) yaml_pass(c, text, ansible, tasks_file);
    if (k8s) k8s_pass(c, text);
    if (toml) toml_check(c, text);
    if (xml && in.n < (24u << 20)) xml_check(c, text);
    if (md) markdown_check(c, text, in.path);
    if (json) json_check(c, in.text, in.n);

    struct Br { char ch; uint32_t line, col; };
    std::vector<Br> stack;
    std::vector<uint8_t> tok;
    uint8_t lstate = 0;
    std::string code;
    std::set<std::string> stages;
    SqlState sqlst;
    CssState cssst;
    uint32_t tab_lines = 0, space_lines = 0;
    std::vector<std::pair<uint32_t, uint32_t>> tab_at, space_at;   /* line, indent width, for the minority style */
    bool utf8_reported = false;
    int crlf_lines = 0, lf_lines = 0;
    uint32_t first_crlf = 0, first_lf = 0;
    bool saw_set_e = false, saw_set_u = false;
    int shell_lines = 0;
    bool make_rule = false, make_recipe = false;
    uint32_t ln = 0;
    size_t pos = 0;
    while (pos <= text.size() && !c.full()) {
        size_t e = text.find('\n', pos);
        bool last = e == sv::npos;
        if (last) e = text.size();
        sv line = text.substr(pos, e - pos);
        bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.remove_suffix(1);
        c.line = ln;
        c.cur = line;
        // generic
        if (!md && !line.empty()) {
            size_t te = line.size();
            while (te > 0 && (line[te - 1] == ' ' || line[te - 1] == '\t')) te--;
            if (te < line.size() && te > 0) { Diag &d = add(c, te, line.size(), SEV_HINT, "trailing-whitespace", "Trailing whitespace"); if (!d.code.empty()) fixit(d, te, line.size(), ""); }
        }
        if (line.size() >= 7 && (starts(line, "<<<<<<< ") || starts(line, ">>>>>>> ") || line == "=======")) add(c, 0, std::min<size_t>(line.size(), 7), SEV_ERROR, "merge-conflict", "Merge conflict marker");
        if (line.size() >= 12) check_secrets(c, line);
        if (ansible && false) {}
        if (cr && ln == 0) add(c, line.size(), line.size() + 1, SEV_HINT, "crlf", "This file uses Windows (CRLF) line endings");
        if (!last) { if (cr) { if (!crlf_lines) first_crlf = ln; crlf_lines++; } else { if (!lf_lines) first_lf = ln; lf_lines++; } }
        if (!line.empty() && !md && !make && ln_ != "Go" && !yaml) {   /* tabs and spaces both used to indent */
            if (line[0] == '\t') { tab_lines++; if (tab_at.size() < 40) tab_at.push_back({ln, 1}); }
            else if (line.size() > 1 && line[0] == ' ' && line[1] == ' ') { size_t k = 0; while (k < line.size() && line[k] == ' ') k++; if (k < line.size()) { space_lines++; if (space_at.size() < 40) space_at.push_back({ln, (uint32_t)k}); } }
        }
        if (!utf8_reported && !line.empty()) {   /* only lines with a byte >= 0x80 are looked at */
            bool hi = false;
            for (unsigned char ch : line) if (ch >= 0x80) { hi = true; break; }
            if (hi) {
                size_t i2 = 0;
                while (i2 < line.size()) {
                    unsigned char b = (unsigned char)line[i2];
                    size_t need = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : (size_t)-1;
                    if (need == (size_t)-1) { add(c, i2, i2 + 1, SEV_WARN, "invalid-utf8", "This is not valid UTF-8 (the file may be in another encoding)"); utf8_reported = true; break; }
                    bool ok = true;
                    for (size_t k = 1; k <= need; k++) if ((i2 + k >= line.size()) || (((unsigned char)line[i2 + k]) & 0xC0) != 0x80) ok = false;
                    if (!ok) { add(c, i2, i2 + 1, SEV_WARN, "invalid-utf8", "This is not valid UTF-8 (the file may be in another encoding)"); utf8_reported = true; break; }
                    i2 += need + 1;
                }
            }
        }
        // code view with strings and comments blanked, from the lexer
        if (lex || (shell && false)) {
            if (tok.size() < line.size() + 1) tok.resize(line.size() + 64);
            lstate = lex_line(in.lang, line.data(), line.size(), lstate, tok.data());
            code.assign(line.data(), line.size());
            for (size_t i = 0; i < line.size(); i++) {
                uint8_t k = tok[i];
                if (k == T_COMMENT || k == T_STRING) code[i] = ' ';
            }
            if (sql) sql_line(c, code, sqlst);
            if (css) css_line(c, code, cssst);
            else if (!sql && !py && !cfam && !js) misc_line(c, lname, code);
            if (brackets) {
                for (size_t i = 0; i < code.size(); i++) {
                    char ch = code[i];
                    if (ch == '(' || ch == '[' || ch == '{') stack.push_back({ch, ln, (uint32_t)i});
                    else if (ch == ')' || ch == ']' || ch == '}') {
                        char want = ch == ')' ? '(' : ch == ']' ? '[' : '{';
                        if (!stack.empty() && stack.back().ch == want) stack.pop_back();
                        else if (!c.full()) add(c, i, i + 1, SEV_ERROR, "bracket", std::string("Unmatched '") + ch + "'");
                    }
                }
            }
            if (py) check_python(c, code, line);
            else if (cfam) check_c(c, code, ends(in.path, ".h") || ends(in.path, ".hpp"));
            else if (js) check_js(c, code);
        }
        if (shell) {
            sv t = trim(line);
            if (starts(t, "set -") ) { sv opts = word_at(t, 5); if (opts.find('e') != sv::npos) saw_set_e = true; if (opts.find('u') != sv::npos) saw_set_u = true; if (t.find("errexit") != sv::npos) saw_set_e = true; if (t.find("nounset") != sv::npos) saw_set_u = true; }
            check_shell(c, line, sh_posix, saw_set_e, saw_set_u);
            if (!t.empty() && t[0] != '#') shell_lines++;
        }
        if (docker) check_docker(c, line, stages);
        if (make) {
            bool recipe_tab = !line.empty() && line[0] == '\t';
            if (recipe_tab) make_recipe = true;
            else if (!line.empty() && line[0] != '#') {
                size_t colon = line.find(':');
                bool rule = line[0] != ' ' && colon != sv::npos && colon + 1 < line.size() + 1 && !(colon + 1 < line.size() && line[colon + 1] == '=') && line.find('=') > colon;
                if (rule || (line[0] != ' ' && colon != sv::npos && colon + 1 < line.size() && line[colon + 1] == ':' && line.find('=') == sv::npos)) { make_rule = true; make_recipe = false; }
                else if (line[0] == ' ' && (make_rule || make_recipe) && trim(line).size() && trim(line)[0] != '#' && !starts(trim(line), "ifeq") && !starts(trim(line), "ifneq") && !starts(trim(line), "else") && !starts(trim(line), "endif") && !starts(trim(line), "ifdef") && !starts(trim(line), "ifndef")) {
                    Diag &d = add(c, 0, line.find_first_not_of(' '), SEV_ERROR, "make-spaces", "A recipe line must start with a tab, not spaces ('missing separator')");
                    if (!d.code.empty()) fixit(d, 0, line.find_first_not_of(' '), "\t");
                } else if (line[0] != ' ') { make_rule = false; make_recipe = false; }
            }
        }
        if (yaml && !ansible && line.find("{{") != sv::npos) {}   /* plain YAML: no Jinja checks */
        ln++;
        pos = e + 1;
        if (last) break;
    }
    if (!text.empty() && text.back() != '\n' && !c.full()) {
        c.line = ln ? ln - 1 : 0;
        c.cur = sv();
        size_t ll = text.rfind('\n');
        size_t len = text.size() - (ll == sv::npos ? 0 : ll + 1);
        add(c, len, len + 1, SEV_HINT, "no-final-newline", "No newline at end of file");
    }
    if (shell && !saw_set_e && shell_lines >= 5 && !c.full()) {
        c.line = 0; c.cur = sv();
        add(c, 0, 1, SEV_HINT, "shell-no-set-e", "No 'set -e' (or 'set -euo pipefail'): the script keeps going after a failing command");
    }
    if (tab_lines > 0 && space_lines > 0 && !c.full() && !minified && !raw_strings && std::min(tab_lines, space_lines) * 4 <= std::max(tab_lines, space_lines)) {   /* point at the lines in the minority style */
        bool tabs_minor = tab_lines < space_lines;
        const auto &minor = tabs_minor ? tab_at : space_at;
        c.cur = sv();
        for (auto &m : minor) { if (c.full()) break; c.line = m.first; add(c, 0, m.second, SEV_HINT, "mixed-indent", tabs_minor ? "This line is indented with a tab; the rest of the file uses spaces" : "This line is indented with spaces; the rest of the file uses tabs"); }
    }
    if (crlf_lines > 0 && lf_lines > 0 && !c.full()) {
        bool crlf_minor = crlf_lines < lf_lines;
        c.line = crlf_minor ? first_crlf : first_lf; c.cur = sv();
        add(c, 0, 1, SEV_HINT, "mixed-eol", crlf_minor ? "This line ends with CRLF; the rest of the file uses LF" : "This line ends with LF; the rest of the file uses CRLF");
    }
    if (!stack.empty() && lex) {
        size_t shown = 0;
        for (size_t i = stack.size(); i-- > 0 && shown < 5; shown++) {
            c.line = stack[i].line; c.cur = sv();
            add(c, stack[i].col, stack[i].col + 1, SEV_ERROR, "bracket", std::string("Unclosed '") + stack[i].ch + "'");
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Diag &a, const Diag &b) { return a.line != b.line ? a.line < b.line : a.c0 < b.c0; });
}

const std::vector<AnsibleTemplate> &ansible_templates() {
    static const std::vector<AnsibleTemplate> t = {
        {"apt: install packages", "- name: Install packages\n  ansible.builtin.apt:\n    name:\n      - nginx\n    state: present\n    update_cache: true\n"},
        {"dnf: install packages", "- name: Install packages\n  ansible.builtin.dnf:\n    name:\n      - nginx\n    state: present\n"},
        {"package: install (any distro)", "- name: Install package\n  ansible.builtin.package:\n    name: nginx\n    state: present\n"},
        {"copy: file with owner and mode", "- name: Copy file\n  ansible.builtin.copy:\n    src: files/example.conf\n    dest: /etc/example.conf\n    owner: root\n    group: root\n    mode: \"0644\"\n"},
        {"template: render a Jinja2 template", "- name: Render template\n  ansible.builtin.template:\n    src: example.conf.j2\n    dest: /etc/example.conf\n    owner: root\n    group: root\n    mode: \"0644\"\n  notify: Restart example\n"},
        {"file: directory", "- name: Create directory\n  ansible.builtin.file:\n    path: /opt/example\n    state: directory\n    owner: root\n    group: root\n    mode: \"0755\"\n"},
        {"lineinfile: set one line", "- name: Set a line\n  ansible.builtin.lineinfile:\n    path: /etc/example.conf\n    regexp: '^key='\n    line: key=value\n    mode: \"0644\"\n"},
        {"service: start and enable", "- name: Start and enable service\n  ansible.builtin.service:\n    name: nginx\n    state: started\n    enabled: true\n"},
        {"systemd: reload and enable", "- name: Enable unit\n  ansible.builtin.systemd:\n    name: example.service\n    state: restarted\n    enabled: true\n    daemon_reload: true\n"},
        {"user: create a user", "- name: Create user\n  ansible.builtin.user:\n    name: deploy\n    shell: /bin/bash\n    groups: sudo\n    append: true\n    create_home: true\n"},
        {"git: clone a repository", "- name: Clone repository\n  ansible.builtin.git:\n    repo: https://example.com/org/repo.git\n    dest: /opt/repo\n    version: main\n"},
        {"get_url: download a file", "- name: Download file\n  ansible.builtin.get_url:\n    url: https://example.com/file.tar.gz\n    dest: /tmp/file.tar.gz\n    mode: \"0644\"\n    checksum: sha256:CHANGEME\n"},
        {"unarchive: extract", "- name: Extract archive\n  ansible.builtin.unarchive:\n    src: /tmp/file.tar.gz\n    dest: /opt/example\n    remote_src: true\n"},
        {"command: with changed_when", "- name: Run a command\n  ansible.builtin.command: /usr/bin/example --check\n  register: example_result\n  changed_when: false\n"},
        {"shell: with pipefail", "- name: Run a pipeline\n  ansible.builtin.shell: |\n    set -o pipefail\n    ps aux | grep example\n  args:\n    executable: /bin/bash\n  register: pipeline_result\n  changed_when: false\n"},
        {"debug: print a variable", "- name: Show variable\n  ansible.builtin.debug:\n    var: example_result.stdout_lines\n"},
        {"set_fact", "- name: Set a fact\n  ansible.builtin.set_fact:\n    example_fact: \"{{ some_var | default('value') }}\"\n"},
        {"assert: check conditions", "- name: Check requirements\n  ansible.builtin.assert:\n    that:\n      - ansible_facts['distribution'] == 'Debian'\n    fail_msg: This playbook supports Debian only\n"},
        {"uri: HTTP request", "- name: Call API\n  ansible.builtin.uri:\n    url: https://example.com/health\n    method: GET\n    status_code: 200\n  register: health\n  until: health.status == 200\n  retries: 5\n  delay: 3\n"},
        {"wait_for: port", "- name: Wait for the port\n  ansible.builtin.wait_for:\n    host: 127.0.0.1\n    port: 8080\n    timeout: 60\n"},
        {"cron: scheduled job", "- name: Schedule job\n  ansible.builtin.cron:\n    name: example job\n    minute: \"0\"\n    hour: \"3\"\n    job: /usr/local/bin/example\n"},
        {"include_tasks (with loop)", "- name: Include tasks per item\n  ansible.builtin.include_tasks: item.yml\n  loop: \"{{ items_list }}\"\n  loop_control:\n    loop_var: item_name\n"},
        {"block / rescue / always", "- name: Try something\n  block:\n    - name: Do the work\n      ansible.builtin.command: /bin/true\n      changed_when: false\n  rescue:\n    - name: Handle the failure\n      ansible.builtin.debug:\n        msg: It failed\n  always:\n    - name: Clean up\n      ansible.builtin.debug:\n        msg: Done\n"},
        {"handler", "- name: Restart example\n  ansible.builtin.service:\n    name: example\n    state: restarted\n"},
        {"ufw: allow a port", "- name: Allow SSH\n  community.general.ufw:\n    rule: allow\n    port: \"22\"\n    proto: tcp\n"},
        {"sysctl: kernel setting", "- name: Set a kernel parameter\n  ansible.posix.sysctl:\n    name: net.ipv4.ip_forward\n    value: \"1\"\n    sysctl_set: true\n    state: present\n    reload: true\n"},
        {"docker_container", "- name: Run container\n  community.docker.docker_container:\n    name: example\n    image: example/app:1.0.0\n    state: started\n    restart_policy: unless-stopped\n    published_ports:\n      - \"8080:80\"\n"},
        {"play: skeleton", "- name: Configure servers\n  hosts: all\n  become: true\n  gather_facts: true\n  tasks:\n    - name: Ping\n      ansible.builtin.ping:\n"},
    };
    return t;
}

const std::vector<AnsibleTemplate> &k8s_templates() {
    static const std::vector<AnsibleTemplate> t = {
        {"Deployment", "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: app\n  labels:\n    app: app\nspec:\n  replicas: 2\n  selector:\n    matchLabels:\n      app: app\n  template:\n    metadata:\n      labels:\n        app: app\n    spec:\n      securityContext:\n        runAsNonRoot: true\n      containers:\n        - name: app\n          image: registry.example.com/app:1.0.0\n          ports:\n            - containerPort: 8080\n          resources:\n            requests:\n              cpu: 100m\n              memory: 128Mi\n            limits:\n              memory: 256Mi\n          readinessProbe:\n            httpGet:\n              path: /healthz\n              port: 8080\n          livenessProbe:\n            httpGet:\n              path: /healthz\n              port: 8080\n          securityContext:\n            allowPrivilegeEscalation: false\n            readOnlyRootFilesystem: true\n"},
        {"Service (ClusterIP)", "apiVersion: v1\nkind: Service\nmetadata:\n  name: app\nspec:\n  selector:\n    app: app\n  ports:\n    - port: 80\n      targetPort: 8080\n"},
        {"Ingress", "apiVersion: networking.k8s.io/v1\nkind: Ingress\nmetadata:\n  name: app\nspec:\n  ingressClassName: nginx\n  rules:\n    - host: app.example.com\n      http:\n        paths:\n          - path: /\n            pathType: Prefix\n            backend:\n              service:\n                name: app\n                port:\n                  number: 80\n  tls:\n    - hosts:\n        - app.example.com\n      secretName: app-tls\n"},
        {"ConfigMap", "apiVersion: v1\nkind: ConfigMap\nmetadata:\n  name: app-config\ndata:\n  LOG_LEVEL: info\n  app.properties: |\n    key=value\n"},
        {"Secret (stringData)", "apiVersion: v1\nkind: Secret\nmetadata:\n  name: app-secret\ntype: Opaque\nstringData:\n  password: CHANGEME   # do not commit real values\n"},
        {"StatefulSet", "apiVersion: apps/v1\nkind: StatefulSet\nmetadata:\n  name: db\nspec:\n  serviceName: db\n  replicas: 1\n  selector:\n    matchLabels:\n      app: db\n  template:\n    metadata:\n      labels:\n        app: db\n    spec:\n      containers:\n        - name: db\n          image: postgres:16.4\n          resources:\n            requests:\n              cpu: 100m\n              memory: 128Mi\n            limits:\n              memory: 256Mi\n          ports:\n            - containerPort: 5432\n          volumeMounts:\n            - name: data\n              mountPath: /var/lib/postgresql/data\n  volumeClaimTemplates:\n    - metadata:\n        name: data\n      spec:\n        accessModes: [\"ReadWriteOnce\"]\n        resources:\n          requests:\n            storage: 10Gi\n"},
        {"DaemonSet", "apiVersion: apps/v1\nkind: DaemonSet\nmetadata:\n  name: agent\nspec:\n  selector:\n    matchLabels:\n      app: agent\n  template:\n    metadata:\n      labels:\n        app: agent\n    spec:\n      containers:\n        - name: agent\n          image: registry.example.com/agent:1.0.0\n          resources:\n            requests:\n              cpu: 100m\n              memory: 128Mi\n            limits:\n              memory: 256Mi\n"},
        {"Job", "apiVersion: batch/v1\nkind: Job\nmetadata:\n  name: task\nspec:\n  backoffLimit: 3\n  template:\n    spec:\n      restartPolicy: Never\n      containers:\n        - name: task\n          image: busybox:1.36\n          command: [\"sh\", \"-c\", \"echo done\"]\n          resources:\n            requests:\n              cpu: 100m\n              memory: 128Mi\n            limits:\n              memory: 256Mi\n"},
        {"CronJob", "apiVersion: batch/v1\nkind: CronJob\nmetadata:\n  name: nightly\nspec:\n  schedule: \"0 3 * * *\"\n  concurrencyPolicy: Forbid\n  jobTemplate:\n    spec:\n      template:\n        spec:\n          restartPolicy: OnFailure\n          containers:\n            - name: nightly\n              image: busybox:1.36\n              command: [\"sh\", \"-c\", \"echo run\"]\n              resources:\n                requests:\n                  cpu: 100m\n                  memory: 64Mi\n                limits:\n                  memory: 128Mi\n"},
        {"PersistentVolumeClaim", "apiVersion: v1\nkind: PersistentVolumeClaim\nmetadata:\n  name: data\nspec:\n  accessModes:\n    - ReadWriteOnce\n  resources:\n    requests:\n      storage: 5Gi\n"},
        {"HorizontalPodAutoscaler", "apiVersion: autoscaling/v2\nkind: HorizontalPodAutoscaler\nmetadata:\n  name: app\nspec:\n  scaleTargetRef:\n    apiVersion: apps/v1\n    kind: Deployment\n    name: app\n  minReplicas: 2\n  maxReplicas: 10\n  metrics:\n    - type: Resource\n      resource:\n        name: cpu\n        target:\n          type: Utilization\n          averageUtilization: 70\n"},
        {"NetworkPolicy (deny all ingress)", "apiVersion: networking.k8s.io/v1\nkind: NetworkPolicy\nmetadata:\n  name: default-deny-ingress\nspec:\n  podSelector: {}\n  policyTypes:\n    - Ingress\n"},
        {"Namespace", "apiVersion: v1\nkind: Namespace\nmetadata:\n  name: team-a\n"},
        {"ServiceAccount + Role + RoleBinding", "apiVersion: v1\nkind: ServiceAccount\nmetadata:\n  name: app\n---\napiVersion: rbac.authorization.k8s.io/v1\nkind: Role\nmetadata:\n  name: app\nrules:\n  - apiGroups: [\"\"]\n    resources: [\"configmaps\"]\n    verbs: [\"get\", \"list\"]\n---\napiVersion: rbac.authorization.k8s.io/v1\nkind: RoleBinding\nmetadata:\n  name: app\nsubjects:\n  - kind: ServiceAccount\n    name: app\nroleRef:\n  apiGroup: rbac.authorization.k8s.io\n  kind: Role\n  name: app\n"},
        {"PodDisruptionBudget", "apiVersion: policy/v1\nkind: PodDisruptionBudget\nmetadata:\n  name: app\nspec:\n  minAvailable: 1\n  selector:\n    matchLabels:\n      app: app\n"},
        {"Pod (debug)", "apiVersion: v1\nkind: Pod\nmetadata:\n  name: debug\nspec:\n  restartPolicy: Never\n  containers:\n    - name: debug\n      image: busybox:1.36\n      command: [\"sleep\", \"3600\"]\n      resources:\n        requests:\n          cpu: 100m\n          memory: 64Mi\n        limits:\n          memory: 128Mi\n"},
    };
    return t;
}
