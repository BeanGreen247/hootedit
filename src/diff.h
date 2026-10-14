/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_DIFF_H
#define HE_DIFF_H
#include <stdint.h>
#include <string>
#include <vector>

/* Line diff (Myers, O(ND)) between two texts, and the aligned rows a side-by-side or inline view draws. */
struct DiffHunk { int a0, an, b0, bn; };           /* old lines [a0, a0+an) became new lines [b0, b0+bn); a pure add has an == 0, a pure delete bn == 0 */

enum DiffKind : uint8_t { DK_SAME, DK_MOD, DK_DEL, DK_ADD };
struct DiffRow {
    int a, b;                                      /* line index on each side, -1 where that side has no line (the hatched filler) */
    DiffKind kind;
    int hunk;                                      /* index into hunks, -1 for unchanged rows */
    int ia0, ia1, ib0, ib1;                        /* DK_MOD only: the byte range inside each line that differs, for the stronger highlight */
};

struct DiffModel {
    std::vector<std::string> a, b;                 /* the lines of each side, without line breaks */
    std::vector<DiffHunk> hunks;
    std::vector<DiffRow> rows;
    bool exact = true;                             /* false when the edit script was too long and the middle was treated as one replace */
};

void diff_split_lines(const std::string &text, std::vector<std::string> &out);
/* ids: equal lines have equal ids. max_d bounds the work; past it the differing middle becomes a single hunk and *exact is false. */
std::vector<DiffHunk> diff_ids(const std::vector<int> &a, const std::vector<int> &b, int max_d, bool *exact);
void diff_build(DiffModel &m, const std::string &old_text, const std::string &new_text);
#endif
