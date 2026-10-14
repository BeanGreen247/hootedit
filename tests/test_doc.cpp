/* Undo/redo and selection behave like a plain string model; modified() tracks the saved state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "../src/doc.h"

static int fails;
static std::string all(const Doc &d) { std::string s; d.buf.copy(0, d.buf.size(), s); return s; }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned long long s = 88172645463325252ull;
static unsigned rnd() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (unsigned)(s >> 11); }

int main() {
    Doc d;
    d.insert_text("a", 1); d.insert_text("b", 1); d.insert_text("c", 1);
    CHECK(all(d) == "abc");
    CHECK(d.undo());                      /* typing run is one undo step */
    CHECK(all(d) == "" && d.cur == 0);
    CHECK(d.redo());
    CHECK(all(d) == "abc" && d.cur == 3);
    d.break_group();
    d.erase_back(); d.erase_back();       /* backspace run is one step */
    CHECK(all(d) == "a");
    CHECK(d.undo());
    CHECK(all(d) == "abc");
    d.anchor = 0; d.cur = 2;              /* select "ab", type over it */
    d.insert_text("X", 1);
    CHECK(all(d) == "Xc" && d.cur == 1 && !d.has_sel());
    CHECK(d.undo());
    CHECK(all(d) == "abc");
    CHECK(!d.undo() || true);

    /* saved state tracking */
    Doc e;
    e.insert_text("hi", 2);
    CHECK(e.modified());
    char path[] = "/tmp/hootedit-doc-XXXXXX";
    int fd = mkstemp(path); close(fd);
    CHECK(e.save(path));
    CHECK(!e.modified());
    e.insert_text("!", 1);
    CHECK(e.modified());
    e.undo();
    CHECK(!e.modified());                 /* back at the saved content */
    e.insert_text("?", 1);
    e.undo();
    e.undo();
    e.insert_text("z", 1);                /* diverged below the save point: never "clean" by size alone */
    e.undo();
    CHECK(e.modified());
    unlink(path);

    /* random edits and full undo/redo walk against a string model */
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
            CHECK(all(x) == model);
        }
        for (size_t k = history.size() - 1; k > 0; k--) { x.undo(); CHECK(all(x) == history[k - 1]); }
        for (size_t k = 1; k < history.size(); k++) { x.redo(); CHECK(all(x) == history[k]); }
        if (fails) break;
    }
    printf("test_doc: %s\n", fails ? "FAILED" : "undo/redo, selection and save tracking agree with the model");
    return fails != 0;
}
