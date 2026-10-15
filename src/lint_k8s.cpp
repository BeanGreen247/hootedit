/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include <sys/stat.h>

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

void build_docs(sv text, sv orig, std::vector<DocTree> &docs) {   /* text is what is parsed; orig the same lines as the user wrote them (same length: Helm actions are blanked in text) */
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
        sv oline = orig.size() == text.size() ? orig.substr(pos, line.size()) : line;
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
                    nd.parent = stack.back(); nd.item = item; nd.has_key = has_key; nd.line = ln; nd.text = oline;
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

/* ---------------------------------------------------------------- schema: field tables of the Kubernetes API, CRDs, Kustomize, Chart.yaml */

enum { F_STR, F_INT, F_NUM, F_BOOL, F_OBJ, F_ARR_OBJ, F_ARR_SCALAR, F_MAP_STR, F_MAP_OBJ, F_ANY, F_INTSTR, F_QUANT, F_MAP_ANY, F_ARR_ANY, F_CORE_OBJ };

struct KStaticField { const char *name; uint8_t kind; uint16_t ref; uint8_t req; };
struct KStaticType { const char *name; uint16_t first, count; uint8_t preserve; };
struct KStaticGvk { const char *gv, *kind; uint16_t type; };
#include "k8s_schema.inc"

struct SField { std::string name; uint8_t kind = F_ANY; int ref = 0; bool req = false; std::vector<std::string> enums; };
struct SType { std::string name; bool preserve = false; std::vector<SField> fields; };
struct SchemaSet { std::vector<SType> types; std::map<std::string, int> roots; };   /* roots: "apiVersion|Kind" -> type */

const SchemaSet &core_schema() {
    static const SchemaSet s = [] {
        SchemaSet r;
        r.types.resize(sizeof KS_TYPES / sizeof KS_TYPES[0]);
        for (size_t i = 0; i < r.types.size(); i++) {
            r.types[i].name = KS_TYPES[i].name;
            r.types[i].preserve = KS_TYPES[i].preserve != 0;
            for (size_t k = 0; k < KS_TYPES[i].count; k++) {
                const KStaticField &f = KS_FIELDS[KS_TYPES[i].first + k];
                SField sf; sf.name = f.name; sf.kind = f.kind; sf.ref = f.ref; sf.req = f.req != 0;
                r.types[i].fields.push_back(std::move(sf));
            }
        }
        for (const KStaticGvk &g : KS_GVKS) r.roots[std::string(g.gv) + "|" + g.kind] = g.type;
        return r;
    }();
    return s;
}

int core_type_named(const char *name) {
    const SchemaSet &s = core_schema();
    for (size_t i = 0; i < s.types.size(); i++) if (s.types[i].name == name) return (int)i;
    return -1;
}

const SField *find_field(const SType &t, const std::string &n) { for (const SField &f : t.fields) if (f.name == n) return &f; return nullptr; }

/* the swagger-style schema in a YAML tree (a CRD's openAPIV3Schema, or our own tables for Kustomize and Chart.yaml) turned into the same tables */
std::vector<std::string> flow_items(sv v) {
    std::vector<std::string> out;
    v = trim(v);
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
        v = v.substr(1, v.size() - 2);
        size_t p = 0;
        while (p <= v.size()) { size_t e = v.find(',', p); if (e == sv::npos) e = v.size(); std::string s = unq(trim(v.substr(p, e - p))); if (!s.empty()) out.push_back(s); p = e + 1; }
    }
    return out;
}

std::vector<std::string> list_values(const Tree &t, int node) {
    std::vector<std::string> out;
    if (node < 0) return out;
    out = flow_items(t.n[(size_t)node].value);
    for (int it : items_of(t, node)) out.push_back(unq(t.n[(size_t)it].value));
    return out;
}

struct Conv { SchemaSet *set; const Tree *t; int core_meta; };

int conv_object(Conv &cv, int node);

std::pair<uint8_t, int> conv_field(Conv &cv, int node) {
    const Tree &t = *cv.t;
    std::string type = node >= 0 && child(t, node, "type") >= 0 ? unq(t.n[(size_t)child(t, node, "type")].value) : std::string();
    auto flag = [&](const char *k) { int n = child(t, node, k); return n >= 0 && unq(t.n[(size_t)n].value) == "true"; };
    if (flag("x-kubernetes-int-or-string")) return {F_INTSTR, 0};
    if (type == "string") return {F_STR, 0};
    if (type == "integer") return {F_INT, 0};
    if (type == "number") return {F_NUM, 0};
    if (type == "boolean") return {F_BOOL, 0};
    if (type == "array") {
        int it = child(t, node, "items");
        if (it < 0) return {F_ARR_ANY, 0};
        std::string itype = child(t, it, "type") >= 0 ? unq(t.n[(size_t)child(t, it, "type")].value) : std::string();
        if (itype == "object" && child(t, it, "properties") >= 0) return {F_ARR_OBJ, conv_object(cv, it)};
        if (itype == "string" || itype == "integer" || itype == "number" || itype == "boolean") return {F_ARR_SCALAR, 0};
        return {F_ARR_ANY, 0};
    }
    if (type == "object" || child(t, node, "properties") >= 0) {
        if (child(t, node, "properties") >= 0) return {F_OBJ, conv_object(cv, node)};
        int ap = child(t, node, "additionalProperties");
        if (ap >= 0) {
            const YN &an = t.n[(size_t)ap];
            if (an.kids.empty()) return {F_MAP_ANY, 0};
            std::string at_ = child(t, ap, "type") >= 0 ? unq(t.n[(size_t)child(t, ap, "type")].value) : std::string();
            if (at_ == "string") return {F_MAP_STR, 0};
            if (child(t, ap, "properties") >= 0) return {F_MAP_OBJ, conv_object(cv, ap)};
        }
        return {F_ANY, 0};
    }
    return {F_ANY, 0};
}

int conv_object(Conv &cv, int node) {
    const Tree &t = *cv.t;
    int idx = (int)cv.set->types.size();
    cv.set->types.emplace_back();
    cv.set->types[(size_t)idx].name = "object";
    int pres = child(t, node, "x-kubernetes-preserve-unknown-fields");
    cv.set->types[(size_t)idx].preserve = pres >= 0 && unq(t.n[(size_t)pres].value) == "true";
    std::vector<std::string> req = list_values(t, child(t, node, "required"));
    int props = child(t, node, "properties");
    std::vector<SField> fields;
    if (props >= 0) for (int p : t.n[(size_t)props].kids) {
        const YN &pn = t.n[(size_t)p];
        if (!pn.has_key) continue;
        SField f;
        f.name = pn.key;
        auto kr = conv_field(cv, p);
        f.kind = kr.first; f.ref = kr.second;
        f.req = std::find(req.begin(), req.end(), pn.key) != req.end();
        f.enums = list_values(t, child(t, p, "enum"));
        fields.push_back(std::move(f));
    }
    cv.set->types[(size_t)idx].fields = std::move(fields);
    return idx;
}

/* our own schemas, written as OpenAPI in YAML */
const char *KUSTOMIZE_SCHEMA = R"Y(
type: object
properties:
  apiVersion: {type: string}
  kind: {type: string}
  metadata: {x-kubernetes-preserve-unknown-fields: true}
  namespace: {type: string}
  namePrefix: {type: string}
  nameSuffix: {type: string}
  resources: {type: array, items: {type: string}}
  bases: {type: array, items: {type: string}}
  components: {type: array, items: {type: string}}
  crds: {type: array, items: {type: string}}
  generators: {type: array, items: {type: string}}
  transformers: {type: array, items: {type: string}}
  validators: {type: array, items: {type: string}}
  buildMetadata: {type: array, items: {type: string}}
  commonLabels: {type: object, additionalProperties: {type: string}}
  commonAnnotations: {type: object, additionalProperties: {type: string}}
  labels:
    type: array
    items:
      type: object
      properties:
        pairs: {type: object, additionalProperties: {type: string}}
        includeSelectors: {type: boolean}
        includeTemplates: {type: boolean}
  images:
    type: array
    items:
      type: object
      required: [name]
      properties:
        name: {type: string}
        newName: {type: string}
        newTag: {type: string}
        digest: {type: string}
  replicas:
    type: array
    items:
      type: object
      required: [name, count]
      properties:
        name: {type: string}
        count: {type: integer}
  patches:
    type: array
    items:
      type: object
      properties:
        path: {type: string}
        patch: {type: string}
        options: {type: object, additionalProperties: true}
        target:
          type: object
          properties:
            group: {type: string}
            version: {type: string}
            kind: {type: string}
            name: {type: string}
            namespace: {type: string}
            labelSelector: {type: string}
            annotationSelector: {type: string}
  patchesStrategicMerge: {type: array, items: {}}
  patchesJson6902: {type: array, items: {}}
  replacements: {type: array, items: {}}
  vars: {type: array, items: {}}
  sortOptions: {x-kubernetes-preserve-unknown-fields: true}
  openapi:
    type: object
    properties:
      path: {type: string}
      version: {type: string}
  generatorOptions:
    type: object
    properties:
      disableNameSuffixHash: {type: boolean}
      immutable: {type: boolean}
      labels: {type: object, additionalProperties: {type: string}}
      annotations: {type: object, additionalProperties: {type: string}}
  configMapGenerator:
    type: array
    items:
      type: object
      properties:
        name: {type: string}
        namespace: {type: string}
        behavior: {type: string, enum: [create, replace, merge]}
        envs: {type: array, items: {type: string}}
        env: {type: string}
        files: {type: array, items: {type: string}}
        literals: {type: array, items: {type: string}}
        options:
          type: object
          properties:
            disableNameSuffixHash: {type: boolean}
            immutable: {type: boolean}
            labels: {type: object, additionalProperties: {type: string}}
            annotations: {type: object, additionalProperties: {type: string}}
  secretGenerator:
    type: array
    items:
      type: object
      properties:
        name: {type: string}
        namespace: {type: string}
        behavior: {type: string, enum: [create, replace, merge]}
        type: {type: string}
        envs: {type: array, items: {type: string}}
        env: {type: string}
        files: {type: array, items: {type: string}}
        literals: {type: array, items: {type: string}}
        options:
          type: object
          properties:
            disableNameSuffixHash: {type: boolean}
            immutable: {type: boolean}
            labels: {type: object, additionalProperties: {type: string}}
            annotations: {type: object, additionalProperties: {type: string}}
  helmCharts:
    type: array
    items:
      type: object
      required: [name]
      properties:
        name: {type: string}
        repo: {type: string}
        version: {type: string}
        releaseName: {type: string}
        namespace: {type: string}
        valuesFile: {type: string}
        valuesInline: {x-kubernetes-preserve-unknown-fields: true}
        valuesMerge: {type: string}
        includeCRDs: {type: boolean}
        additionalValuesFiles: {type: array, items: {type: string}}
        apiVersions: {type: array, items: {type: string}}
        kubeVersion: {type: string}
        nameTemplate: {type: string}
        skipTests: {type: boolean}
)Y";

const char *CHART_SCHEMA = R"Y(
type: object
required: [apiVersion, name, version]
properties:
  apiVersion: {type: string, enum: [v1, v2]}
  name: {type: string}
  version: {type: string}
  kubeVersion: {type: string}
  description: {type: string}
  type: {type: string, enum: [application, library]}
  keywords: {type: array, items: {type: string}}
  home: {type: string}
  sources: {type: array, items: {type: string}}
  dependencies:
    type: array
    items:
      type: object
      required: [name]
      properties:
        name: {type: string}
        version: {type: string}
        repository: {type: string}
        condition: {type: string}
        tags: {type: array, items: {type: string}}
        import-values: {type: array, items: {}}
        alias: {type: string}
  maintainers:
    type: array
    items:
      type: object
      properties:
        name: {type: string}
        email: {type: string}
        url: {type: string}
  icon: {type: string}
  appVersion: {type: string}
  deprecated: {type: boolean}
  annotations: {type: object, additionalProperties: {type: string}}
)Y";

const SchemaSet &own_schema(const char *yaml, const char *key) {
    static std::map<std::string, SchemaSet> cache;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<DocTree> docs;
    sv text = yaml;
    build_docs(text, text, docs);
    SchemaSet s;
    Conv cv{&s, &docs[0].t, -1};
    int root = conv_object(cv, 0);
    s.roots["*"] = root;
    return cache.emplace(key, std::move(s)).first->second;
}

/* custom resources: CRDs found in the workspace and in ~/.config/hootedit/crds, converted once by a background scan */
struct CrdIndex { std::mutex mu; std::map<std::string, std::shared_ptr<const SchemaSet>> by_key; bool built = false; std::atomic<bool> wanted{false}; };
CrdIndex &crd_index() { static CrdIndex *i = new CrdIndex; return *i; }

void crd_from_doc(const Tree &t, std::map<std::string, std::shared_ptr<const SchemaSet>> &out) {
    int kind = child(t, 0, "kind");
    if (kind < 0 || unq(t.n[(size_t)kind].value) != "CustomResourceDefinition") return;
    int spec = child(t, 0, "spec");
    std::string group = spec >= 0 && child(t, spec, "group") >= 0 ? unq(t.n[(size_t)child(t, spec, "group")].value) : std::string();
    int names = path(t, 0, {"spec", "names", "kind"});
    if (group.empty() || names < 0) return;
    std::string ckind = unq(t.n[(size_t)names].value);
    auto build = [&](const std::string &version, int schema_node) {
        if (schema_node < 0) return;
        auto set = std::make_shared<SchemaSet>();
        Conv cv{set.get(), &t, core_type_named("ObjectMeta")};
        int root = conv_object(cv, schema_node);
        SType &rt = set->types[(size_t)root];
        rt.name = ckind;
        for (const char *hf : {"apiVersion", "kind"}) if (!find_field(rt, hf)) { SField f; f.name = hf; f.kind = F_STR; rt.fields.push_back(f); }
        if (SField *mf = const_cast<SField *>(find_field(rt, "metadata"))) { mf->kind = F_CORE_OBJ; mf->ref = cv.core_meta; }
        else { SField f; f.name = "metadata"; f.kind = F_CORE_OBJ; f.ref = cv.core_meta; rt.fields.push_back(f); }
        set->roots[group + "/" + version + "|" + ckind] = root;
        out[group + "/" + version + "|" + ckind] = set;
    };
    for (int v : items_of(t, child(t, spec, "versions"))) {
        int nm = child(t, v, "name");
        if (nm < 0) continue;
        build(unq(t.n[(size_t)nm].value), path(t, v, {"schema", "openAPIV3Schema"}));
    }
    int legacy = path(t, spec, {"validation", "openAPIV3Schema"});
    int ver = child(t, spec, "version");
    if (legacy >= 0 && ver >= 0) build(unq(t.n[(size_t)ver].value), legacy);
}

std::shared_ptr<const SchemaSet> crd_find(const std::string &key) {
    CrdIndex &ix = crd_index();
    std::lock_guard<std::mutex> g(ix.mu);
    auto it = ix.by_key.find(key);
    if (it != ix.by_key.end()) return it->second;
    if (!ix.built) ix.wanted = true;
    return nullptr;
}

/* ---------------------------------------------------------------- validation against the tables */

struct VOpts { bool helm = false; };

std::string lower_copy(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

bool templated_value(const YN &n) { return n.text.find("{{") != sv::npos || n.value.find("$(") != sv::npos || n.value.find("${") != sv::npos; }

void quote_fix(Diag *d, const YN &kn) {
    if (!d) return;
    d->has_fix = true; d->f0 = (uint32_t)kn.v0; d->f1 = (uint32_t)(kn.v0 + kn.value.size()); d->fix = "\"" + std::string(kn.value) + "\"";
}
void unquote_fix(Diag *d, const YN &kn, const std::string &inner) {
    if (!d) return;
    d->has_fix = true; d->f0 = (uint32_t)kn.v0; d->f1 = (uint32_t)(kn.v0 + kn.value.size()); d->fix = inner;
}

void validate_obj(Ctx &c, const Tree &t, int node, const SchemaSet &set, const SchemaSet &core, int type, const VOpts &o, bool is_root, int anchor);

bool bool_word(sv v) {
    static const char *w[] = {"true", "false", "yes", "no", "on", "off", "True", "False", "TRUE", "FALSE", "Yes", "No", "YES", "NO", "On", "Off", "ON", "OFF", "y", "n", "Y", "N"};
    for (const char *x : w) if (v == x) return true;
    return false;
}

void check_field(Ctx &c, const Tree &t, int kid, const SField &f, const SchemaSet &set, const SchemaSet &core, const VOpts &o) {
    const YN &k = t.n[(size_t)kid];
    bool has_kids = !k.kids.empty();
    bool flow = !k.value.empty() && (k.value[0] == '{' || k.value[0] == '[');
    bool block = !k.value.empty() && (k.value[0] == '|' || k.value[0] == '>');
    bool templ = templated_value(k);
    bool list_kids = false, map_kids = false;
    for (int x : k.kids) { if (t.n[(size_t)x].item) list_kids = true; else if (t.n[(size_t)x].has_key) map_kids = true; }
    auto wrong = [&](const std::string &msg) { at_key(c, k, SEV_ERROR, "k8s-wrong-type", msg); };
    std::string v = unq(k.value);
    bool is_quoted = quoted(k.value);
    switch (f.kind) {
    case F_STR:
        if (has_kids) { wrong("'" + k.key + "' must be a string, not " + (list_kids ? "a list" : "a mapping")); break; }
        if (!k.value.empty() && !flow && !block && !templ && !is_quoted && looks_scalar_non_string(k.value)) {
            Diag *d = nullptr;
            at_val(c, k, SEV_ERROR, "k8s-string-value", "'" + k.key + "' must be a string: write \"" + std::string(k.value) + "\" in quotes, or Kubernetes rejects the number or boolean", &d);
            quote_fix(d, k);
        }
        if (!f.enums.empty() && !templ && !v.empty() && std::find(f.enums.begin(), f.enums.end(), v) == f.enums.end()) {
            std::string list;
            for (size_t i = 0; i < f.enums.size() && i < 8; i++) list += (i ? ", " : "") + f.enums[i];
            at_val(c, k, SEV_ERROR, "k8s-enum", "'" + v + "' is not one of: " + list);
        }
        break;
    case F_INT: case F_NUM:
        if (has_kids) { wrong("'" + k.key + "' must be a number"); break; }
        if (k.value.empty() || flow || templ) break;
        if (is_quoted) {
            bool numeric = !v.empty() && v.find_first_not_of("0123456789.-+") == std::string::npos;
            Diag *d = nullptr;
            at_val(c, k, SEV_ERROR, "k8s-number-type", "'" + k.key + "' must be a number, not a string: remove the quotes", &d);
            if (numeric) unquote_fix(d, k, v);
        } else if (!looks_scalar_non_string(k.value) || bool_word(k.value) || (f.kind == F_INT && k.value.find('.') != sv::npos)) at_val(c, k, SEV_ERROR, "k8s-number-type", "'" + k.key + "' must be " + (f.kind == F_INT ? "a whole number" : "a number") + ", not '" + std::string(k.value) + "'");
        break;
    case F_BOOL:
        if (has_kids) { wrong("'" + k.key + "' must be true or false"); break; }
        if (k.value.empty() || flow || templ) break;
        if (is_quoted) { Diag *d = nullptr; at_val(c, k, SEV_ERROR, "k8s-number-type", "'" + k.key + "' must be true or false, not a string: remove the quotes", &d); if (v == "true" || v == "false") unquote_fix(d, k, v); }
        else if (!bool_word(k.value)) at_val(c, k, SEV_ERROR, "k8s-number-type", "'" + k.key + "' must be true or false, not '" + std::string(k.value) + "'");
        break;
    case F_INTSTR: case F_QUANT:
        if (has_kids) wrong("'" + k.key + "' must be a single value");
        break;
    case F_OBJ: case F_CORE_OBJ: {
        if (list_kids && !map_kids) { wrong("'" + k.key + "' must be a mapping (key: value pairs), not a list"); break; }
        if (!has_kids && !k.value.empty() && !flow && !templ && !block) { wrong("'" + k.key + "' must be a mapping (key: value pairs), not '" + std::string(k.value) + "'"); break; }
        if (has_kids) {
            const SchemaSet &ts = f.kind == F_CORE_OBJ ? core : set;
            validate_obj(c, t, kid, ts, core, f.ref, o, false, kid);
        }
        break;
    }
    case F_ARR_OBJ: {
        if (map_kids && !list_kids) { wrong("'" + k.key + "' must be a list: start each entry with '- '"); break; }
        if (!has_kids && !k.value.empty() && !flow && !templ) { wrong("'" + k.key + "' must be a list, not '" + std::string(k.value) + "'"); break; }
        for (int it : items_of(t, kid)) {
            const YN &in = t.n[(size_t)it];
            if (in.kids.empty()) { if (!in.value.empty() && !templated_value(in)) at(c, in, in.v0, in.v0 + in.value.size(), SEV_ERROR, "k8s-wrong-type", "A list entry of '" + k.key + "' must be a mapping, not '" + std::string(in.value) + "'"); continue; }
            validate_obj(c, t, it, set, core, f.ref, o, false, in.kids[0]);
        }
        break;
    }
    case F_ARR_SCALAR: case F_ARR_ANY:
        if (map_kids && !list_kids) wrong("'" + k.key + "' must be a list: start each entry with '- '");
        else if (!has_kids && !k.value.empty() && !flow && !templ && !block) wrong("'" + k.key + "' must be a list, not '" + std::string(k.value) + "'");
        break;
    case F_MAP_STR:
        if (list_kids && !map_kids) { wrong("'" + k.key + "' must be a mapping (key: value pairs), not a list"); break; }
        for (int x : k.kids) {
            const YN &en = t.n[(size_t)x];
            if (!en.has_key) continue;
            if (!en.kids.empty()) { at_key(c, en, SEV_ERROR, "k8s-wrong-type", "The value of '" + en.key + "' must be a string"); continue; }
            if (!en.value.empty() && !quoted(en.value) && !templated_value(en) && en.value[0] != '{' && en.value[0] != '[' && en.value[0] != '|' && en.value[0] != '>' && looks_scalar_non_string(en.value)) {
                Diag *d = nullptr;
                at_val(c, en, SEV_ERROR, "k8s-string-value", "The value of '" + en.key + "' must be a string: write \"" + std::string(en.value) + "\" in quotes", &d);
                quote_fix(d, en);
            }
        }
        break;
    case F_MAP_OBJ:
        for (int x : k.kids) if (t.n[(size_t)x].has_key && !t.n[(size_t)x].kids.empty()) validate_obj(c, t, x, set, core, f.ref, o, false, x);
        break;
    default: break;
    }
}

void validate_obj(Ctx &c, const Tree &t, int node, const SchemaSet &set, const SchemaSet &core, int type, const VOpts &o, bool is_root, int anchor) {
    if (type < 0 || (size_t)type >= set.types.size()) return;
    const SType &ty = set.types[(size_t)type];
    std::set<std::string> seen;
    for (int kid : t.n[(size_t)node].kids) {
        const YN &kn = t.n[(size_t)kid];
        if (!kn.has_key) continue;
        if (is_root && kn.key == "status") { seen.insert(kn.key); continue; }
        seen.insert(kn.key);
        const SField *f = find_field(ty, kn.key);
        if (!f) {
            if (ty.preserve || kn.key.find("{{") != std::string::npos) continue;
            std::string best;
            int bd = kn.key.size() >= 6 ? 3 : 2;
            for (const SField &sf : ty.fields) { int dd = lev(kn.key, sf.name); if (dd < bd) { bd = dd; best = sf.name; } }
            if (lower_copy(kn.key) != kn.key) for (const SField &sf : ty.fields) if (lower_copy(sf.name) == lower_copy(kn.key)) best = sf.name;
            std::string where = ty.name == "object" ? std::string() : " in " + ty.name;
            Diag *d = nullptr;
            at(c, kn, kn.k0, kn.k1, SEV_ERROR, "k8s-unknown-field", "Unknown field '" + kn.key + "'" + where + (best.empty() ? std::string() : ". Did you mean '" + best + "'?"), &d);
            if (d && !best.empty()) { d->has_fix = true; d->f0 = (uint32_t)kn.k0; d->f1 = (uint32_t)kn.k1; d->fix = best; }
            continue;
        }
        check_field(c, t, kid, *f, set, core, o);
    }
    if (o.helm) return;   /* a template can add the missing parts */
    for (const SField &f : ty.fields) {
        if (!f.req || seen.count(f.name)) continue;
        if (is_root && (f.name == "status")) continue;
        const YN &an = t.n[(size_t)(anchor >= 0 ? anchor : node)];
        if (an.text.find("{{") != sv::npos) continue;
        at(c, an, an.has_key ? an.k0 : (an.item ? an.v0 : 0), an.has_key ? an.k1 : (an.item ? an.v0 + 1 : 1), SEV_ERROR, "k8s-required", "Missing required field '" + f.name + "'" + (ty.name == "object" ? std::string() : " in " + ty.name));
    }
}

void check_container(Ctx &c, const Tree &t, int ct, const std::string &kind, bool init, bool pod_nonroot, std::set<std::string> &names, bool helm) {
    const YN &cn = t.n[(size_t)ct];
    int name = child(t, ct, "name"), image = child(t, ct, "image");
    const YN &anchor = name >= 0 ? t.n[(size_t)name] : cn;
    auto fail_at = [&](int sev, const char *code, const std::string &msg) {
        if (name >= 0) at_val(c, t.n[(size_t)name], sev, code, msg);
        else if (!cn.kids.empty()) at_key(c, t.n[(size_t)cn.kids[0]], sev, code, msg);
    };
    (void)fail_at;
    if (name >= 0) {
        std::string nm = unq(t.n[(size_t)name].value);
        if (!names.insert(nm).second) at_val(c, t.n[(size_t)name], SEV_ERROR, "k8s-container-duplicate", "Two containers are named '" + nm + "'");
    }
    if (image >= 0) {
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
    if (res < 0 && !init && !helm) at(c, anchor, anchor.k0, anchor.k1 ? anchor.k1 : 1, SEV_WARN, "k8s-no-resources", "No resources: set requests and limits so the scheduler and the node can protect each other");
    else if (res >= 0) {
        int lim = child(t, res, "limits"), req = child(t, res, "requests");
        if (!helm && (lim < 0 || child(t, lim, "memory") < 0)) at_key(c, t.n[(size_t)res], SEV_WARN, "k8s-no-memory-limit", "No memory limit: one leak can take the whole node");
        if (!helm && req < 0) at_key(c, t.n[(size_t)res], SEV_HINT, "k8s-no-requests", "No requests: the scheduler assumes the container needs nothing");
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
    if (!helm && !init && (kind == "Deployment" || kind == "StatefulSet" || kind == "DaemonSet") && child(t, ct, "readinessProbe") < 0)
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
    if (!nonroot && !init && !helm) at(c, anchor, anchor.k0, anchor.k1 ? anchor.k1 : 1, SEV_HINT, "k8s-root-possible", "runAsNonRoot is not set: the container may run as root");
    // env
    int env = child(t, ct, "env");
    for (int e : items_of(t, env)) {
        int en = child(t, e, "name"), ev = child(t, e, "value");
        if (ev >= 0 && !t.n[(size_t)ev].value.empty()) {
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
        if (!quoted(pn.value) && !v.empty() && v.find_first_not_of("0123456789") == std::string::npos && (atol(v.c_str()) < 1 || atol(v.c_str()) > 65535)) at_val(c, pn, SEV_ERROR, "k8s-port-range", "A port is between 1 and 65535");
    }
}

void check_doc(Ctx &c, const Tree &t, uint32_t first_line, bool helm) {
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

    // every field against the API's tables: unknown names, wrong shapes, missing required fields, numbers where strings are needed
    {
        const SchemaSet &core = core_schema();
        std::string key = av + "|" + kd;
        const SchemaSet *set = &core;
        std::shared_ptr<const SchemaSet> crd;
        auto rit = core.roots.find(key);
        int type = rit != core.roots.end() ? rit->second : -1;
        if (type < 0) {   /* an old or wrong apiVersion (already reported): check the fields against the current one so nothing else is missed */
            auto kk = kind_api().find(kd);
            if (kk != kind_api().end()) { auto r2 = core.roots.find(kk->second + "|" + kd); if (r2 != core.roots.end()) type = r2->second; }
        }
        if (type < 0 && av.find('/') != std::string::npos && !core_group(av) && av.find("{{") == std::string::npos) {   /* a custom resource: its CRD, if one is in the workspace */
            crd = crd_find(key);
            if (crd) { set = crd.get(); type = crd->roots.at(key); }
        }
        if (type >= 0) { VOpts vo; vo.helm = helm; validate_obj(c, t, 0, *set, core, type, vo, true, kind); }
    }

    bool workload = kd == "Deployment" || kd == "StatefulSet" || kd == "DaemonSet" || kd == "ReplicaSet";
    if (workload && !helm) {
        int sel = path(t, 0, {"spec", "selector"});
        int ml = path(t, 0, {"spec", "selector", "matchLabels"});
        int me = path(t, 0, {"spec", "selector", "matchExpressions"});
        int spec = child(t, 0, "spec");
        if (sel >= 0 && ml < 0 && me < 0) at_key(c, t.n[(size_t)sel], SEV_ERROR, "k8s-selector-missing", "spec.selector needs matchLabels or matchExpressions");
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
        for (int ct : items_of(t, cons)) check_container(c, t, ct, kd, false, pod_nonroot, names, helm);
        for (int ct : items_of(t, child(t, pod, "initContainers"))) check_container(c, t, ct, kd, true, true, names, helm);
    }
}

}  // namespace

bool k8s_detect(sv text, const std::string &path) {
    (void)path;
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

/* A Helm template is YAML with Go template actions in it. Whole-line actions ({{- if }}, {{- include ... | nindent 4 }}) become blank lines, actions inside a
 * value become a run of letters of the same length, so the structure can be read and every line keeps its number and its columns. */
std::string helm_neutralize(sv text) {
    std::string out(text);
    bool in_action = false;
    size_t pos = 0;
    while (pos <= out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        sv line(out.data() + pos, e - pos);
        sv t = trim(line);
        auto blank = [&](size_t from, size_t to) { for (size_t i = from; i < to; i++) if (out[i] != '\r') out[i] = ' '; };
        if (in_action) {
            size_t close = line.find("}}");
            if (close == sv::npos) blank(pos, e); else { blank(pos, pos + close + 2); in_action = false; }
        } else if (starts(t, "{{")) {
            size_t close = line.find("}}", line.find("{{"));
            if (close == sv::npos) { blank(pos, e); in_action = true; }
            else if (trim(line.substr(close + 2)).empty() || trim(line.substr(close + 2))[0] == '#') blank(pos, e);
            else { size_t o = line.find("{{"); for (size_t i = o; i < close + 2; i++) out[pos + i] = 'x'; }
        } else {
            size_t o = 0;
            while ((o = line.find("{{", o)) != sv::npos) {
                size_t close = line.find("}}", o);
                if (close == sv::npos) { for (size_t i = o; i < line.size(); i++) out[pos + i] = 'x'; break; }
                for (size_t i = o; i < close + 2; i++) out[pos + i] = 'x';
                o = close + 2;
            }
        }
        if (e >= out.size()) break;
        pos = e + 1;
    }
    return out;
}

void k8s_pass(Ctx &c, sv text, sv orig, bool helm) {
    std::vector<DocTree> docs;
    build_docs(text, orig, docs);
    for (DocTree &d : docs) { if (c.full()) break; check_doc(c, d.t, d.first_line, helm); }
    c.cur = sv();
}

/* kustomization.yaml and Chart.yaml: the same tables, plus the things the tables cannot say */
void deprecated_key(Ctx &c, const Tree &t, const char *key, const char *msg) {
    int n = child(t, 0, key);
    if (n >= 0) at_key(c, t.n[(size_t)n], SEV_WARN, "k8s-deprecated", msg);
}

void kustomize_pass(Ctx &c, sv text, const std::string &path) {
    std::vector<DocTree> docs;
    build_docs(text, text, docs);
    const SchemaSet &set = own_schema(KUSTOMIZE_SCHEMA, "kustomize");
    const SchemaSet &core = core_schema();
    std::string dir = path.rfind('/') == std::string::npos ? std::string(".") : path.substr(0, path.rfind('/'));
    for (DocTree &d : docs) {
        const Tree &t = d.t;
        if (t.n[0].kids.empty()) continue;
        VOpts vo;
        validate_obj(c, t, 0, set, core, set.roots.at("*"), vo, true, t.n[0].kids[0]);
        deprecated_key(c, t, "bases", "'bases' is deprecated: list the entries under 'resources'");
        deprecated_key(c, t, "patchesStrategicMerge", "'patchesStrategicMerge' is deprecated: use 'patches'");
        deprecated_key(c, t, "patchesJson6902", "'patchesJson6902' is deprecated: use 'patches' with a target");
        deprecated_key(c, t, "commonLabels", "'commonLabels' is deprecated: use 'labels' (and set includeSelectors only if you mean to change selectors)");
        deprecated_key(c, t, "vars", "'vars' is deprecated: use 'replacements'");
        for (const char *key : {"resources", "bases", "components", "crds", "generators", "transformers", "validators"}) {
            for (int it : items_of(t, child(t, 0, key))) {
                const YN &in = t.n[(size_t)it];
                std::string p = unq(in.value);
                if (p.empty() || p.find("://") != std::string::npos || starts(p, "github.com/") || starts(p, "git@") || p.find("?ref=") != std::string::npos || p.find("{{") != std::string::npos || p.find('$') != std::string::npos) continue;
                struct stat st;
                std::string full = (p[0] == '/' ? std::string() : dir + "/") + p;
                if (stat(full.c_str(), &st) != 0) at(c, in, in.v0, in.v0 + in.value.size(), SEV_WARN, "k8s-kustomize-missing", "'" + p + "' does not exist (paths are relative to this kustomization)");
            }
        }
        for (int it : items_of(t, child(t, 0, "patches"))) {
            int pp = child(t, it, "path");
            if (pp < 0) continue;
            std::string p = unq(t.n[(size_t)pp].value);
            struct stat st;
            if (!p.empty() && p.find("{{") == std::string::npos && stat((dir + "/" + p).c_str(), &st) != 0) at_val(c, t.n[(size_t)pp], SEV_WARN, "k8s-kustomize-missing", "'" + p + "' does not exist (paths are relative to this kustomization)");
        }
    }
    c.cur = sv();
}

void chart_pass(Ctx &c, sv text) {
    std::vector<DocTree> docs;
    build_docs(text, text, docs);
    const SchemaSet &set = own_schema(CHART_SCHEMA, "chart");
    const SchemaSet &core = core_schema();
    for (DocTree &d : docs) {
        const Tree &t = d.t;
        if (t.n[0].kids.empty()) continue;
        VOpts vo;
        validate_obj(c, t, 0, set, core, set.roots.at("*"), vo, true, t.n[0].kids[0]);
        int av = child(t, 0, "apiVersion"), ver = child(t, 0, "version");
        if (av >= 0 && unq(t.n[(size_t)av].value) == "v1") at_val(c, t.n[(size_t)av], SEV_HINT, "helm-api-v1", "Chart apiVersion v1 is Helm 2; v2 is the current format (it also allows dependencies in Chart.yaml)");
        if (ver >= 0) {
            std::string v = unq(t.n[(size_t)ver].value);
            size_t i = 0, parts = 0;
            while (i < v.size() && parts < 3) { size_t j = i; while (j < v.size() && isdigit((unsigned char)v[j])) j++; if (j == i) break; parts++; i = j; if (i < v.size() && v[i] == '.' && parts < 3) i++; else break; }
            if (!v.empty() && v.find("{{") == std::string::npos && (parts < 3 || (i < v.size() && v[i] != '-' && v[i] != '+'))) at_val(c, t.n[(size_t)ver], SEV_ERROR, "helm-version", "A chart version must be a semantic version like 1.2.3 (not '" + v + "')");
        }
    }
    c.cur = sv();
}

void k8s_special(Ctx &c, sv text, const std::string &path) {
    std::string base = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    if (base == "Chart.yaml") chart_pass(c, text);
    else kustomize_pass(c, text, path);
}

}  // namespace lintp

/* the CRD scan, called from a thread of the editor with the YAML files of the workspace */
bool lint_crd_wanted() { return lintp::crd_index().wanted.exchange(false); }
bool lint_crd_ready() { lintp::CrdIndex &ix = lintp::crd_index(); std::lock_guard<std::mutex> g(ix.mu); return ix.built; }
void lint_crd_reset() { lintp::CrdIndex &ix = lintp::crd_index(); std::lock_guard<std::mutex> g(ix.mu); ix.built = false; ix.by_key.clear(); }

void lint_crd_build(const std::vector<std::string> &files) {
    std::map<std::string, std::shared_ptr<const lintp::SchemaSet>> found;
    for (const std::string &f : files) {
        FILE *fp = fopen(f.c_str(), "rb");
        if (!fp) continue;
        std::string text;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, fp)) > 0 && text.size() < (4u << 20)) text.append(buf, n);
        fclose(fp);
        if (text.find("CustomResourceDefinition") == std::string::npos) continue;
        std::vector<lintp::DocTree> docs;
        lintp::sv sv_text(text);
        lintp::build_docs(sv_text, sv_text, docs);
        for (lintp::DocTree &d : docs) lintp::crd_from_doc(d.t, found);
    }
    lintp::CrdIndex &ix = lintp::crd_index();
    std::lock_guard<std::mutex> g(ix.mu);
    ix.by_key = std::move(found);
    ix.built = true;
}
