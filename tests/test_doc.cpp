/* Undo/redo and selection behave like a plain string model; modified() tracks the saved state. */
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "../src/doc.h"

static std::string all(const Doc &d) { std::string s; d.buf.copy(0, d.buf.size(), s); return s; }

static unsigned long long s = 88172645463325252ull;
static unsigned rnd() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)(s >> 11); }

TEST(Doc, UndoRedoAndSelection) {
    Doc d;
    d.insert_text("a", 1); d.insert_text("b", 1); d.insert_text("c", 1);
    EXPECT_EQ(all(d), "abc");
    EXPECT_TRUE(d.undo());                      /* typing run is one undo step */
    EXPECT_TRUE(all(d) == "" && d.cur == 0);
    EXPECT_TRUE(d.redo());
    EXPECT_TRUE(all(d) == "abc" && d.cur == 3);
    d.break_group();
    d.erase_back(); d.erase_back();       /* backspace run is one step */
    EXPECT_EQ(all(d), "a");
    EXPECT_TRUE(d.undo());
    EXPECT_EQ(all(d), "abc");
    d.anchor = 0; d.cur = 2;              /* select "ab", type over it */
    d.insert_text("X", 1);
    EXPECT_TRUE(all(d) == "Xc" && d.cur == 1 && !d.has_sel());
    EXPECT_TRUE(d.undo());
    EXPECT_EQ(all(d), "abc");
}

TEST(Doc, SaveTracking) {
    Doc e;
    e.insert_text("hi", 2);
    EXPECT_TRUE(e.modified());
    char path[] = "/tmp/hootedit-doc-XXXXXX";
    int fd = mkstemp(path); close(fd);
    EXPECT_TRUE(e.save(path));
    EXPECT_FALSE(e.modified());
    e.insert_text("!", 1);
    EXPECT_TRUE(e.modified());
    e.undo();
    EXPECT_FALSE(e.modified());                 /* back at the saved content */
    e.insert_text("?", 1);
    e.undo();
    e.undo();
    e.insert_text("z", 1);                /* diverged below the save point: never "clean" by size alone */
    e.undo();
    EXPECT_TRUE(e.modified());
    unlink(path);
}

TEST(Doc, RandomEditsAndFullUndoRedoWalkAgainstAStringModel) {
    for (int round = 0; round < 50; round++) {
        Doc x;
        std::string model;
        std::vector<std::string> history = {""};
        for (int i = 0; i < 120; i++) {
            x.break_group();
            size_t pos = model.empty() ? 0 : rnd() % (model.size() + 1);
            size_t del = model.size() > pos ? rnd() % std::min<size_t>(4, model.size() - pos + 1) : 0;
            std::string ins;
            for (unsigned k = 0, n = rnd() % 4; k < n; k++) ins += "ab\n"[rnd() % 3];
            if (!del && ins.empty()) continue;
            x.replace(pos, del, ins.data(), ins.size());
            model.replace(pos, del, ins);
            history.push_back(model);
            ASSERT_EQ(all(x), model);
        }
        for (size_t k = history.size() - 1; k > 0; k--) { x.undo(); ASSERT_EQ(all(x), history[k - 1]); }
        for (size_t k = 1; k < history.size(); k++) { x.redo(); ASSERT_EQ(all(x), history[k]); }
    }
}

/* the column of a byte in a line (tabs to a multiple of 4, one column per UTF-8 character) against a plain reference, on lines of many sizes
 * (short ones and ones past the 256-byte stack buffer), in a document that has been edited so its text spans several pieces */
TEST(Doc, ColumnOfByteAgainstAReference) {
    Doc x;
    std::string model;
    const char *bits[] = {"a", "b", "\t", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", " ", "\n"};
    for (int i = 0; i < 400; i++) {
        std::string ins;
        for (unsigned k = 0, n = 1 + rnd() % 80; k < n; k++) ins += bits[rnd() % (rnd() % 9 == 0 ? 8 : 7)];   /* mostly no newline, so some lines pass 256 bytes */
        size_t pos = model.empty() ? 0 : rnd() % (model.size() + 1);
        while (pos < model.size() && ((unsigned char)model[pos] & 0xC0) == 0x80) pos++;   /* keep the UTF-8 characters whole */
        x.break_group();
        x.replace(pos, 0, ins.data(), ins.size());
        model.insert(pos, ins);
    }
    ASSERT_EQ(all(x), model);
    size_t ls = 0, line = 0;
    while (ls <= model.size()) {
        size_t nl = model.find('\n', ls), le = nl == std::string::npos ? model.size() : nl;
        for (size_t byte : {(size_t)0, (size_t)1, (size_t)(le - ls) / 2, (size_t)(le - ls), (size_t)(le - ls) + 5}) {
            int col = 0;
            for (size_t i = 0; i < le - ls && i < byte;) {
                unsigned char c = (unsigned char)model[ls + i];
                col = c == '\t' ? (col / 4 + 1) * 4 : col + 1;
                i += c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
            }
            ASSERT_EQ(x.col_of_byte(line, byte), col) << "line " << line << " byte " << byte;
        }
        if (nl == std::string::npos) break;
        ls = nl + 1;
        line++;
    }
}

TEST(Doc, ReloadFromDiskReplacesOnlyWhatDiffers) {
    char path[] = "/tmp/hootedit-reload-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, "one\ntwo\nthree\n", 14), 14);
    close(fd);
    Doc d;
    ASSERT_TRUE(d.open(path));
    d.cur = d.anchor = 9;   /* inside "three" */
    FILE *f = fopen(path, "w");
    fputs("one\nTWO changed\nthree\n", f);
    fclose(f);
    EXPECT_TRUE(d.disk_changed());
    EXPECT_TRUE(d.reload_disk());
    EXPECT_EQ(all(d), "one\nTWO changed\nthree\n");
    EXPECT_FALSE(d.modified());
    EXPECT_FALSE(d.disk_changed());
    EXPECT_EQ(d.cur, 9u + 8u);          /* the cursor stayed in "three": the text before it grew by 8 bytes */
    EXPECT_TRUE(d.undo());              /* one undo step brings the old text back */
    EXPECT_EQ(all(d), "one\ntwo\nthree\n");
    unlink(path);
}
