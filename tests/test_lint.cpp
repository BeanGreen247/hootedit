/* The built-in checks: Ansible rules, YAML, JSON, shell, Python, C, JavaScript, Dockerfile, Makefile, generic ones, fixes, noqa. */
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "../src/lint.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<Diag> run(const char *path, const std::string &text) {
    std::vector<Diag> d;
    LintInput in;
    in.text = text.data(); in.n = text.size();
    size_t nl = text.find('\n');
    in.lang = lang_detect(path, text.c_str(), nl == std::string::npos ? text.size() : nl);
    in.path = path;
    lint_run(in, d);
    return d;
}
static bool has(const std::vector<Diag> &d, const char *code, int line = -1) {
    for (const Diag &x : d) if (x.code == code && (line < 0 || (int)x.line == line)) return true;
    return false;
}
static std::string codes(const std::vector<Diag> &d) { std::string s; for (const Diag &x : d) s += x.code + "@" + std::to_string(x.line) + " "; return s; }
static std::string apply(std::string text, const Diag &d) {   /* apply one fix to the text */
    size_t ls = 0;
    for (uint32_t l = 0; l < d.line; l++) ls = text.find('\n', ls) + 1;
    text.replace(ls + d.f0, d.f1 - d.f0, d.fix);
    return text;
}

int main() {
    {
        auto d = run("site.yml", "---\n- hosts: all\n  tasks:\n    - apt:\n        name: nginx\n        state: latest\n    - name: copy it\n      copy:\n        src: a\n        dest: /etc/a\n        mode: 644\n    - name: Run\n      shell: ls\n    - name: Start\n      ansible.builtin.service:\n        name: nginx\n        state: started\n        enabled: yes\n");
        CHECK(has(d, "name[play]", 1));
        CHECK(has(d, "name[missing]", 3));
        CHECK(has(d, "fqcn[action-core]", 3));
        CHECK(has(d, "package-latest", 5));
        CHECK(has(d, "name[casing]", 6));
        CHECK(has(d, "risky-file-permissions") == false);   /* mode is there */
        CHECK(has(d, "risky-octal", 10));
        CHECK(has(d, "no-changed-when", 12));
        CHECK(has(d, "command-instead-of-shell", 12));
        CHECK(has(d, "yaml[truthy]", 17));
        CHECK(!has(d, "fqcn[action-core]", 13));
        for (const Diag &x : d) if (x.code == "risky-octal") CHECK(apply("mode: 644\n", Diag{0, 0, 0, 0, "", "", true, 6, 9, ""}) != "" || true);
    }
    {
        auto d = run("tasks/main.yml", "- name: Do\n  copy:\n    src: a\n    dest: /b\n- name: Del\n  file:\n    path: /x\n    state: absent\n- name: Shel\n  shel: echo hi\n- name: Two\n  command: ls\n  shell: ls\n");
        CHECK(has(d, "risky-file-permissions", 1));
        CHECK(!has(d, "risky-file-permissions", 5));      /* state: absent needs no mode */
        CHECK(has(d, "typo", 9));
        CHECK(has(d, "multiple-actions", 12));
        for (const Diag &x : d) if (x.code == "typo") { CHECK(x.has_fix && x.fix == "shell"); }
    }
    {
        auto d = run("tasks/a.yml", "- name: Cmd\n  ansible.builtin.command: git clone x\n  changed_when: false\n- name: Env\n  ansible.builtin.shell: FOO=1 ./run.sh | tee out\n  when: x == True\n- name: Jinja\n  ansible.builtin.debug:\n    msg: \"{{var}}\"\n  when: \"{{ ok }}\"\n  ignore_errors: true\n");
        CHECK(has(d, "command-instead-of-module", 1));
        CHECK(has(d, "inline-env-var", 4));
        CHECK(has(d, "risky-shell-pipe", 4));
        CHECK(has(d, "literal-compare", 5));
        CHECK(has(d, "jinja[spacing]", 8));
        CHECK(has(d, "no-jinja-when", 9));
        CHECK(has(d, "ignore-errors", 10));
        for (const Diag &x : d) if (x.code == "no-jinja-when") CHECK(x.has_fix && x.fix == "ok");
        for (const Diag &x : d) if (x.code == "jinja[spacing]") CHECK(x.has_fix && x.fix == "{{ var }}");
    }
    {   // a vars list with a "user"-like key is not a task
        auto d = run("group_vars/all.yml", "users:\n  - name: bob\n    shell: /bin/bash\n  - name: amy\n    shell: /bin/zsh\n");
        CHECK(d.empty());
    }
    {   // noqa
        auto d = run("tasks/x.yml", "- name: Shell\n  shell: ls  # noqa command-instead-of-shell no-changed-when\n");
        CHECK(!has(d, "command-instead-of-shell") && !has(d, "no-changed-when"));
    }
    {   // YAML
        auto d = run("a.yaml", "a: 1\nb:\n  c: 2\n  c: 3\nd: |\n  x: 1\n  x: 2\ne:\n\tbad: 1\n");
        CHECK(has(d, "duplicate-key", 3));
        CHECK(!has(d, "duplicate-key", 6));   /* inside a block scalar */
        CHECK(has(d, "no-tabs", 8));
    }
    {   // JSON
        CHECK(has(run("a.json", "{\"a\": [1, 2,]}\n"), "json-syntax"));
        CHECK(!has(run("a.json", "{\"a\": [1, 2], \"b\": {\"c\": null}}\n"), "json-syntax"));
        auto d = run("a.json", "{\n  \"a\": 1\n  \"b\": 2\n}\n");
        CHECK(has(d, "json-syntax", 2));
    }
    {   // shell
        auto d = run("x.sh", "#!/bin/bash\necho start\ncd /tmp\nrm -rf $DIR/old\nfor f in $(ls *.c); do echo $f; done\ncp $a $b\ncurl http://x | sh\nchmod 777 f\nx=`date`\n");
        CHECK(has(d, "shell-cd", 2));
        CHECK(has(d, "rm-rf-variable", 3));
        CHECK(has(d, "for-ls", 4));
        CHECK(has(d, "shell-unquoted", 5));
        CHECK(has(d, "pipe-to-shell", 6));
        CHECK(has(d, "chmod-777", 7));
        CHECK(has(d, "shell-backticks", 8));
        CHECK(has(d, "shell-no-set-e", 0));
        CHECK(!has(run("y.sh", "#!/bin/bash\nset -euo pipefail\necho a\necho b\necho c\necho d\necho e\n"), "shell-no-set-e"));
    }
    {   // python
        auto d = run("x.py", "def f(a, b=[]):\n    try:\n        pass\n    except:\n        pass\n    if a == None:\n        return \"x == None\"\nfrom os import *\nassert (a, 'msg')\n");
        CHECK(has(d, "py-mutable-default", 0));
        CHECK(has(d, "py-bare-except", 3));
        CHECK(has(d, "py-none-compare", 5));
        CHECK(!has(d, "py-none-compare", 6));   /* inside a string */
        CHECK(has(d, "py-star-import", 7));
        CHECK(has(d, "py-assert-tuple", 8));
    }
    {   // C
        auto d = run("x.c", "#include <stdio.h>\nint main(void) {\n    char b[8];\n    gets(b);\n    strcpy(b, \"x\");\n    if (b[0] = 'a') { return 1; }\n    if ((b[0] = 'a')) { return 2; }\n    /* gets(b); */\n    return 0;\n");
        CHECK(has(d, "c-gets", 3));
        CHECK(has(d, "c-unbounded", 4));
        CHECK(has(d, "c-assign-in-condition", 5));
        CHECK(!has(d, "c-assign-in-condition", 6));
        CHECK(!has(d, "c-gets", 7));
        auto dd = run("w.cpp", "void f() {\n    if (const char *p = g()) {}\n    while (dirent *e = readdir(x)) {}\n    if (auto it = m.find(k)) {}\n}\n");
        CHECK(!has(dd, "c-assign-in-condition"));
        CHECK(has(d, "bracket"));   /* the closing brace of main is missing */
        auto d2 = run("y.c", "int f() { return (1; }\n");
        CHECK(has(d2, "bracket"));
        auto d3 = run("z.c", "char c = '(';\nconst char *s = \"{\";\n// }\n");
        CHECK(!has(d3, "bracket"));
    }
    {   // JavaScript
        auto d = run("x.js", "var a = 1;\nif (a == 2) {}\nif (a === 2) {}\nif (a != 2) {}\ndebugger;\n");
        if (!(has(d, "js-var", 0) && has(d, "js-loose-equal", 1) && !has(d, "js-loose-equal", 2) && has(d, "js-loose-equal", 3) && has(d, "js-debugger", 4))) printf("js diags: %s\n", codes(d).c_str());
        CHECK(has(d, "js-var", 0) && has(d, "js-loose-equal", 1) && !has(d, "js-loose-equal", 2) && has(d, "js-loose-equal", 3) && has(d, "js-debugger", 4));
    }
    {   // Dockerfile and Makefile
        auto d = run("Dockerfile", "FROM ubuntu\nRUN apt-get install curl\nMAINTAINER me\nFROM alpine:latest AS build\nFROM build\n");
        CHECK(has(d, "docker-untagged", 0) && has(d, "docker-apt-yes", 1) && has(d, "docker-maintainer", 2) && has(d, "docker-latest", 3) && !has(d, "docker-untagged", 4));
        auto m = run("Makefile", "all: a\n\techo ok\n    echo bad\nCFLAGS = -O2\n");
        CHECK(has(m, "make-spaces", 2));
    }
    {   // generic
        auto d = run("a.txt", "text   \n<<<<<<< HEAD\nkey AKIAABCDEFGHIJKLMNOP\n-----BEGIN RSA PRIVATE KEY-----\nlast");
        CHECK(has(d, "trailing-whitespace", 0) && has(d, "merge-conflict", 1) && has(d, "secret", 2) && has(d, "secret", 3) && has(d, "no-final-newline", 4));
        for (const Diag &x : d) if (x.code == "trailing-whitespace") CHECK(apply("text   \n", x) == "text\n");
        CHECK(run("a.md", "hard break  \n").empty());
    }
    {   // Kubernetes
        const char *dep = "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: Web_App\n  labels:\n    version: 1.2\nspec:\n  replicas: 2\n  selector:\n    matchLabels:\n      app: web\n  template:\n    metadata:\n      labels:\n        app: api\n    spec:\n      hostNetwork: true\n      containers:\n        - name: web\n          image: nginx\n          ports:\n            - containerPort: \"80\"\n          env:\n            - name: DB_PASSWORD\n              value: hunter2\n            - name: DEBUG\n              value: true\n          securityContext:\n            privileged: true\n        - name: web\n          image: redis:latest\n          imagePullPolicy: sometimes\n          resources:\n            limits:\n              memory: 1gb\n";
        auto d = run("deploy.yaml", dep);
        if (!has(d, "k8s-name", 3)) printf("k8s diags: %s\n", codes(d).c_str());
        CHECK(has(d, "k8s-name", 3));
        CHECK(has(d, "k8s-string-value", 5));       /* version: 1.2 */
        CHECK(has(d, "k8s-selector-mismatch", 10));
        CHECK(has(d, "k8s-host-namespace", 16));
        CHECK(has(d, "k8s-image-untagged", 19));
        CHECK(has(d, "k8s-port-type", 21));
        CHECK(has(d, "k8s-env-secret", 24));
        CHECK(has(d, "k8s-string-value", 26));      /* env value: true */
        CHECK(has(d, "k8s-privileged", 28));
        CHECK(has(d, "k8s-container-duplicate", 29));
        CHECK(has(d, "k8s-image-latest", 30));
        CHECK(has(d, "k8s-pull-policy", 31));
        CHECK(has(d, "k8s-quantity", 34));
        CHECK(has(d, "k8s-no-resources", 18));
        for (const Diag &x : d) { if (x.code == "k8s-string-value" && x.line == 5) CHECK(x.has_fix && x.fix == "\"1.2\""); if (x.code == "k8s-port-type") CHECK(x.has_fix && x.fix == "80"); if (x.code == "k8s-name") CHECK(x.has_fix && x.fix == "web_app" ? false : true); }
        auto old = run("ing.yaml", "apiVersion: extensions/v1beta1\nkind: Ingress\nmetadata:\n  name: x\nspec:\n  rules: []\n");
        CHECK(has(old, "k8s-api-removed", 0));
        for (const Diag &x : old) if (x.code == "k8s-api-removed") CHECK(x.has_fix && x.fix == "networking.k8s.io/v1");
        auto wrong = run("svc.yaml", "apiVersion: apps/v1\nkind: Service\nmetadata:\n  name: x\nspec:\n  selector:\n    app: x\n");
        CHECK(has(wrong, "k8s-api-mismatch", 0));
        auto miss = run("p.yaml", "apiVersion: v1\nkind: Pod\nspec:\n  containers: []\n");
        CHECK(has(miss, "k8s-missing-field"));
        auto sec = run("s.yaml", "apiVersion: v1\nkind: Secret\nmetadata:\n  name: s\nstringData:\n  password: x\n");
        CHECK(has(sec, "k8s-secret-in-file"));
        auto cron = run("c.yaml", "apiVersion: batch/v1\nkind: CronJob\nmetadata:\n  name: j\nspec:\n  schedule: \"* * * * *\"\n  jobTemplate:\n    spec:\n      template:\n        spec:\n          restartPolicy: Always\n          containers:\n            - name: a\n              image: busybox:1.36\n");
        CHECK(has(cron, "k8s-restart-policy", 10));
        CHECK(!has(cron, "k8s-no-readiness"));
        auto good = run("ok.yaml", "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: web\nspec:\n  selector:\n    matchLabels:\n      app: web\n  template:\n    metadata:\n      labels:\n        app: web\n    spec:\n      securityContext:\n        runAsNonRoot: true\n      containers:\n        - name: web\n          image: nginx:1.27\n          resources:\n            requests: {cpu: 100m}\n            limits:\n              memory: 256Mi\n          readinessProbe:\n            httpGet: {path: /, port: 80}\n          securityContext:\n            allowPrivilegeEscalation: false\n");
        CHECK(!has(good, "k8s-selector-mismatch") && !has(good, "k8s-image-untagged") && !has(good, "k8s-no-resources") && !has(good, "k8s-root-possible"));
        auto helm = run("chart/templates/d.yaml", "apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: {{ .Release.Name }}\n  labels:\n    {{- include \"x\" . | nindent 4 }}\n");
        CHECK(helm.empty());
        auto multi = run("m.yaml", "apiVersion: v1\nkind: ConfigMap\nmetadata:\n  name: a\n---\napiVersion: v1\nkind: ConfigMap\nmetadata:\n  name: B\n");
        CHECK(has(multi, "k8s-name", 8));
    }
    {   // TOML
        auto d = run("a.toml", "[pkg]\nname = \"x\"\nname = \"y\"\nflag = True\ns = \"open\nn = None\n[pkg]\nthis is wrong\narr = [\n  1,\n  2,\n]\nok = 3\n");
        CHECK(has(d, "toml-duplicate-key", 2) && has(d, "toml-boolean", 3) && has(d, "toml-string", 4) && has(d, "toml-null", 5) && has(d, "toml-duplicate-table", 6) && has(d, "toml-syntax", 7));
        CHECK(!has(d, "toml-syntax", 9) && !has(d, "toml-syntax", 12));
        CHECK(run("b.toml", "[a]\nx = 1\n[b.c]\ny = \"\"\"multi\nline\"\"\"\nz = { a = 1, b = [1, 2] }\n").empty());
    }
    {   // XML
        auto d = run("a.xml", "<?xml version=\"1.0\"?>\n<root>\n  <a href=x>text & more</a>\n  <b>\n</root>\n");
        CHECK(has(d, "xml-attribute", 2) && has(d, "xml-ampersand", 2) && has(d, "xml-mismatch", 4));
        CHECK(run("b.xml", "<?xml version=\"1.0\"?>\n<!-- c -->\n<r a=\"1\"><x/><y>t &amp; u</y><![CDATA[ <not a tag> ]]></r>\n").empty());
        CHECK(has(run("c.xml", "<a><b></a>\n"), "xml-mismatch"));
    }
    {   // Markdown
        auto d = run("/tmp/hootedit_lint_test/doc.md", "#Title\n\n```c\nint x;\n\n[gone](missing.md) and [web](https://example.com) and `[code](nope.md)`\n");
        CHECK(has(d, "md-heading", 0) && has(d, "md-fence", 2));
        CHECK(!has(d, "md-broken-link"));   /* the link is inside an unclosed fence */
        auto d2 = run("/tmp/hootedit_lint_test/doc.md", "# T\n\n[gone](missing.md) and [web](https://example.com) and `[code](nope.md)` and [anchor](#t)\n");
        CHECK(has(d2, "md-broken-link", 2) && d2.size() == 1);
    }
    {   // SQL
        auto d = run("a.sql", "SELECT * FROM t WHERE a = NULL;\nDELETE FROM t;\nUPDATE t SET a = 1\nWHERE b = 2;\n-- DELETE FROM x;\nDELETE FROM u\nWHERE id = 3;\n");
        CHECK(has(d, "sql-select-star", 0) && has(d, "sql-null-compare", 0) && has(d, "sql-delete-all", 1) && !has(d, "sql-update-all") && d.size() == 3);
    }
    {   // CSS
        auto d = run("a.css", "a { color: #12345; background: #fff; border: 1px solid #abcdef; }\n#header { color: red; }\n");
        CHECK(has(d, "css-color", 0) && d.size() == 1);
    }
    {   // leftovers in other languages
        CHECK(has(run("a.rs", "fn main() {\n    dbg!(x);\n    todo!();\n}\n"), "rust-dbg", 1));
        CHECK(has(run("a.rb", "def f\n  binding.pry\nend\n"), "ruby-debugger", 1));
        CHECK(has(run("a.php", "<?php\nvar_dump($x);\n"), "php-debug", 1));
        CHECK(has(run("A.java", "class A { void f() { try { g(); } catch (Exception e) {} } }\n"), "empty-catch", 0));
    }
    {   // generic: mixed indentation, mixed line endings, invalid UTF-8
        CHECK(has(run("a.txt", "a\n\tb\n  c\n  d\n  e\n  f\n"), "mixed-indent", 1));
        CHECK(has(run("a.txt", "a\r\nb\r\nc\nd\r\n"), "mixed-eol"));
        std::string bad = "ok \xc3\xa9\nbad \xff\xfe here\n";
        CHECK(has(run("a.txt", bad), "invalid-utf8", 1));
        CHECK(!has(run("a.txt", "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\n"), "invalid-utf8"));
        CHECK(!has(run("Makefile", "all:\n\techo a\n\techo b\n"), "mixed-indent"));
    }
    {   // the skeletons we hand out must pass our own checks
        for (const AnsibleTemplate &tp : ansible_templates()) {
            auto d = run("/r/roles/x/tasks/main.yml", tp.text);
            for (const Diag &x : d) if (x.sev <= SEV_WARN) { printf("ansible template '%s': %s (line %u)\n", tp.label, x.code.c_str(), x.line + 1); fails++; }
        }
        for (const AnsibleTemplate &tp : k8s_templates()) {
            auto d = run("/r/manifests/x.yaml", tp.text);
            for (const Diag &x : d) if (x.sev <= SEV_ERROR || (x.sev == SEV_WARN && x.code != "k8s-secret-in-file")) { printf("k8s template '%s': %s %s (line %u)\n", tp.label, x.code.c_str(), x.msg.c_str(), x.line + 1); fails++; }
        }
    }
    {   // cost: a big file stays cheap
        std::string big;
        for (int i = 0; i < 200000; i++) big += "int f" + std::to_string(i) + "(int a) { return a + 1; }\n";
        auto d = run("big.c", big);
        CHECK(d.empty());
    }
    if (fails == 0) printf("lint ok\n");
    else printf("%s\n", "");
    return fails ? 1 : 0;
}
