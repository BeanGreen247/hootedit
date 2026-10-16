/* bench_search ROOT PATTERN [regex] [i]: lists ROOT's files, runs our parallel search, prints hits and timing. Compare with grep -rn. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <chrono>
#include <thread>
#include "../src/search.h"

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bench_search ROOT PATTERN [regex] [i]\n"); return 2; }
    bool rx = false, ic = false;
    for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "regex")) rx = true; if (!strcmp(argv[i], "i")) ic = true; }
    double t0 = now();
    auto files = std::make_shared<std::vector<std::string>>(list_workspace_files(argv[1], false));
    double t1 = now();
    SearchQuery q;
    q.text = argv[2];
    q.regex = rx;
    q.casing = ic ? SearchQuery::INSENSITIVE : SearchQuery::SENSITIVE;
    q.max_results = 1u << 30;
    Searcher s;
    double t2 = now();
    s.start(files, q, nullptr);
    while (!s.stats().done) std::this_thread::sleep_for(std::chrono::microseconds(200));
    double t3 = now();
    SearchStats st = s.stats();
    printf("%zu files listed in %.1f ms; search: %llu hits in %llu files, %.0f MB scanned, %.1f ms wall (%s)\n", files->size(), (t1 - t0) * 1e3, (unsigned long long)st.hits,
           (unsigned long long)st.files_with_hits, (double)st.bytes / 1e6, (t3 - t2) * 1e3, st.error.empty() ? "ok" : st.error.c_str());
    return 0;
}
