/* Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <string.h>
#include <string>

/* The link around byte `at` of `line`, or "". Scans for a scheme, extends to the next space/quote/angle bracket and trims trailing punctuation. */
inline std::string link_at(const std::string &line, size_t at) {
    static const char *schemes[] = {"https://", "http://", "ftp://", "file://", "mailto:"};
    for (const char *sc : schemes) {
        size_t sl = strlen(sc), from = 0;
        while ((from = line.find(sc, from)) != std::string::npos) {
            size_t e = from + sl;
            while (e < line.size() && (unsigned char)line[e] > 32 && !strchr("\"'<>`", line[e])) e++;
            while (e > from + sl && strchr(".,;:!?)]}", line[e - 1])) e--;
            if (e > from + sl && at >= from && at < e) return line.substr(from, e - from);
            from = e > from ? e : from + sl;
        }
    }
    return "";
}
