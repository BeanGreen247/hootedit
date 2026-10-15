/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "outline.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>

namespace {
bool ident_ch(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$' || c == ':' || c == '~' || c == '.'; }

}  // namespace

void outline_scan(const Doc &d, std::vector<OItem> &out) {
    out.clear();
    bool md = std::string(lang_name(d.lang)) == "Markdown";
    static const char *mods[] = {"export ", "default ", "public ", "private ", "protected ", "static ", "async ", "pub ", "pub(crate) ", "final ", "abstract ", "inline ", "virtual ", "extern ", "unsafe ", "sealed ", "partial ", "open ", "override "};
    static const char *types[] = {"class ", "struct ", "enum ", "union ", "namespace ", "interface ", "trait ", "impl ", "mod ", "module ", "object ", "protocol ", "extension ", "type "};
    static const char *funcs[] = {"def ", "fn ", "func ", "function ", "function* ", "fun ", "sub ", "proc "};
    static const char *ctl[] = {"if", "for", "while", "switch", "return", "else", "do", "case", "catch", "sizeof", "delete", "new", "throw", "using", "typedef", "goto", "elif", "with", "assert", "await", "yield", "not", "and", "or"};
    std::vector<int> stack;   /* indents of the enclosing items: depth = how many are shallower */
    std::string t;
    size_t n = std::min<size_t>(d.buf.lines(), 30000);
    for (size_t l = 0; l < n && out.size() < 3000; l++) {
        if (d.buf.line_len(l) > 300) continue;
        d.buf.line_text(l, t);
        int ind = 0; size_t i = 0;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) { ind += t[i] == '\t' ? 4 : 1; i++; }
        if (i >= t.size()) continue;
        std::string s = t.substr(i);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
        if (s.empty()) continue;
        auto push = [&](const std::string &name, char kind) {
            while (!stack.empty() && stack.back() >= ind) stack.pop_back();
            out.push_back({name, l, std::min<int>((int)stack.size(), 4), kind});
            stack.push_back(ind);
        };
        if (md) {
            if (s[0] != '#') continue;
            size_t h = 0; while (h < s.size() && s[h] == '#') h++;
            if (h > 6 || h >= s.size() || s[h] != ' ') continue;
            out.push_back({s.substr(h + 1), l, (int)h - 1, 'H'});
            continue;
        }
        if (s[0] == '#' || s[0] == '/' || s[0] == '*' || s[0] == '}' || s[0] == '"' || s[0] == '\'') continue;
        std::string k = s;
        for (bool again = true; again;) { again = false; for (const char *m : mods) { size_t ml = strlen(m); if (k.compare(0, ml, m) == 0) { k.erase(0, ml); again = true; } } }
        auto take = [&](size_t from) { size_t e = from; while (e < k.size() && ident_ch(k[e])) e++; while (e > from && (k[e - 1] == ':' || k[e - 1] == '.')) e--; return k.substr(from, e - from); };   /* "class A:" is A */
        bool done = false;
        for (const char *ty : types) {
            size_t tl = strlen(ty);
            if (k.compare(0, tl, ty) != 0) continue;
            size_t from = tl;
            if (k.compare(from, 6, "class ") == 0) from += 6; else if (k.compare(from, 7, "struct ") == 0) from += 7;
            std::string nm;
            if (!strcmp(ty, "impl ")) { nm = k.substr(from); size_t b = nm.find('{'); if (b != std::string::npos) nm.erase(b); while (!nm.empty() && nm.back() == ' ') nm.pop_back(); }
            else nm = take(from);
            if (nm.empty() || (k.back() == ';' && k.find('{') == std::string::npos)) { done = true; break; }
            push(nm, 'C'); done = true; break;
        }
        if (done) continue;
        for (const char *fn : funcs) {
            size_t fl = strlen(fn);
            if (k.compare(0, fl, fn) != 0) continue;
            size_t from = fl;
            if (k[from] == '(') { size_t c = k.find(')', from); if (c == std::string::npos) break; from = c + 1; while (from < k.size() && k[from] == ' ') from++; }   /* Go receiver */
            std::string nm = take(from);
            if (!nm.empty()) push(nm, 'F');
            done = true; break;
        }
        if (done) continue;
        /* C-like definitions: "type name(...) {", "Class::method(...) const", "name(...) {" inside a body */
        char last = k.back();
        if (!(isalpha((unsigned char)k[0]) || k[0] == '_' || k[0] == '~')) continue;
        if (last == ';' || last == ',' || last == '=' || last == '(') continue;
        size_t par = k.find('(');
        if (par == std::string::npos || par == 0) continue;
        if (k.find('=') < par || k.find('"') < par) continue;
        size_t e = par; while (e > 0 && k[e - 1] == ' ') e--;
        size_t b = e; while (b > 0 && ident_ch(k[b - 1])) b--;
        if (b == e) continue;
        std::string nm = k.substr(b, e - b);
        bool ctlw = false;
        size_t fw = 0; while (fw < k.size() && ident_ch(k[fw])) fw++;
        std::string first = k.substr(0, fw);
        for (const char *c : ctl) if (first == c) ctlw = true;
        if (ctlw) continue;
        bool typed = b > 0, scoped = nm.find("::") != std::string::npos;
        bool ret_paren = last == ')' || last == '{' || k.find(") const") != std::string::npos || k.find(") noexcept") != std::string::npos || k.find(") override") != std::string::npos;
        if (!ret_paren) continue;
        if (!typed && !scoped && !(last == '{' && k.size() >= 3 && k[k.size() - 2] == ' ' && k[k.size() - 3] == ')' && k.find("=>") == std::string::npos && k.find('\'') == std::string::npos)) continue;
        push(nm, 'F');
    }
}

