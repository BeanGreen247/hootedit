/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_OUTLINE_H
#define HE_OUTLINE_H
#include <stddef.h>
#include <string>
#include <vector>

#include "doc.h"

/* One symbol of the Explorer outline. kind: 'C' type, 'F' function, 'H' markdown heading. */
struct OItem { std::string name; size_t line; int depth; char kind; };

/* A cheap line scan, no parser: types, functions and markdown headings of one document. */
void outline_scan(const Doc &d, std::vector<OItem> &out);
#endif
