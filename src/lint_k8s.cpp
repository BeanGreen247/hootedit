/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <map>
#include <set>

#include "lint_priv.h"

/* Kubernetes manifests: a small YAML tree per document, then the checks that catch what `kubectl apply` or a cluster would complain about
 * later (removed API versions, selectors that do not match the pod labels, numbers where labels need strings) and what a reviewer would
 * flag (no limits, `:latest`, privileged containers, secrets in the file). */

namespace lintp {
namespace {

struct YN {
    int parent = -1;
    std::vector<int> kids;
    std::string key;
    sv value, text;
    uint32_t line = 0;
    size_t k0 = 0, k1 = 0, v0 = 0, col = 0;
    bool item = false, has_key = false;
};

struct Tree {
    std::vector<YN> n;
    Tree() { n.emplace_back(); n[0].col = (size_t)-1; }
};

int child(const Tree &t, int node, const char *key) {
    if (node < 0) return -1;
    for (int k : t.n[(size_t)node].kids) if (t.n[(size_t)k].has_key && t.n[(size_t)k].key == key) return k;
    return -1;
}
int path(const Tree &t, int node, std::initializer_list<const char *> keys) {
    for (const char *k : keys) { node = child(t, node, k); if (node < 0) return -1; }
    return node;
}
std::vector<int> items_of(const Tree &t, int node) {
    std::vector<int> out;
    if (node < 0) return out;
    for (int k : t.n[(size_t)node].kids) if (t.n[(size_t)k].item) out.push_back(k);
    return out;
}
std::string unq(sv v) {
    v = trim(v);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return std::string(v);
}
bool quoted(sv v) { v = trim(v); return !v.empty() && (v.front() == '"' || v.front() == '\''); }

struct DocTree { Tree t; uint32_t first_line = 0; };

void build_docs(sv text, std::vector<DocTree> &docs) {
    docs.emplace_back();
    std::vector<int> stack = {0};
    size_t block_col = (size_t)-1;
    uint32_t ln = 0;
    size_t pos = 0;
    YLine y;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == sv::npos) e = text.size();
        sv line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (starts(line, "---") && (line.size() == 3 || line[3] == ' ' || line[3] == '\t')) {
            if (!docs.back().t.n[0].kids.empty()) { docs.emplace_back(); stack = {0}; }
            docs.back().first_line = ln + 1;
            block_col = (size_t)-1;
        } else {
            parse_yline(line, y);
            bool skip = false;
            if (block_col != (size_t)-1) {
                if (line.find_first_not_of(" \t") == sv::npos || y.indent > block_col) skip = true;
                else block_col = (size_t)-1;
            }
            if (!skip && !y.blank) {
                Tree &t = docs.back().t;
                size_t c = y.dashes > 0 ? y.indent : y.kp;
                while (stack.size() > 1) {
                    const YN &top = t.n[(size_t)stack.back()];
                    if (top.col > c || (top.col == c && !(y.dashes > 0 && top.has_key && !top.item))) stack.pop_back(); else break;
                }
                auto mk = [&](bool item, bool has_key) {
                    YN nd;
                    nd.parent = stack.back(); nd.item = item; nd.has_key = has_key; nd.line = ln; nd.text = line;
                    t.n.push_back(nd);
                    int idx = (int)t.n.size() - 1;
                    t.n[(size_t)nd.parent].kids.push_back(idx);
                    return idx;
                };
                if (y.dashes > 0) {
                    int it = mk(true, false);
                    t.n[(size_t)it].col = y.indent;
                    stack.push_back(it);
                    if (y.is_key) {
                        int k = mk(false, true);
                        YN &kn = t.n[(size_t)k];
                        kn.key = std::string(y.key); kn.value = y.value; kn.k0 = y.k0; kn.k1 = y.k1; kn.v0 = y.v0; kn.col = y.kp;
                        stack.push_back(k);
                        if (!y.value.empty() && (y.value[0] == '|' || y.value[0] == '>')) block_col = y.kp;
                    } else {
                        sv rest = trim(line.substr(std::min(y.kp, line.size())));
                        size_t hash = rest.find(" #");
                        if (hash != sv::npos) rest = trim(rest.substr(0, hash));
                        t.n[(size_t)it].value = rest;
                        t.n[(size_t)it].v0 = y.kp;
                    }
                } else if (y.is_key) {
                    int k = mk(false, true);
                    YN &kn = t.n[(size_t)k];
                    kn.key = std::string(y.key); kn.value = y.value; kn.k0 = y.k0; kn.k1 = y.k1; kn.v0 = y.v0; kn.col = y.kp;
                    stack.push_back(k);
                    if (!y.value.empty() && (y.value[0] == '|' || y.value[0] == '>')) block_col = y.kp;
                }
            }
        }
        ln++;
        pos = e + 1;
        if (e >= text.size()) break;
    }
}

const std::map<std::string, std::string> &kind_api() {
    static const std::map<std::string, std::string> m = {
        {"Deployment", "apps/v1"}, {"StatefulSet", "apps/v1"}, {"DaemonSet", "apps/v1"}, {"ReplicaSet", "apps/v1"}, {"Service", "v1"}, {"Pod", "v1"}, {"ConfigMap", "v1"},
        {"Secret", "v1"}, {"Namespace", "v1"}, {"PersistentVolume", "v1"}, {"PersistentVolumeClaim", "v1"}, {"ServiceAccount", "v1"}, {"Endpoints", "v1"}, {"LimitRange", "v1"},
        {"ResourceQuota", "v1"}, {"Ingress", "networking.k8s.io/v1"}, {"NetworkPolicy", "networking.k8s.io/v1"}, {"IngressClass", "networking.k8s.io/v1"}, {"Job", "batch/v1"},
        {"CronJob", "batch/v1"}, {"HorizontalPodAutoscaler", "autoscaling/v2"}, {"Role", "rbac.authorization.k8s.io/v1"}, {"ClusterRole", "rbac.authorization.k8s.io/v1"},
        {"RoleBinding", "rbac.authorization.k8s.io/v1"}, {"ClusterRoleBinding", "rbac.authorization.k8s.io/v1"}, {"StorageClass", "storage.k8s.io/v1"}, {"PodDisruptionBudget", "policy/v1"}};
    return m;
}

bool core_group(sv api) {
    static const char *g[] = {"extensions/", "apps/", "batch/", "policy/", "networking.k8s.io/", "rbac.authorization.k8s.io/", "autoscaling/", "admissionregistration.k8s.io/", "apiextensions.k8s.io/",
                              "storage.k8s.io/", "scheduling.k8s.io/", "certificates.k8s.io/", "coordination.k8s.io/", "discovery.k8s.io/", "events.k8s.io/", "flowcontrol.apiserver.k8s.io/", "node.k8s.io/"};
    for (const char *x : g) if (starts(api, x)) return true;
    return false;
}

bool looks_scalar_non_string(sv v) {
    v = trim(v);
    if (v.empty() || quoted(v)) return false;
    static const char *w[] = {"true", "false", "yes", "no", "on", "off", "null", "~", "True", "False", "TRUE", "FALSE", "Yes", "No", "YES", "NO", "On", "Off", "Null", "NULL"};
    for (const char *x : w) if (v == x) return true;
    size_t i = 0;
    if (v[0] == '-' || v[0] == '+') i = 1;
    bool digit = false, dot = false;
    for (; i < v.size(); i++) {
        if (isdigit((unsigned char)v[i])) digit = true;
        else if (v[i] == '.' && !dot) dot = true;
        else return false;
    }
    return digit;
}

void at(Ctx &c, const YN &n, size_t c0, size_t c1, int sev, const char *code, const std::string &msg, Diag **dp = nullptr) {
    Ctx cc{c.out, c.max};
    cc.line = n.line;
    cc.cur = n.text;
    Diag &d = add(cc, c0, c1, sev, code, msg);
    if (dp) *dp = d.code.empty() ? nullptr : &d;
}
void at_key(Ctx &c, const YN &n, int sev, const char *code, const std::string &msg) { at(c, n, n.k0, n.k1, sev, code, msg); }
void at_val(Ctx &c, const YN &n, int sev, const char *code, const std::string &msg, Diag **dp = nullptr) { at(c, n, n.v0, n.v0 + std::max<size_t>(1, n.value.size()), sev, code, msg, dp); }

bool valid_quantity(sv v) {   /* 100m, 0.5, 512Mi, 1G, 2e3 ... */
    if (v.empty()) return false;
    size_t i = 0;
    while (i < v.size() && (isdigit((unsigned char)v[i]) || v[i] == '.')) i++;
    if (i == 0) return false;
    sv suf = v.substr(i);
    static const char *ok[] = {"", "m", "k", "M", "G", "T", "P", "E", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "n", "u"};
    for (const char *s : ok) if (suf == s) return true;
    if ((suf[0] == 'e' || suf[0] == 'E') && suf.size() > 1) return true;
    return false;
}

/* a map whose values must be strings: labels, annotations, node selectors */
void string_map(Ctx &c, const Tree &t, int node, const char *what) {
    if (node < 0) return;
    for (int k : t.n[(size_t)node].kids) {
        const YN &kn = t.n[(size_t)k];
        if (!kn.has_key || kn.value.empty()) continue;
        if (looks_scalar_non_string(kn.value)) {
            Diag *d = nullptr;
            at_val(c, kn, SEV_ERROR, "k8s-string-value", std::string("A ") + what + " value must be a string: write \"" + std::string(kn.value) + "\" in quotes", &d);
            if (d) { d->has_fix = true; d->f0 = (uint32_t)kn.v0; d->f1 = (uint32_t)(kn.v0 + kn.value.size()); d->fix = "\"" + std::string(kn.value) + "\""; }
        }
    }
}

void check_container(Ctx &c, const Tree &t, int ct, const std::string &kind, bool init, bool pod_nonroot, std::set<std::string> &names) {
    const YN &cn = t.n[(size_t)ct];
    int name = child(t, ct, "name"), image = child(t, ct, "image");
    const YN &anchor = name >= 0 ? t.n[(size_t)name] : cn;
    auto fail_at = [&](int sev, const char *code, const std::string &msg) {
        if (name >= 0) at_val(c, t.n[(size_t)name], sev, code, msg);
        else if (!cn.kids.empty()) at_key(c, t.n[(size_t)cn.kids[0]], sev, code, msg);
    };
    if (name < 0) fail_at(SEV_ERROR, "k8s-container-name", "A container needs a name");
    else {
        std::string nm = unq(t.n[(size_t)name].value);
        if (!names.insert(nm).second) at_val(c, t.n[(size_t)name], SEV_ERROR, "k8s-container-duplicate", "Two containers are named '" + nm + "'");
    }
    if (image < 0) fail_at(SEV_ERROR, "k8s-image-missing", "A container needs an image");
    else {
        const YN &in = t.n[(size_t)image];
        std::string img = unq(in.value);
        if (img.find('@') == std::string::npos && img.find("{{") == std::string::npos && img.find('$') == std::string::npos && !img.empty()) {
            size_t slash = img.rfind('/'), colon = img.rfind(':');
            bool tagged = colon != std::string::npos && (slash == std::string::npos || colon > slash);
            if (!tagged) at_val(c, in, SEV_WARN, "k8s-image-untagged", "The image has no tag, so it means ':latest'; pin a version");
            else if (img.substr(colon + 1) == "latest") at_val(c, in, SEV_WARN, "k8s-image-latest", "':latest' changes under you and hides which version runs; pin a version");
        }
    }
    int pull = child(t, ct, "imagePullPolicy");
    if (pull >= 0) { std::string v = unq(t.n[(size_t)pull].value); if (v != "Always" && v != "IfNotPresent" && v != "Never") at_val(c, t.n[(size_t)pull], SEV_ERROR, "k8s-pull-policy", "imagePullPolicy must be Always, IfNotPresent or Never"); }
    // resources
    int res = child(t, ct, "resources");
    if (res < 0 && !init) at(c, anchor, anchor.k0, anchor.k1 ? anchor.k1 : 1, SEV_WARN, "k8s-no-resources", "No resources: set requests and limits so the scheduler and the node can protect each other");
    else if (res >= 0) {
        int lim = child(t, res, "limits"), req = child(t, res, "requests");
        if (lim < 0 || child(t, lim, "memory") < 0) at_key(c, t.n[(size_t)res], SEV_WARN, "k8s-no-memory-limit", "No memory limit: one leak can take the whole node");
        if (req < 0) at_key(c, t.n[(size_t)res], SEV_HINT, "k8s-no-requests", "No requests: the scheduler assumes the container needs nothing");
        for (int grp : {lim, req}) {
            if (grp < 0) continue;
            for (int q : t.n[(size_t)grp].kids) {
                const YN &qn = t.n[(size_t)q];
                if (!qn.has_key || qn.value.empty()) continue;
                std::string v = unq(qn.value);
                if (qn.key == "cpu" || qn.key == "memory" || qn.key == "ephemeral-storage") {
                    if (v.find("{{") != std::string::npos || v.find('$') != std::string::npos) continue;
                    if (!valid_quantity(v)) at_val(c, qn, SEV_ERROR, "k8s-quantity", "'" + v + "' is not a valid quantity (units are m, k, M, G, Ki, Mi, Gi; note the capital letters)");
                }
            }
        }
    }
    // probes (long-running workloads only)
    if (!init && (kind == "Deployment" || kind == "StatefulSet" || kind == "DaemonSet") && child(t, ct, "readinessProbe") < 0)
        at(c, anchor, anchor.k0, anchor.k1 ? anchor.k1 : 1, SEV_HINT, "k8s-no-readiness", "No readinessProbe: traffic reaches the pod before it can answer");
    // security
    int sc = child(t, ct, "securityContext");
    if (sc >= 0) {
        int priv = child(t, sc, "privileged");
        if (priv >= 0 && unq(t.n[(size_t)priv].value) == "true") at_val(c, t.n[(size_t)priv], SEV_WARN, "k8s-privileged", "A privileged container has the run of the node");
        int ru = child(t, sc, "runAsUser");
        if (ru >= 0 && unq(t.n[(size_t)ru].value) == "0") at_val(c, t.n[(size_t)ru], SEV_WARN, "k8s-run-as-root", "runAsUser 0 runs the container as root");
        int ape = child(t, sc, "allowPrivilegeEscalation");
        if (ape >= 0 && unq(t.n[(size_t)ape].value) == "true") at_val(c, t.n[(size_t)ape], SEV_WARN, "k8s-priv-escalation", "allowPrivilegeEscalation: true lets a process gain more rights than its parent");
        int caps = path(t, sc, {"capabilities", "add"});
        if (caps >= 0) for (int it : items_of(t, caps)) { std::string v = unq(t.n[(size_t)it].value); if (v == "ALL" || v == "SYS_ADMIN") at(c, t.n[(size_t)it], t.n[(size_t)it].v0, t.n[(size_t)it].v0 + v.size(), SEV_WARN, "k8s-capability", "The " + v + " capability is close to privileged"); }
    }
    bool nonroot = pod_nonroot || (sc >= 0 && child(t, sc, "runAsNonRoot") >= 0) || (sc >= 0 && child(t, sc, "runAsUser") >= 0);
    if (!nonroot && !init) at(c, anchor, anchor.k0, anchor.k1 ? anchor.k1 : 1, SEV_HINT, "k8s-root-possible", "runAsNonRoot is not set: the container may run as root");
    // env
    int env = child(t, ct, "env");
    for (int e : items_of(t, env)) {
        int en = child(t, e, "name"), ev = child(t, e, "value");
        if (ev >= 0 && !t.n[(size_t)ev].value.empty()) {
            if (looks_scalar_non_string(t.n[(size_t)ev].value)) {
                Diag *d = nullptr;
                at_val(c, t.n[(size_t)ev], SEV_ERROR, "k8s-string-value", "An env value must be a string: write \"" + std::string(t.n[(size_t)ev].value) + "\" in quotes", &d);
                if (d) { d->has_fix = true; d->f0 = (uint32_t)t.n[(size_t)ev].v0; d->f1 = (uint32_t)(t.n[(size_t)ev].v0 + t.n[(size_t)ev].value.size()); d->fix = "\"" + std::string(t.n[(size_t)ev].value) + "\""; }
            }
            if (en >= 0) {
                std::string nm = unq(t.n[(size_t)en].value), up;
                for (char ch : nm) up += (char)toupper((unsigned char)ch);
                for (const char *w : {"PASSWORD", "SECRET", "TOKEN", "API_KEY", "APIKEY", "PRIVATE_KEY"})
                    if (up.find(w) != std::string::npos && unq(t.n[(size_t)ev].value).find("$(") == std::string::npos) { at_val(c, t.n[(size_t)ev], SEV_WARN, "k8s-env-secret", "'" + nm + "' looks like a secret kept in the manifest: use valueFrom: secretKeyRef"); break; }
            }
        }
    }
    // ports
    int ports = child(t, ct, "ports");
    for (int pt : items_of(t, ports)) {
        int cp = child(t, pt, "containerPort");
        if (cp < 0) continue;
        const YN &pn = t.n[(size_t)cp];
        std::string v = unq(pn.value);
        if (quoted(pn.value) && !v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
            Diag *d = nullptr;
            at_val(c, pn, SEV_ERROR, "k8s-port-type", "containerPort must be a number, not a string", &d);
            if (d) { d->has_fix = true; d->f0 = (uint32_t)pn.v0; d->f1 = (uint32_t)(pn.v0 + pn.value.size()); d->fix = v; }
        } else if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos && (atol(v.c_str()) < 1 || atol(v.c_str()) > 65535)) at_val(c, pn, SEV_ERROR, "k8s-port-range", "A port is between 1 and 65535");
    }
}

void check_doc(Ctx &c, const Tree &t, uint32_t first_line) {
    const YN &root = t.n[0];
    if (root.kids.empty()) return;
    int api = child(t, 0, "apiVersion"), kind = child(t, 0, "kind"), meta = child(t, 0, "metadata");
    if (api < 0 && kind < 0) return;   /* not a Kubernetes document */
    auto doc_line = [&](const char *code, const std::string &msg) {
        Ctx cc{c.out, c.max};
        cc.line = first_line;
        cc.cur = sv();
        add(cc, 0, 1, SEV_ERROR, code, msg);
    };
    if (api < 0) { const YN &k = t.n[(size_t)kind]; at_key(c, k, SEV_ERROR, "k8s-missing-field", "Missing apiVersion"); }
    if (kind < 0) { const YN &a = t.n[(size_t)api]; at_key(c, a, SEV_ERROR, "k8s-missing-field", "Missing kind"); return; }
    std::string kd = unq(t.n[(size_t)kind].value), av = api >= 0 ? unq(t.n[(size_t)api].value) : "";
    if (kd.find("{{") != std::string::npos) return;
    int name = path(t, 0, {"metadata", "name"});
    if (meta < 0) at_key(c, t.n[(size_t)kind], SEV_ERROR, "k8s-missing-field", "Missing metadata");
    else if (name < 0 && child(t, meta, "generateName") < 0 && kd != "List") at_key(c, t.n[(size_t)meta], SEV_ERROR, "k8s-missing-field", "metadata.name is missing");
    if (name >= 0) {
        std::string nm = unq(t.n[(size_t)name].value);
        bool templated = nm.find("{{") != std::string::npos || nm.find('$') != std::string::npos;
        if (!templated && !nm.empty()) {
            bool bad = nm.size() > 253;
            bool upper = false;
            for (char ch : nm) { if (!(islower((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '-' || ch == '.')) { bad = true; if (isupper((unsigned char)ch)) upper = true; } }
            if (nm.front() == '-' || nm.front() == '.' || nm.back() == '-' || nm.back() == '.') bad = true;
            bool rbac = kd == "Role" || kd == "ClusterRole" || kd == "RoleBinding" || kd == "ClusterRoleBinding";   /* RBAC names may contain ':' and capitals */
            if (bad && !(rbac && nm.find_first_of(" ") == std::string::npos)) {
                Diag *d = nullptr;
                at_val(c, t.n[(size_t)name], SEV_ERROR, "k8s-name", "A name is lowercase letters, digits, '-' and '.', and starts and ends with a letter or digit", &d);
                if (d && upper) { std::string low = nm; for (char &ch : low) ch = (char)tolower((unsigned char)ch); bool only_case = true; for (char ch : low) if (!(islower((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '-' || ch == '.')) only_case = false; if (only_case) { d->has_fix = true; d->f0 = (uint32_t)t.n[(size_t)name].v0; d->f1 = (uint32_t)(t.n[(size_t)name].v0 + t.n[(size_t)name].value.size()); d->fix = quoted(t.n[(size_t)name].value) ? "\"" + low + "\"" : low; } }
            }
        }
    }
    // apiVersion against kind
    auto kit = kind_api().find(kd);
    if (api >= 0 && kit != kind_api().end() && av != kit->second) {
        const YN &an = t.n[(size_t)api];
        bool alt_ok = (kd == "HorizontalPodAutoscaler" && av == "autoscaling/v1");
        if (!alt_ok) {
            Diag *d = nullptr;
            bool beta = av.find("beta") != std::string::npos || av.find("alpha") != std::string::npos || starts(av, "extensions/");
            if (beta) at_val(c, an, SEV_ERROR, "k8s-api-removed", "'" + av + "' is deprecated or removed; " + kd + " is " + kit->second, &d);
            else at_val(c, an, SEV_ERROR, "k8s-api-mismatch", "kind " + kd + " belongs to " + kit->second + ", not " + av, &d);
            if (d) { d->has_fix = true; d->f0 = (uint32_t)an.v0; d->f1 = (uint32_t)(an.v0 + an.value.size()); d->fix = kit->second; }
        }
    } else if (api >= 0 && kit == kind_api().end() && core_group(av) && (av.find("beta") != std::string::npos || av.find("alpha") != std::string::npos))
        at_val(c, t.n[(size_t)api], SEV_WARN, "k8s-api-beta", "'" + av + "' is a pre-release API and may be removed in a later Kubernetes version");

    // labels, annotations and node selectors are string maps
    string_map(c, t, path(t, 0, {"metadata", "labels"}), "label");
    string_map(c, t, path(t, 0, {"metadata", "annotations"}), "annotation");
    string_map(c, t, path(t, 0, {"spec", "template", "metadata", "labels"}), "label");
    string_map(c, t, path(t, 0, {"spec", "template", "metadata", "annotations"}), "annotation");
    string_map(c, t, path(t, 0, {"spec", "selector", "matchLabels"}), "label");
    string_map(c, t, path(t, 0, {"spec", "nodeSelector"}), "node selector");
    string_map(c, t, path(t, 0, {"spec", "template", "spec", "nodeSelector"}), "node selector");

    bool workload = kd == "Deployment" || kd == "StatefulSet" || kd == "DaemonSet" || kd == "ReplicaSet";
    if (workload) {
        int sel = path(t, 0, {"spec", "selector"});
        int ml = path(t, 0, {"spec", "selector", "matchLabels"});
        int me = path(t, 0, {"spec", "selector", "matchExpressions"});
        int spec = child(t, 0, "spec");
        if (sel < 0 && spec >= 0) at_key(c, t.n[(size_t)spec], SEV_ERROR, "k8s-selector-missing", kd + " needs spec.selector (apps/v1 requires it)");
        else if (sel >= 0 && ml < 0 && me < 0) at_key(c, t.n[(size_t)sel], SEV_ERROR, "k8s-selector-missing", "spec.selector needs matchLabels or matchExpressions");
        int pl = path(t, 0, {"spec", "template", "metadata", "labels"});
        if (ml >= 0 && pl >= 0 && !t.n[(size_t)ml].kids.empty()) {
            for (int k : t.n[(size_t)ml].kids) {
                const YN &kn = t.n[(size_t)k];
                if (!kn.has_key) continue;
                int pk = child(t, pl, kn.key.c_str());
                if (pk < 0 || unq(t.n[(size_t)pk].value) != unq(kn.value)) { at_key(c, kn, SEV_ERROR, "k8s-selector-mismatch", "The selector label '" + kn.key + "' is not in the pod template labels, so the " + kd + " would select no pods (and the API server rejects it)"); break; }
            }
        } else if (ml >= 0 && pl < 0 && path(t, 0, {"spec", "template"}) >= 0) at_key(c, t.n[(size_t)ml], SEV_ERROR, "k8s-selector-mismatch", "The pod template has no labels for this selector to match");
    }
    if (kd == "Service") {
        int spec = child(t, 0, "spec"), sel = path(t, 0, {"spec", "selector"});
        int type = path(t, 0, {"spec", "type"});
        if (spec >= 0 && sel < 0 && !(type >= 0 && unq(t.n[(size_t)type].value) == "ExternalName")) at_key(c, t.n[(size_t)spec], SEV_HINT, "k8s-service-selector", "The Service has no selector: it selects no pods (fine if you manage Endpoints yourself)");
        string_map(c, t, sel, "selector");
        for (int pt : items_of(t, path(t, 0, {"spec", "ports"}))) {
            for (const char *pk : {"port", "targetPort"}) {
                int p = child(t, pt, pk);
                if (p < 0) continue;
                std::string v = unq(t.n[(size_t)p].value);
                if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos && !quoted(t.n[(size_t)p].value) && (atol(v.c_str()) < 1 || atol(v.c_str()) > 65535)) at_val(c, t.n[(size_t)p], SEV_ERROR, "k8s-port-range", "A port is between 1 and 65535");
            }
        }
    }
    if (kd == "Secret") {
        int data = child(t, 0, "data"), sd = child(t, 0, "stringData");
        if ((data >= 0 && !t.n[(size_t)data].kids.empty()) || (sd >= 0 && !t.n[(size_t)sd].kids.empty()))
            at_val(c, t.n[(size_t)kind], SEV_WARN, "k8s-secret-in-file", "A Secret with its values in the file ends up in version control: use SOPS, Sealed Secrets or an external secret store");
    }
    // pod spec
    int pod = -1;
    if (kd == "Pod") pod = child(t, 0, "spec");
    else if (workload || kd == "Job") pod = path(t, 0, {"spec", "template", "spec"});
    else if (kd == "CronJob") pod = path(t, 0, {"spec", "jobTemplate", "spec", "template", "spec"});
    if (pod >= 0) {
        for (const char *hk : {"hostNetwork", "hostPID", "hostIPC"}) {
            int h = child(t, pod, hk);
            if (h >= 0 && unq(t.n[(size_t)h].value) == "true") at_key(c, t.n[(size_t)h], SEV_WARN, "k8s-host-namespace", std::string(hk) + ": true shares the node's namespace with the pod");
        }
        for (int v : items_of(t, child(t, pod, "volumes"))) { int hp = child(t, v, "hostPath"); if (hp >= 0) at_key(c, t.n[(size_t)hp], SEV_WARN, "k8s-hostpath", "hostPath gives the pod access to the node's file system"); }
        int psc = child(t, pod, "securityContext");
        bool pod_nonroot = psc >= 0 && (child(t, psc, "runAsNonRoot") >= 0 || child(t, psc, "runAsUser") >= 0);
        int rp = child(t, pod, "restartPolicy");
        if (rp >= 0) { std::string v = unq(t.n[(size_t)rp].value); if (v != "Always" && v != "OnFailure" && v != "Never") at_val(c, t.n[(size_t)rp], SEV_ERROR, "k8s-restart-policy", "restartPolicy must be Always, OnFailure or Never"); else if ((kd == "Job" || kd == "CronJob") && v == "Always") at_val(c, t.n[(size_t)rp], SEV_ERROR, "k8s-restart-policy", "A Job's pods cannot use restartPolicy Always: use OnFailure or Never"); }
        std::set<std::string> names;
        int cons = child(t, pod, "containers");
        if (cons < 0) at_key(c, t.n[(size_t)pod], SEV_ERROR, "k8s-no-containers", "The pod spec has no containers");
        for (int ct : items_of(t, cons)) check_container(c, t, ct, kd, false, pod_nonroot, names);
        for (int ct : items_of(t, child(t, pod, "initContainers"))) check_container(c, t, ct, kd, true, true, names);
    }
}

}  // namespace

bool k8s_detect(sv text, const std::string &path) {
    if (path.find("/templates/") != std::string::npos && text.find("{{") != sv::npos) return false;   /* a Helm template is not YAML yet */
    size_t p = 0;
    bool api = false, kind = false;
    while (p < text.size() && !(api && kind)) {
        size_t e = text.find('\n', p);
        if (e == sv::npos) e = text.size();
        sv l = text.substr(p, e - p);
        if (starts(l, "apiVersion:")) api = true;
        else if (starts(l, "kind:")) kind = true;
        p = e + 1;
    }
    return api && kind;
}

void k8s_pass(Ctx &c, sv text) {
    std::vector<DocTree> docs;
    build_docs(text, docs);
    for (DocTree &d : docs) { if (c.full()) break; check_doc(c, d.t, d.first_line); }
    c.cur = sv();
}

}  // namespace lintp
