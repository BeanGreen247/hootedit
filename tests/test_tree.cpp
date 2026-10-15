/* The Explorer tree: sorted rows, compact folders, reveal, refresh keeping folders open, ghost rows for deleted files. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include "../src/tree.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void put(const std::string &p, const char *t = "x") { FILE *f = fopen(p.c_str(), "w"); if (f) { fputs(t, f); fclose(f); } }

int main() {
    char tmpl[] = "/tmp/hootedit_tree_XXXXXX";
    if (!mkdtemp(tmpl)) return 1;
    std::string r = tmpl;
    mkdir((r + "/a").c_str(), 0755); mkdir((r + "/a/b").c_str(), 0755); mkdir((r + "/a/b/c").c_str(), 0755);
    mkdir((r + "/z").c_str(), 0755); mkdir((r + "/z/one").c_str(), 0755); mkdir((r + "/z/two").c_str(), 0755);
    mkdir((r + "/.git").c_str(), 0755);
    put(r + "/a/b/c/deep.txt"); put(r + "/z/one/f.txt"); put(r + "/B.txt"); put(r + "/a.txt");
    Tree t;
    t.set_root(r);
    const auto &rows = t.rows();
    CHECK(rows.size() == 4);                       /* a/b/c, z, a.txt, B.txt: folders first, then files by name ignoring case; .git hidden */
    CHECK(rows[0].name == "a/b/c" && rows[0].dir && rows[0].path == r + "/a/b/c");
    CHECK(rows[1].name == "z");                    /* z has two folders: not compacted */
    CHECK(rows[2].name == "a.txt" && rows[3].name == "B.txt");
    t.toggle(0);
    CHECK(t.rows().size() == 5 && t.rows()[1].name == "deep.txt" && t.rows()[1].depth == 1);
    t.toggle(0);
    CHECK(t.rows().size() == 4);
    t.reveal(r + "/z/one/f.txt");
    CHECK(t.find(r + "/z/one/f.txt") >= 0);
    CHECK(t.find(r + "/z/two") >= 0);
    put(r + "/z/one/g.txt");
    t.refresh();
    CHECK(t.find(r + "/z/one/g.txt") >= 0 && t.find(r + "/z/one/f.txt") >= 0);   /* open folders stay open */
    t.set_ghosts({r + "/z/one/gone.txt", r + "/gone2.txt", r + "/a.txt"});
    t.refresh();
    long g = t.find(r + "/z/one/gone.txt");
    CHECK(g >= 0 && t.rows()[(size_t)g].ghost && !t.rows()[(size_t)g].dir);
    long g2 = t.find(r + "/gone2.txt");
    CHECK(g2 >= 0 && t.rows()[(size_t)g2].ghost);
    long real = t.find(r + "/a.txt");
    CHECK(real >= 0 && !t.rows()[(size_t)real].ghost);   /* a ghost never hides or duplicates a real file */
    auto all = t.all_files();
    bool ghost_in_search = false;
    for (const auto &f : all) if (f.find("gone") != std::string::npos) ghost_in_search = true;
    CHECK(!ghost_in_search && all.size() == 5);
    std::string cmd = "rm -rf " + r;
    if (system(cmd.c_str()) != 0) {}
    if (fails == 0) printf("tree ok\n");
    return fails ? 1 : 0;
}
