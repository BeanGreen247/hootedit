/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_SYNTAX_H
#define HE_SYNTAX_H
#include <stddef.h>
#include <stdint.h>

/* Token classes written one byte per source byte; the renderer maps a class to a colour. */
enum Tok : uint8_t { T_PLAIN, T_COMMENT, T_STRING, T_NUMBER, T_KEYWORD, T_TYPE, T_PREPROC, T_PUNCT, T_FUNC, T_KEY, T_HEADING, T_INS, T_DEL, T_COUNT };

struct Lang;

int lang_count();
const Lang *lang_at(int i);
const char *lang_name(const Lang *l);
int lang_id(const Lang *l);                                   /* stable index, 0 = Plain Text */
const Lang *lang_plain();
/* by file name (extension or exact name), then by "#!" first line; never null */
const Lang *lang_detect(const char *path, const char *first_line, size_t first_len);

/* Lex one line. `in` is the state at the start of the line (0 = normal); tok[i] gets the class of byte i.
 * Returns the state at the start of the next line (block comment, triple-quoted string, ...). A line is
 * lexed in one forward pass, no regex, no backtracking: cost is linear in the line length. */
uint8_t lex_line(const Lang *l, const char *s, size_t n, uint8_t in, uint8_t *tok);
#endif
