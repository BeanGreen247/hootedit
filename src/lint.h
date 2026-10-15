/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_LINT_H
#define HE_LINT_H
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "syntax.h"

/* Built-in checks for common mistakes: one linear pass over the text, no regex engine, no subprocess. It covers Ansible playbooks
 * (the rules of ansible-lint that can be decided from the text), YAML, JSON, shell, Python, C and C++, JavaScript and TypeScript,
 * Dockerfile and Makefile, plus generic ones (unbalanced brackets, merge conflict markers, committed secrets, trailing whitespace).
 * A diagnostic can carry a fix (a replacement inside one line). "# noqa" or "# noqa: code" on a line silences it. */
enum { SEV_ERROR = 0, SEV_WARN = 1, SEV_INFO = 2, SEV_HINT = 3 };

struct Diag {
    uint32_t line = 0;              /* 0-based */
    uint32_t c0 = 0, c1 = 0;        /* byte columns in the line, [c0, c1) */
    uint8_t sev = SEV_WARN;
    std::string code, msg;
    bool has_fix = false;
    uint32_t f0 = 0, f1 = 0;        /* the fix replaces bytes [f0, f1) of the line with fix */
    std::string fix;
};

struct LintInput {
    const char *text = nullptr;
    size_t n = 0;
    const Lang *lang = nullptr;
    std::string path;               /* for file-name based detection (roles/, tasks/, Dockerfile, .json) */
};

void lint_run(const LintInput &in, std::vector<Diag> &out, size_t max = 5000);
bool lint_is_ansible(const std::string &path, const char *text, size_t n);   /* playbook, tasks file, handlers, role vars... */

/* Task skeletons for "Ansible: Insert Task": name, text with "- " first; every line after the first is indented by the caller */
struct AnsibleTemplate { const char *label; const char *text; };
const std::vector<AnsibleTemplate> &ansible_templates();
#endif
