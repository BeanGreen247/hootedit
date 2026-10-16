/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_MERGE_H
#define HE_MERGE_H
#include <string>
#include <vector>

/* A file with merge conflict markers, cut into runs of common lines and conflicts, and the choices made so far.
 * The merge editor tab shows it as Incoming | Current on top and the Result below. */
struct MergeSeg {
    bool conflict = false;
    std::vector<std::string> common;                  /* a run of lines both sides agree on (conflict == false) */
    std::vector<std::string> ours, base, theirs;      /* a conflict: current, common ancestor (diff3 style only), incoming */
    bool has_base = false;
    int choice = 0;                                   /* 0 undecided, 1 current, 2 incoming, 3 both (current first), 4 both (incoming first), 5 base */
};
struct MRow { int seg = 0, kind = 0, a = -1, b = -1; };   /* top pane row: kind 0 common line (a), 1 conflict header, 2 conflict line (a: current index, b: incoming index, -1 none), 3 elided run of a lines */
struct MRes { std::string text; int kind = 0; int seg = 0; };   /* result pane row: kind 0 plain, 1 placeholder of an undecided conflict, 2 taken from a conflict */
struct MergeView {
    std::string root, path, cur_label, inc_label;
    bool crlf = false, final_nl = true;
    std::vector<MergeSeg> segs;
    int nconf = 0;
    long top = 0, rtop = 0;                           /* first visible row of the two panes */
    int cur = -1;                                     /* the conflict the buttons in the toolbar act on */
    bool dirty = true;                                /* rows and res need merge_layout */
    std::vector<MRow> rows;
    std::vector<MRes> res;
    std::vector<int> conf_seg, conf_row;              /* per conflict number: its segment and its header row */
    int unresolved = 0;
};

bool merge_parse(const std::string &text, MergeView &v);                       /* false: no complete conflict block in the text */
std::vector<std::string> merge_result_lines(const MergeView &v, int *unresolved);   /* undecided conflicts are left out and counted */
void merge_layout(MergeView &v);                                                 /* rebuild rows, res and the conflict index after a change */
std::string merge_result_text(const MergeView &v);                              /* the file as it would be written, line endings as found */
#endif
