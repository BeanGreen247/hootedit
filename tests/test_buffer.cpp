/* Differential test: a random edit sequence applied to Buffer and to a plain std::string must always agree, including the line index and flat().
 * A file changed or truncated by another program while it is open must neither change our text nor kill us. */
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include "../src/buffer.h"

static unsigned long long s = 88172645463325252ull;
static unsigned rnd() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)(s >> 11); }

static ::testing::AssertionResult agree(const Buffer &b, const std::string &ref, int step) {
    std::string all;
    b.copy(0, b.size(), all);
    if (all != ref) return ::testing::AssertionFailure() << "step " << step << ": content differs (" << all.size() << " vs " << ref.size() << ")";
    size_t fn;
    const char *fl = b.flat(&fn);
    if (fn != ref.size() || (fn && memcmp(fl, ref.data(), fn) != 0)) return ::testing::AssertionFailure() << "step " << step << ": flat() differs";
    size_t ln = 1;
    for (char c : ref) if (c == '\n') ln++;
    if (b.lines() != ln) return ::testing::AssertionFailure() << "step " << step << ": lines " << b.lines() << " vs " << ln;
    size_t start = 0, line = 0;
    for (size_t i = 0; i <= ref.size(); i++) {
        if (i == ref.size() || ref[i] == '\n') {
            if (b.line_start(line) != start || b.line_len(line) != i - start) return ::testing::AssertionFailure() << "step " << step << ": line " << line << " index";
            line++; start = i + 1;
        }
    }
    return ::testing::AssertionSuccess();
}

TEST(Buffer, SmallFileRewrittenInPlaceDoesNotChangeTheText) {
    char small[] = "/tmp/hootedit-ext-XXXXXX";
    int fd = mkstemp(small);
    ASSERT_EQ(write(fd, "one\ntwo\n", 8), 8);
    close(fd);
    {
        Buffer b;
        ASSERT_TRUE(b.open(small));
        FILE *f = fopen(small, "w");   /* in place, truncating: what a shell redirect does */
        fputs("rewritten completely\n", f);
        fclose(f);
        std::string t;
        b.copy(0, b.size(), t);
        EXPECT_EQ(t, "one\ntwo\n");
    }
    unlink(small);
}

TEST(Buffer, MappedFileTruncatedByAnotherProgramDoesNotKillUs) {
    char big[] = "/tmp/hootedit-extbig-XXXXXX";
    int fd = mkstemp(big);
    std::string line(1023, 'x');
    line += '\n';
    for (int i = 0; i < 20000; i++) ASSERT_GE(write(fd, line.data(), line.size()), 0);   /* about 20 MB: mapped, not read */
    close(fd);
    {
        Buffer b;
        ASSERT_TRUE(b.open(big));
        ASSERT_EQ(truncate(big, 0), 0);
        size_t sum = 0;
        std::string t;
        for (size_t l = 0; l < b.lines(); l += 997) { b.line_text(l, t); sum += t.size(); }   /* reads pages that no longer exist */
        (void)sum;
    }
    unlink(big);
}

TEST(Buffer, RandomEditsAgreeWithAStdString) {
    char gen[] = "/tmp/hootedit-test-XXXXXX";   /* 60000 lines: crosses many line-index chunks, no big file in the repo */
    int fd = mkstemp(gen);
    FILE *f = fdopen(fd, "w");
    for (int i = 0; i < 60000; i++) fprintf(f, "line %d some text here\n", i);
    fclose(f);
    int steps = 0;
    for (int round = 0; round < 40; round++) {
        std::string ref;
        Buffer b;
        bool from_file = round % 2 == 0 && round < 6;
        if (from_file) {
            ASSERT_TRUE(b.open(gen));
            b.copy(0, b.size(), ref);
        }
        for (int i = 0; i < (from_file ? 800 : 400); i++, steps++) {
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
            if (ref.size() < 4096 || steps % 61 == 0) ASSERT_TRUE(agree(b, ref, steps));
            else {   /* big file: a full check is O(n), spot-check a few lines every step */
                for (int q = 0; q < 3; q++) {
                    size_t line = rnd() % b.lines(), st = 0, k = 0;
                    for (size_t j = 0; j < ref.size() && k < line; j++) if (ref[j] == '\n') { k++; st = j + 1; }
                    size_t en = ref.find('\n', st);
                    if (en == std::string::npos) en = ref.size();
                    ASSERT_TRUE(b.line_start(line) == st && b.line_len(line) == en - st && b.line_of(st) == line) << "spot check step " << steps << " line " << line;
                }
            }
        }
    }
    unlink(gen);
    EXPECT_GT(steps, 10000);
}
