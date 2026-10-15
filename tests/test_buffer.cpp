/* Differential test: a random edit sequence applied to Buffer and to a plain std::string must always agree,
 * including the line index. Run under ASan/UBSan (make test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include "../src/buffer.h"

static unsigned long long s = 88172645463325252ull;
static unsigned rnd() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)(s >> 11); }

static int check(const Buffer &b, const std::string &ref, const char *what, int step) {
    std::string all;
    b.copy(0, b.size(), all);
    if (all != ref) { printf("FAIL %s step %d: content differs (%zu vs %zu)\n", what, step, all.size(), ref.size()); return 1; }
    size_t fn;
    const char *fl = b.flat(&fn);
    if (fn != ref.size() || (fn && memcmp(fl, ref.data(), fn) != 0)) { printf("FAIL %s step %d: flat() differs\n", what, step); return 1; }
    size_t ln = 1;
    for (char c : ref) if (c == '\n') ln++;
    if (b.lines() != ln) { printf("FAIL %s step %d: lines %zu vs %zu\n", what, step, b.lines(), ln); return 1; }
    size_t start = 0, line = 0;
    for (size_t i = 0; i <= ref.size(); i++) {
        if (i == ref.size() || ref[i] == '\n') {
            if (b.line_start(line) != start || b.line_len(line) != i - start) { printf("FAIL %s step %d: line %zu index\n", what, step, line); return 1; }
            line++; start = i + 1;
        }
    }
    return 0;
}

/* a file rewritten or truncated by another program while it is open must neither change the text we hold (small files are read into memory)
 * nor kill us (big files are mapped; the SIGBUS guard puts zeros where the file is gone) */
static int external_change_test() {
    int bad = 0;
    char small[] = "/tmp/hootedit-ext-XXXXXX";
    int fd = mkstemp(small);
    if (write(fd, "one\ntwo\n", 8) != 8) return 1;
    close(fd);
    {
        Buffer b;
        if (!b.open(small)) return 1;
        FILE *f = fopen(small, "w");   /* in place, truncating: what a shell redirect does */
        fputs("rewritten completely\n", f);
        fclose(f);
        std::string t;
        b.copy(0, b.size(), t);
        if (t != "one\ntwo\n") { printf("FAIL: small file changed under the buffer: %s\n", t.c_str()); bad = 1; }
    }
    unlink(small);
    char big[] = "/tmp/hootedit-extbig-XXXXXX";
    fd = mkstemp(big);
    std::string line(1023, 'x');
    line += '\n';
    for (int i = 0; i < 20000; i++) if (write(fd, line.data(), line.size()) < 0) return 1;   /* about 20 MB: mapped, not read */
    close(fd);
    {
        Buffer b;
        if (!b.open(big)) return 1;
        if (truncate(big, 0) != 0) return 1;
        size_t sum = 0;
        std::string t;
        for (size_t l = 0; l < b.lines(); l += 997) { b.line_text(l, t); sum += t.size(); }   /* reads pages that no longer exist */
        (void)sum;
    }
    unlink(big);
    return bad;
}

int main(int argc, char **argv) {
    int bad = 0, steps = 0;
    if (external_change_test()) return 1;
    char gen[] = "/tmp/hootedit-test-XXXXXX";
    if (argc > 1 && !strcmp(argv[1], "--gen")) {   /* 60000 lines: crosses many line-index chunks, no big file in the repo */
        int fd = mkstemp(gen);
        FILE *f = fdopen(fd, "w");
        for (int i = 0; i < 60000; i++) fprintf(f, "line %d some text here\n", i);
        fclose(f);
        argv[1] = gen;
    }
    for (int round = 0; round < 40 && !bad; round++) {
        std::string ref;
        Buffer b;
        const char *path = argc > 1 ? argv[1] : nullptr;
        if (path && round % 2 == 0 && round < 6) {
            if (!b.open(path)) { printf("cannot open %s\n", path); return 2; }
            b.copy(0, b.size(), ref);
        }
        for (int i = 0; i < (path && round % 2 == 0 && round < 6 ? 800 : 400) && !bad; i++, steps++) {
            if (rnd() % 3 && !ref.empty() ? (rnd() % 4 == 0) : true) {
                size_t pos = ref.empty() ? 0 : rnd() % (ref.size() + 1);
                std::string t;
                for (unsigned k = 0, n = rnd() % 12 + (rnd() % 50 == 0 ? 3000 : 0); k < n; k++) t += "ab\n \t"[rnd() % 5];
                b.insert(pos, t.data(), t.size());
                ref.insert(pos, t);
            } else if (!ref.empty()) {
                size_t pos = rnd() % ref.size(), n = 1 + rnd() % (rnd() % 40 == 0 ? 20000 : 20);
                b.erase(pos, n);
                ref.erase(pos, n);
            }
            if (ref.size() < 4096 || steps % 61 == 0) bad |= check(b, ref, "random", steps);
            else if (!bad) {   /* big file: full check is O(n), spot-check a few lines every step */
                for (int q = 0; q < 3 && !bad; q++) {
                    size_t line = rnd() % b.lines(), st = 0, k = 0;
                    for (size_t j = 0; j < ref.size() && k < line; j++) if (ref[j] == '\n') { k++; st = j + 1; }
                    size_t en = ref.find('\n', st);
                    if (en == std::string::npos) en = ref.size();
                    if (b.line_start(line) != st || b.line_len(line) != en - st || b.line_of(st) != line) { printf("FAIL spot step %d line %zu\n", steps, line); bad = 1; }
                }
            }
        }
    }
    if (!strcmp(gen, argc > 1 ? argv[1] : "")) unlink(gen);
    printf("test_buffer: %d steps, %s\n", steps, bad ? "FAILED" : "all agree");
    return bad;
}
