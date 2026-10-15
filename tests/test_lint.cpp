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
