/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "syntax.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <string>
#include <vector>

namespace {

enum Flags : unsigned {
    F_PREPROC = 1,      /* #directive at the start of a line (C family) */
    F_TRIPLE = 2,       /* """ and ''' multi-line strings (Python) */
    F_BACKTICK = 4,     /* multi-line `...` strings (JS, Go) */
    F_KEYCOLON = 8,     /* identifier followed by ':' is a key (YAML, CSS) */
    F_KEYEQ = 16,       /* first identifier on the line followed by '=' is a key (INI, TOML) */
    F_STRKEY = 32,      /* "string": is a key (JSON) */
    F_DOLLAR = 64,      /* $var and ${...} (shell, Perl, PHP, Makefile) */
    F_AT = 128,         /* @decorator / @annotation */
    F_CI = 256,         /* keywords are case-insensitive (SQL) */
    F_MARKUP = 512,     /* <tag attr="..."> */
    F_MD = 1024,        /* Markdown */
    F_DIFF = 2048,      /* unified diff */
    F_DASHIDENT = 4096, /* '-' inside identifiers (CSS, YAML, Dockerfile) */
    F_CAPTYPE = 8192,   /* Capitalised identifiers are types (Java, TS, Rust, Go, C#, Swift, Kotlin) */
};

enum State : uint8_t { S_NORMAL = 0, S_BLOCK = 1, S_TDQ = 2, S_TSQ = 3, S_BT = 4, S_TAG = 5, S_FENCE = 6 };

struct Entry { const char *w; uint8_t len, cls; };

}  // namespace

struct Lang {
    const char *name, *exts, *lc1, *lc2, *bo, *bc, *quotes;
    unsigned flags;
    const char *kw, *types;
    /* built on first use */
    bool ready;
    std::vector<Entry> table;
    size_t blen_o, blen_c, lc1l, lc2l;
    unsigned char bo0, lc10, lc20;   /* first bytes, so the comment checks cost a compare, not a memcmp call, per token */
    bool is_quote[256], is_punct[256];
};

namespace {

#define KW_C "if else for while do switch case default break continue return goto sizeof typedef struct union enum static extern const volatile register inline auto restrict"
#define TY_C "int char short long float double void unsigned signed size_t ssize_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t bool FILE NULL true false"

Lang g_langs[] = {
    {"Plain Text", "txt text log", "", "", "", "", "", 0, "", ""},
    {"C", "c", "//", "", "/*", "*/", "\"'", F_PREPROC, KW_C, TY_C},
    {"C++", "cpp cc cxx h hpp hh hxx ipp inl", "//", "", "/*", "*/", "\"'", F_PREPROC,
     KW_C " class namespace template typename public private protected virtual override final new delete this operator using try catch throw noexcept constexpr consteval constinit mutable explicit friend nullptr static_cast dynamic_cast reinterpret_cast const_cast co_await co_return co_yield concept requires decltype alignas alignof",
     TY_C " string vector map set unordered_map unordered_set array pair tuple optional variant unique_ptr shared_ptr span string_view wchar_t char8_t char16_t char32_t"},
    {"Python", "py pyw pyi", "#", "", "", "", "\"'", F_TRIPLE | F_AT,
     "and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield match case",
     "int str float bool list dict set tuple bytes object None True False self cls print len range type super"},
    {"JavaScript", "js mjs cjs jsx", "//", "", "/*", "*/", "\"'", F_BACKTICK | F_AT,
     "async await break case catch class const continue debugger default delete do else export extends finally for function if import in instanceof let new of return static super switch this throw try typeof var void while with yield get set",
     "null undefined true false NaN Infinity Object Array String Number Boolean Promise Map Set Date Math JSON console window document"},
    {"TypeScript", "ts tsx mts cts", "//", "", "/*", "*/", "\"'", F_BACKTICK | F_AT | F_CAPTYPE,
     "async await break case catch class const continue debugger default delete do else enum export extends finally for function if implements import in instanceof interface let new of private protected public readonly return static super switch this throw try type typeof var void while with yield abstract declare namespace module as is keyof infer satisfies",
     "null undefined true false any unknown never string number boolean object symbol bigint void Promise Array Record Partial Readonly"},
    {"Go", "go", "//", "", "/*", "*/", "\"'", F_BACKTICK | F_CAPTYPE,
     "break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var",
     "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr true false nil iota any"},
    {"Rust", "rs", "//", "", "/*", "*/", "\"", F_CAPTYPE,
     "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait type unsafe use where while",
     "bool char f32 f64 i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize str String Vec Option Result Box Rc Arc true false None Some Ok Err"},
    {"Java", "java", "//", "", "/*", "*/", "\"'", F_AT | F_CAPTYPE,
     "abstract assert break case catch class const continue default do else enum extends final finally for goto if implements import instanceof interface native new package private protected public return static strictfp super switch synchronized this throw throws transient try volatile while var record sealed permits",
     "boolean byte char double float int long short void String Object true false null"},
    {"C#", "cs", "//", "", "/*", "*/", "\"'", F_PREPROC | F_AT | F_CAPTYPE,
     "abstract as base break case catch checked class const continue default delegate do else enum event explicit extern finally fixed for foreach goto if implicit in interface internal is lock namespace new operator out override params private protected public readonly ref return sealed sizeof stackalloc static struct switch this throw try typeof unchecked unsafe using virtual volatile while async await var record init",
     "bool byte char decimal double float int long object sbyte short string uint ulong ushort void true false null"},
    {"Shell", "sh bash zsh ksh bashrc zshrc profile", "#", "", "", "", "\"'`", F_DOLLAR,
     "if then else elif fi for while until do done case esac in function select time return exit break continue local export readonly declare typeset unset source alias set shift trap eval exec",
     "true false echo printf cd pwd ls cat grep sed awk cut sort uniq tr head tail test read mkdir rm cp mv ln chmod chown find xargs tee"},
    {"Perl", "pl pm", "#", "", "", "", "\"'", F_DOLLAR,
     "my our local use no package sub return if elsif else unless while until for foreach last next redo goto do eval require die warn print printf",
     "undef defined ref scalar shift push pop keys values exists delete length substr index join split map grep sort reverse"},
    {"Ruby", "rb rake gemspec", "#", "", "=begin", "=end", "\"'", F_CAPTYPE,
     "alias and begin break case class def defined? do else elsif end ensure for if in module next nil not or redo rescue retry return self super then undef unless until when while yield require require_relative include extend attr_accessor attr_reader attr_writer",
     "true false nil puts print p"},
    {"PHP", "php phtml", "//", "#", "/*", "*/", "\"'", F_DOLLAR | F_CAPTYPE,
     "abstract and as break case catch class clone const continue declare default do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile extends final finally fn for foreach function global if implements include include_once instanceof insteadof interface isset list match namespace new or print private protected public readonly require require_once return static switch throw trait try unset use var while xor yield",
     "true false null int float string bool array object void mixed self parent"},
    {"Lua", "lua", "--", "", "--[[", "]]", "\"'", 0,
     "and break do else elseif end for function goto if in local not or repeat return then until while",
     "true false nil self require print pairs ipairs tostring tonumber type table string math os io"},
    {"SQL", "sql", "--", "", "/*", "*/", "\"'", F_CI,
     "select from where and or not in is null like between join inner left right outer full cross on group by order having limit offset insert into values update set delete create alter drop table index view database schema primary key foreign references unique default constraint union all distinct as case when then else end exists asc desc with begin commit rollback grant revoke",
     "int integer bigint smallint tinyint varchar char text boolean bool date time timestamp datetime float double decimal numeric serial blob json true false count sum avg min max coalesce"},
    {"HTML", "html htm xhtml vue svelte", "", "", "<!--", "-->", "\"'", F_MARKUP, "", ""},
    {"XML", "xml xsd xsl xslt svg plist rss atom csproj", "", "", "<!--", "-->", "\"'", F_MARKUP, "", ""},
    {"CSS", "css scss less", "//", "", "/*", "*/", "\"'", F_KEYCOLON | F_DASHIDENT | F_AT,
     "important inherit initial unset auto none", "px em rem vh vw pt cm mm deg ms"},
    {"JSON", "json jsonc json5 geojson webmanifest", "//", "", "/*", "*/", "\"", F_STRKEY, "", "true false null"},
    {"YAML", "yaml yml", "#", "", "", "", "\"'", F_KEYCOLON | F_DASHIDENT, "", "true false null yes no on off True False"},
    {"TOML", "toml", "#", "", "", "", "\"'", F_KEYEQ | F_DASHIDENT, "", "true false"},
    {"INI", "ini cfg conf desktop service socket timer mount properties editorconfig gitconfig", "#", ";", "", "", "\"'", F_KEYEQ | F_DASHIDENT, "", "true false yes no on off"},
    {"Markdown", "md markdown mdx", "", "", "", "", "", F_MD, "", ""},
    {"Makefile", "Makefile makefile GNUmakefile mk mak", "#", "", "", "", "\"'", F_DOLLAR | F_KEYEQ,
     "ifeq ifneq ifdef ifndef else endif include sinclude define endef export unexport override vpath", ".PHONY .DEFAULT .SUFFIXES"},
    {"Dockerfile", "Dockerfile dockerfile containerfile", "#", "", "", "", "\"'", F_DOLLAR | F_DASHIDENT,
     "FROM RUN CMD LABEL EXPOSE ENV ADD COPY ENTRYPOINT VOLUME USER WORKDIR ARG ONBUILD STOPSIGNAL HEALTHCHECK SHELL AS", ""},
    {"Ansible/Jinja", "j2 jinja jinja2", "", "", "{#", "#}", "\"'", 0,
     "if else elif endif for endfor in not and or is set block endblock extends include macro endmacro call filter raw endraw", "true false none"},
    {"Diff", "diff patch", "", "", "", "", "", F_DIFF, "", ""},
    {"CMake", "cmake", "#", "", "", "", "\"", F_DOLLAR,
     "if else elseif endif foreach endforeach while endwhile function endfunction macro endmacro return set unset option include find_package project add_executable add_library target_link_libraries target_include_directories install message", "ON OFF TRUE FALSE"},
    {"Assembly", "s asm", "#", ";", "/*", "*/", "\"'", F_PREPROC,
     "mov push pop call ret jmp je jne jz jnz jg jl jge jle cmp test add sub mul div imul idiv inc dec and or xor not shl shr lea nop int syscall leave", "rax rbx rcx rdx rsi rdi rsp rbp eax ebx ecx edx esi edi esp ebp r8 r9 r10 r11 r12 r13 r14 r15"},
    {"Kotlin", "kt kts", "//", "", "/*", "*/", "\"'", F_AT | F_CAPTYPE,
     "abstract as break by class companion const constructor continue data do else enum final finally for fun if import in init inline interface internal is lateinit object open operator out override package private protected public return sealed super suspend this throw try typealias val var when where while",
     "Int Long Short Byte Float Double Boolean Char String Unit Any Nothing true false null"},
    {"Swift", "swift", "//", "", "/*", "*/", "\"", F_AT | F_CAPTYPE,
     "associatedtype break case catch class continue default defer deinit do else enum extension fallthrough fileprivate for func guard if import in init inout internal is let open operator private protocol public repeat rethrows return self Self static struct subscript super switch throw throws try typealias var where while async await actor",
     "Int Double Float Bool String Character Array Dictionary Set Optional Any AnyObject Void true false nil"},
};

const int NLANGS = (int)(sizeof g_langs / sizeof g_langs[0]);

uint32_t fnv(const char *p, size_t n, bool ci) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)p[i]; if (ci) c = (unsigned char)tolower(c); h = (h ^ c) * 16777619u; }
    return h;
}

void add_words(Lang &L, const char *list, uint8_t cls, std::vector<Entry> &out) {
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e > p) out.push_back({p, (uint8_t)(e - p), cls});
        p = e;
    }
}

void prep(Lang &L) {
    if (L.ready) return;
    std::vector<Entry> words;
    add_words(L, L.kw, T_KEYWORD, words);
    add_words(L, L.types, T_TYPE, words);
    size_t sz = 16;
    while (sz < words.size() * 3) sz <<= 1;
    L.table.assign(sz, Entry{nullptr, 0, 0});
    bool ci = L.flags & F_CI;
    for (const Entry &w : words) {
        size_t k = fnv(w.w, w.len, ci) & (sz - 1);
        while (L.table[k].w) k = (k + 1) & (sz - 1);
        L.table[k] = w;
    }
    L.blen_o = strlen(L.bo);
    L.blen_c = strlen(L.bc);
    L.lc1l = strlen(L.lc1);
    L.lc2l = strlen(L.lc2);
    L.bo0 = (unsigned char)L.bo[0]; L.lc10 = (unsigned char)L.lc1[0]; L.lc20 = (unsigned char)L.lc2[0];
    memset(L.is_quote, 0, sizeof L.is_quote);
    memset(L.is_punct, 0, sizeof L.is_punct);
    for (const char *q = L.quotes; *q; q++) L.is_quote[(unsigned char)*q] = true;
    for (const char *q = "+-*/%=<>!&|^~?:;,.()[]{}"; *q; q++) L.is_punct[(unsigned char)*q] = true;
    L.ready = true;
}

int lookup(const Lang &L, const char *w, size_t n) {
    if (L.table.empty() || n > 255) return -1;
    bool ci = L.flags & F_CI;
    size_t mask = L.table.size() - 1, k = fnv(w, n, ci) & mask;
    while (L.table[k].w) {
        const Entry &e = L.table[k];
        if (e.len == n && (ci ? strncasecmp(e.w, w, n) == 0 : memcmp(e.w, w, n) == 0)) return e.cls;
        k = (k + 1) & mask;
    }
    return -1;
}

inline void fill(uint8_t *tok, size_t a, size_t b, uint8_t c) {
    if (b - a < 16) { for (size_t k = a; k < b; k++) tok[k] = c; }   /* most tokens are a few bytes: a loop beats a memset call */
    else memset(tok + a, c, b - a);
}
/* locale-free ASCII classes: bit 1 identifier start, bit 2 identifier part, bit 4 digit */
struct CharTab {
    uint8_t t[256];
    CharTab() {
        for (int c = 0; c < 256; c++) {
            bool a = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80, d = c >= '0' && c <= '9';
            t[c] = (uint8_t)((a ? 1 | 2 : 0) | (d ? 2 | 4 : 0));
        }
    }
};
const CharTab g_ct;
inline bool is_id_start(unsigned char c) { return g_ct.t[c] & 1; }
inline bool is_id(unsigned char c) { return g_ct.t[c] & 2; }
inline bool is_digit(unsigned char c) { return g_ct.t[c] & 4; }

const char *find_str(const char *s, size_t n, size_t from, const char *pat, size_t pl) {
    if (!pl || from + pl > n) return nullptr;
    return static_cast<const char *>(memmem(s + from, n - from, pat, pl));
}

uint8_t lex_md(const char *s, size_t n, uint8_t st, uint8_t *tok) {
    size_t i = 0;
    while (i < n && (s[i] == ' ')) i++;
    if (n - i >= 3 && (!memcmp(s + i, "```", 3) || !memcmp(s + i, "~~~", 3))) {
        fill(tok, 0, n, T_KEYWORD);
        return st == S_FENCE ? S_NORMAL : S_FENCE;
    }
    if (st == S_FENCE) { fill(tok, 0, n, T_STRING); return st; }
    if (i < n && s[i] == '#') { size_t k = i; while (k < n && s[k] == '#') k++; if (k < n && s[k] == ' ') { fill(tok, 0, n, T_HEADING); return st; } }
    if (i < n && s[i] == '>') { fill(tok, 0, n, T_COMMENT); return st; }
    if (i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < n && s[i + 1] == ' ') { tok[i] = T_KEYWORD; i += 1; }
    else if (i < n && isdigit((unsigned char)s[i])) { size_t k = i; while (k < n && isdigit((unsigned char)s[k])) k++; if (k + 1 < n && s[k] == '.' && s[k + 1] == ' ') { fill(tok, i, k + 1, T_KEYWORD); i = k + 1; } }
    for (; i < n; i++) {
        if (s[i] == '`') {
            size_t e = i + 1;
            while (e < n && s[e] != '`') e++;
            if (e < n) { fill(tok, i, e + 1, T_STRING); i = e; }
        } else if (s[i] == '[') {
            size_t e = i + 1;
            while (e < n && s[e] != ']') e++;
            if (e + 1 < n && s[e + 1] == '(') {
                size_t c = e + 2;
                while (c < n && s[c] != ')') c++;
                if (c < n) { fill(tok, i + 1, e, T_FUNC); fill(tok, e + 2, c, T_STRING); i = c; }
            }
        } else if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
            const char *e = find_str(s, n, i + 2, "**", 2);
            if (e) { size_t ee = (size_t)(e - s); fill(tok, i, ee + 2, T_TYPE); i = ee + 1; }
        }
    }
    return st;
}

uint8_t lex_diff(const char *s, size_t n, uint8_t st, uint8_t *tok) {
    if (!n) return st;
    uint8_t c = T_PLAIN;
    if (n >= 3 && (!memcmp(s, "+++", 3) || !memcmp(s, "---", 3))) c = T_KEYWORD;
    else if (s[0] == '+') c = T_INS;
    else if (s[0] == '-') c = T_DEL;
    else if (s[0] == '@') c = T_TYPE;
    else if (n >= 4 && !memcmp(s, "diff", 4)) c = T_KEYWORD;
    fill(tok, 0, n, c);
    return st;
}

}  // namespace

int lang_count() { return NLANGS; }
const Lang *lang_at(int i) { return i >= 0 && i < NLANGS ? &g_langs[i] : &g_langs[0]; }
const char *lang_name(const Lang *l) { return l->name; }
int lang_id(const Lang *l) { return (int)(l - g_langs); }
const Lang *lang_plain() { return &g_langs[0]; }

const Lang *lang_detect(const char *path, const char *first, size_t first_len) {
    if (path && *path) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        const char *dot = strrchr(base, '.');
        for (int i = 1; i < NLANGS; i++) {
            for (const char *p = g_langs[i].exts; *p;) {
                while (*p == ' ') p++;
                const char *e = p;
                while (*e && *e != ' ') e++;
                size_t l = (size_t)(e - p);
                if (l && ((strlen(base) == l && !strncasecmp(base, p, l)) || (dot && strlen(dot + 1) == l && !strncasecmp(dot + 1, p, l)))) return &g_langs[i];
                p = e;
            }
        }
    }
    if (first && first_len > 2 && first[0] == '#' && first[1] == '!') {
        std::string f(first, first_len);
        auto has = [&](const char *w) { return f.find(w) != std::string::npos; };
        if (has("python")) return &g_langs[3];
        if (has("node") || has("deno")) return &g_langs[4];
        if (has("perl")) return &g_langs[11];
        if (has("ruby")) return &g_langs[12];
        if (has("bash") || has("/sh") || has("zsh") || has("env sh")) return &g_langs[10];
    }
    return &g_langs[0];
}

uint8_t lex_line(const Lang *cl, const char *s, size_t n, uint8_t st, uint8_t *tok) {
    Lang &L = *const_cast<Lang *>(cl);
    prep(L);
    if (n) memset(tok, T_PLAIN, n);
    if (L.flags & F_MD) return lex_md(s, n, st, tok);
    if (L.flags & F_DIFF) return lex_diff(s, n, st, tok);
    const unsigned fl = L.flags;
    const size_t lc1 = L.lc1l, lc2 = L.lc2l;
    bool first_tok = true;   /* nothing but whitespace seen yet on this line */
    size_t i = 0;
    while (i < n) {
        if (st == S_BLOCK) {
            const char *e = find_str(s, n, i, L.bc, L.blen_c);
            size_t end = e ? (size_t)(e - s) + L.blen_c : n;
            fill(tok, i, end, T_COMMENT);
            i = end;
            if (e) st = S_NORMAL;
            continue;
        }
        if (st == S_TDQ || st == S_TSQ) {
            const char *pat = st == S_TDQ ? "\"\"\"" : "'''";
            size_t k = i;
            const char *e = nullptr;
            while (k < n) {
                if (s[k] == '\\') { k += 2; continue; }
                if (k + 3 <= n && !memcmp(s + k, pat, 3)) { e = s + k; break; }
                k++;
            }
            size_t end = e ? (size_t)(e - s) + 3 : n;
            fill(tok, i, end, T_STRING);
            i = end;
            if (e) st = S_NORMAL;
            continue;
        }
        if (st == S_BT) {
            size_t k = i;
            bool closed = false;
            while (k < n) { if (s[k] == '\\') { k += 2; continue; } if (s[k] == '`') { closed = true; k++; break; } k++; }
            if (k > n) k = n;
            fill(tok, i, k, T_STRING);
            i = k;
            if (closed) st = S_NORMAL;
            continue;
        }
        if (st == S_TAG) {
            unsigned char c = (unsigned char)s[i];
            if (c == '>') { tok[i] = T_KEYWORD; i++; st = S_NORMAL; }
            else if (c == '"' || c == '\'') {
                size_t k = i + 1;
                while (k < n && s[k] != (char)c) k++;
                if (k < n) k++;
                fill(tok, i, k, T_STRING);
                i = k;
            } else if (is_id_start(c)) {
                size_t k = i + 1;
                while (k < n && (is_id((unsigned char)s[k]) || s[k] == '-' || s[k] == ':')) k++;
                fill(tok, i, k, T_TYPE);
                i = k;
            } else { if (c == '/' || c == '?') tok[i] = T_KEYWORD; i++; }
            continue;
        }

        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t') { i++; continue; }

        if (c == L.lc10 && lc1 && i + lc1 <= n && !memcmp(s + i, L.lc1, lc1)) { fill(tok, i, n, T_COMMENT); return st; }
        if (c == L.lc20 && lc2 && i + lc2 <= n && !memcmp(s + i, L.lc2, lc2)) { fill(tok, i, n, T_COMMENT); return st; }
        if (c == L.bo0 && L.blen_o && i + L.blen_o <= n && !memcmp(s + i, L.bo, L.blen_o)) {
            fill(tok, i, i + L.blen_o, T_COMMENT);
            i += L.blen_o;
            st = S_BLOCK;
            first_tok = false;
            continue;
        }
        bool was_first = first_tok;
        first_tok = false;

        if (fl & F_MARKUP) {
            if (c == '<' && i + 1 < n && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '?' || s[i + 1] == '!')) {
                size_t k = i + 1;
                if (s[k] == '/' || s[k] == '?' || s[k] == '!') k++;
                while (k < n && (is_id((unsigned char)s[k]) || s[k] == '-' || s[k] == ':' || s[k] == '.')) k++;
                fill(tok, i, k, T_KEYWORD);
                i = k;
                st = S_TAG;
            } else if (c == '&') {
                size_t k = i + 1;
                while (k < n && s[k] != ';' && k - i < 10 && isalnum((unsigned char)s[k])) k++;
                if (k < n && s[k] == ';') { fill(tok, i, k + 1, T_NUMBER); i = k + 1; } else i++;
            } else i++;
            continue;
        }

        if (L.is_quote[c]) {
            if (!L.is_quote[c]) goto punct;
            if (c == '`' && (fl & F_BACKTICK)) { st = S_BT; tok[i] = T_STRING; i++; continue; }
            if ((fl & F_TRIPLE) && c != '`' && i + 2 < n && s[i + 1] == (char)c && s[i + 2] == (char)c) {
                st = c == '"' ? S_TDQ : S_TSQ;
                fill(tok, i, i + 3, T_STRING);
                i += 3;
                continue;
            }
            size_t k = i + 1;
            while (k < n && s[k] != (char)c) { if (s[k] == '\\') k++; k++; }
            if (k > n) k = n;
            size_t end = k < n ? k + 1 : n;
            uint8_t cls = T_STRING;
            if (fl & F_STRKEY) {
                size_t a = end;
                while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
                if (a < n && s[a] == ':') cls = T_KEY;
            } else if (fl & F_KEYCOLON) {
                size_t a = end;
                while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
                if (a < n && s[a] == ':' && (a + 1 == n || s[a + 1] == ' ')) cls = T_KEY;
            }
            fill(tok, i, end, cls);
            i = end;
            continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit((unsigned char)s[i + 1]))) {
            size_t k = i + 1;
            while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '_' || s[k] == '.' || s[k] == '\'')) {
                if ((s[k] == 'e' || s[k] == 'E') && k + 1 < n && (s[k + 1] == '+' || s[k + 1] == '-') && !(s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X'))) k++;
                k++;
            }
            fill(tok, i, k, T_NUMBER);
            i = k;
            continue;
        }
        if ((fl & F_PREPROC) && c == '#' && was_first) {
            size_t k = i + 1;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            size_t w = k;
            while (k < n && is_id((unsigned char)s[k])) k++;
            fill(tok, i, k, T_PREPROC);
            if (k - w == 7 && !memcmp(s + w, "include", 7)) {   /* <header> is a string like in VS Code */
                size_t a = k;
                while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
                if (a < n && s[a] == '<') { size_t e = a; while (e < n && s[e] != '>') e++; if (e < n) e++; fill(tok, a, e, T_STRING); k = e; }
            }
            i = k;
            continue;
        }
        if ((fl & F_AT) && c == '@' && i + 1 < n && is_id_start((unsigned char)s[i + 1])) {
            size_t k = i + 1;
            while (k < n && (is_id((unsigned char)s[k]) || s[k] == '.' || ((fl & F_DASHIDENT) && s[k] == '-'))) k++;
            fill(tok, i, k, T_PREPROC);
            i = k;
            continue;
        }
        if ((fl & F_DOLLAR) && c == '$' && i + 1 < n) {
            size_t k = i + 1;
            if (s[k] == '{' || s[k] == '(') {
                char close = s[k] == '{' ? '}' : ')';
                while (k < n && s[k] != close) k++;
                if (k < n) k++;
            } else if (is_id_start((unsigned char)s[k])) { while (k < n && is_id((unsigned char)s[k])) k++; }
            else k++;
            fill(tok, i, k, T_TYPE);
            i = k;
            continue;
        }
        if (is_id_start(c)) {
            size_t k = i + 1;
            for (; k < n; k++) {
                unsigned char d = (unsigned char)s[k];
                if (is_id(d)) continue;
                if ((fl & F_DASHIDENT) && d == '-' && k + 1 < n && is_id((unsigned char)s[k + 1])) continue;
                break;
            }
            int cls = lookup(L, s + i, k - i);
            size_t a = k;
            while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
            if (cls < 0) {
                if ((fl & F_KEYCOLON) && a < n && s[a] == ':' && (a + 1 == n || s[a + 1] == ' ')) cls = T_KEY;   /* "key: value"; CSS "a:hover" stays plain */
                if (cls < 0 && (fl & F_KEYEQ) && was_first && a < n && (s[a] == '=' || s[a] == ':')) cls = T_KEY;
                if (cls < 0 && a < n && s[a] == '(') cls = T_FUNC;
                if (cls < 0 && (fl & F_CAPTYPE) && isupper(c)) {
                    bool lower = false;
                    for (size_t q = i; q < k; q++) if (islower((unsigned char)s[q])) { lower = true; break; }
                    if (lower) cls = T_TYPE;
                }
            }
            if (cls > 0) fill(tok, i, k, (uint8_t)cls);
            i = k;
            continue;
        }
    punct:
        if (L.is_punct[c]) tok[i] = T_PUNCT;
        i++;
    }
    return st;
}
