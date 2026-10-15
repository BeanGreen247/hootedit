/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "benchcore.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "doc.h"
#include "fuzzy.h"
#include "outline.h"
#include "regex.h"
#include "search.h"

namespace {
double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
}  // namespace

int bench_core(const char *file, int reps, const char *dir) {
    enum { OPEN, LEX, SCAN_LIT, SCAN_RX, FUZZY, OUTLINE, EDIT, UNDO, SEARCH, N };
    const char *names[N] = {"open", "lex", "scan-literal", "scan-regex", "fuzzy", "outline", "edits", "undo", "dir-search"};
    double best[N];
    for (double &b : best) b = 1e9;
    unsigned long long sink = 0;
    for (int r = 0; r < reps; r++) {
        double t[N + 1];
        double t0 = now();
        Doc d;
        if (!d.open(file)) { fprintf(stderr, "bench-core: cannot open %s\n", file); return 1; }
        d.redetect();
        t[OPEN] = now() - t0;

        size_t fn;
        const char *fb = d.buf.flat(&fn);
        t0 = now();
        {
            std::vector<uint8_t> tok(4096);
            uint8_t st = 0;
            for (size_t p = 0; p < fn;) {
                const char *nl = static_cast<const char *>(memchr(fb + p, '\n', fn - p));
                size_t e = nl ? (size_t)(nl - fb) : fn;
                if (e - p > tok.size()) tok.resize(e - p);
                st = lex_line(d.lang, fb + p, e - p, st, tok.data());
                sink += tok[0];
                p = e + 1;
            }
        }
        t[LEX] = now() - t0;

        std::string err;
        t0 = now();
        {
            Regex re;
            re.compile("static inline", false, &err);
            Matcher m(re);
            m.scan(fb, fn, [&](size_t, size_t) { sink++; return true; });
        }
        t[SCAN_LIT] = now() - t0;
        t0 = now();
        {
            Regex re;
            re.compile("\\b(static|inline)\\s+\\w+\\s*\\(", true, &err);
            Matcher m(re);
            m.scan(fb, fn, [&](size_t ls, size_t le) { size_t ms, me; if (m.find(fb + ls, le - ls, 0, &ms, &me)) sink += ms; return true; });
        }
        t[SCAN_RX] = now() - t0;

        t0 = now();
        {
            std::vector<std::string> names_v;
            std::string line;
            for (size_t l = 0; l < d.buf.lines() && names_v.size() < 40000; l += 5) { d.buf.line_text(l, line); names_v.push_back(line.substr(0, 60)); }
            std::vector<int> ps;
            for (const char *q : {"stdio", "sbuf", "^#inc", "!static fn"}) {
                FuzzyQuery fq;
                fq.parse(q);
                for (const std::string &nm : names_v) { int sc; if (fq.match(nm.data(), nm.size(), &sc, &ps)) sink += (unsigned)sc; }
            }
        }
        t[FUZZY] = now() - t0;

        t0 = now();
        { std::vector<OItem> o; outline_scan(d, o); sink += o.size(); }
        t[OUTLINE] = now() - t0;

        t0 = now();
        {
            unsigned long long s = 88172645463325252ull;
            for (int i = 0; i < 20000; i++) {
                s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                size_t pos = (size_t)(s % (d.buf.size() + 1));
                d.cur = d.anchor = pos;
                if (i % 3 == 2) d.erase_back(); else d.insert_text("ab", 2);
                if (i % 50 == 0) d.break_group();
            }
        }
        t[EDIT] = now() - t0;
        t0 = now();
        { int n = 0; while (d.undo() && n++ < 20000) {} while (d.redo() && n-- > 0) {} }
        t[UNDO] = now() - t0;

        t0 = now();
        if (dir) {
            auto files = std::make_shared<std::vector<std::string>>(list_workspace_files(dir, false));
            for (int k = 0; k < 2; k++) {
                SearchQuery q;
                q.text = k ? "\\bstatic\\s+\\w+" : "static inline";
                q.regex = k == 1;
                q.casing = SearchQuery::SENSITIVE;
                q.max_results = 1u << 30;
                Searcher s;
                s.start(files, q, nullptr);
                while (!s.stats().done) std::this_thread::sleep_for(std::chrono::microseconds(200));
                sink += s.stats().hits;
            }
        }
        t[SEARCH] = now() - t0;
        for (int i = 0; i < N; i++) if (t[i] < best[i]) best[i] = t[i];
    }
    double total = 0;
    for (int i = 0; i < N; i++) { printf("%s %.1f ms | ", names[i], best[i] * 1e3); total += best[i]; }
    printf("total %.1f ms (%llu)\n", total * 1e3, sink & 1);
    return 0;
}
