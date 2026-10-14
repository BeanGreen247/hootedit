#include "../src/merge.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int main() {
    MergeView v;
    CHECK(!merge_parse("a\nb\n", v));
    CHECK(merge_parse("a\n<<<<<<< HEAD\nx\n=======\ny\n>>>>>>> feat\nz\n", v));
    CHECK(v.nconf == 1 && v.segs.size() == 3 && v.cur_label == "HEAD" && v.inc_label == "feat");
    int un = 0;
    CHECK(merge_result_lines(v, &un).size() == 2 && un == 1);
    v.segs[1].choice = 1;
    CHECK(merge_result_text(v) == "a\nx\nz\n");
    v.segs[1].choice = 2;
    CHECK(merge_result_text(v) == "a\ny\nz\n");
    v.segs[1].choice = 3;
    CHECK(merge_result_text(v) == "a\nx\ny\nz\n");
    v.segs[1].choice = 4;
    CHECK(merge_result_text(v) == "a\ny\nx\nz\n");
    /* diff3 style with a base, two conflicts, CRLF, no final newline */
    CHECK(merge_parse("<<<<<<< a\r\n1\r\n||||||| base\r\n0\r\n=======\r\n2\r\n>>>>>>> b\r\nmid\r\n<<<<<<< a\r\n\r\n=======\r\nq\r\n>>>>>>> b", v));
    CHECK(v.nconf == 2 && v.crlf && !v.final_nl && v.segs[0].has_base && v.segs[0].base.size() == 1);
    v.segs[0].choice = 5; v.segs[2].choice = 3;
    CHECK(merge_result_text(v) == "0\r\nmid\r\n\r\nq");
    /* unfinished block: not parsed */
    CHECK(!merge_parse("a\n<<<<<<< x\nb\n=======\nc\n", v));
    /* layout: a long common run is cut to its edges, conflicts are indexed, the result lists unresolved ones */
    std::string big = "<<<<<<< a\nx\n=======\ny\n>>>>>>> b\n";
    for (int i = 0; i < 20; i++) big += "c" + std::to_string(i) + "\n";
    big += "<<<<<<< a\n=======\nz\n>>>>>>> b\n";
    CHECK(merge_parse(big, v));
    merge_layout(v);
    CHECK(v.conf_seg.size() == 2 && v.unresolved == 2);
    int elided = 0, plain = 0;
    for (const MRow &r : v.rows) { if (r.kind == 3) elided += r.a; if (r.kind == 0) plain++; }
    CHECK(plain == 6 && elided == 14);
    v.segs[v.conf_seg[0]].choice = 2; v.dirty = true; merge_layout(v);
    CHECK(v.unresolved == 1);
    puts("merge ok");
    return 0;
}
