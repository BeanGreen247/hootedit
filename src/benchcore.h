/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_BENCHCORE_H
#define HE_BENCHCORE_H
/* hootedit --bench-core FILE [REPS] [DIR]: the CPU-bound paths without a window (open + line index, lexer, regex scan, fuzzy, outline,
 * edits, undo, workspace search over DIR). Prints one line of best-of-REPS times. The PGO training run uses it too. */
int bench_core(const char *file, int reps, const char *dir);
#endif
