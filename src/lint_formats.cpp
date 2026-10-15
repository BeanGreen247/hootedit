/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <map>
#include <set>

#include <sys/stat.h>

#include "lint_priv.h"

/* Checks for file formats and languages that are not worth a file of their own: TOML, XML, Markdown, SQL, CSS and a few leftover-debugging patterns. */

namespace lintp {
namespace {

sv strip_toml_comment(sv s) {
    bool q1 = false, q2 = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && q2) { i++; continue; }
        if (s[i] == '"' && !q1) q2 = !q2;
        else if (s[i] == '\'' && !q2) q1 = !q1;
        else if (s[i] == '#' && !q1 && !q2) return trim(s.substr(0, i));
    }
    return trim(s);
}

size_t toml_eq(sv s) {   /* the '=' that ends the key, outside quotes */
    bool q1 = false, q2 = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '"' && !q1) q2 = !q2;
        else if (s[i] == '\'' && !q2) q1 = !q1;
        else if (s[i] == '=' && !q1 && !q2) return i;
    }
    return sv::npos;
}

}  // namespace

void toml_check(Ctx &c, sv text) {
    std::set<std::string> tables;
    std::map<std::string, std::set<std::string>> keys;
    std::string cur;
    int depth = 0;             /* open [ or { of a value that continues on the next line */
    std::string ml;            /* """ or ''' while inside a multi-line string */
    uint32_t ln = 0;
    size_t pos = 0;
    while (pos <= text.size() && !c.full()) {
        size_t e = text.find('\n', pos);
        if (e == sv::npos) e = text.size();
        sv line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        c.line = ln; c.cur = line;
        auto advance = [&] { ln++; pos = e + 1; };
        if (!ml.empty()) { size_t p = line.find(ml); if (p != sv::npos) ml.clear(); if (e >= text.size()) break; advance(); continue; }
        sv t = trim(line);
        if (t.empty() || t[0] == '#') { if (e >= text.size()) break; advance(); continue; }
        size_t lead = line.find_first_not_of(" \t");
        if (depth > 0) {
            bool q1 = false, q2 = false;
            for (size_t i = 0; i < t.size(); i++) {
                char ch = t[i];
                if (ch == '\\' && q2) { i++; continue; }
                if (ch == '"' && !q1) q2 = !q2; else if (ch == '\'' && !q2) q1 = !q1;
                else if (!q1 && !q2) { if (ch == '#') break; if (ch == '[' || ch == '{') depth++; else if (ch == ']' || ch == '}') depth--; }
            }
            if (depth < 0) depth = 0;
            if (e >= text.size()) break;
            advance(); continue;
        }
        if (t[0] == '[') {
            bool arr = starts(t, "[[");
            size_t close = t.find(arr ? "]]" : "]");
            if (close == sv::npos) add(c, lead, lead + t.size(), SEV_ERROR, "toml-header", "Unclosed table header: missing ']'");
            else {
                std::string name(trim(t.substr(arr ? 2 : 1, close - (arr ? 2 : 1))));
                sv rest = trim(t.substr(close + (arr ? 2 : 1)));
                if (!rest.empty() && rest[0] != '#') add(c, lead, lead + t.size(), SEV_ERROR, "toml-header", "Unexpected text after the table header");
                if (name.empty()) add(c, lead, lead + t.size(), SEV_ERROR, "toml-header", "Empty table name");
                else if (!arr && !tables.insert(name).second) add(c, lead, lead + t.size(), SEV_ERROR, "toml-duplicate-table", "Duplicate table [" + name + "]");
                cur = (arr ? "[[" : "[") + name;
                if (arr) keys[cur].clear();
            }
        } else {
            size_t eq = toml_eq(t);
            if (eq == sv::npos) add(c, lead, lead + t.size(), SEV_ERROR, "toml-syntax", "Expected 'key = value'");
            else {
                std::string key(trim(t.substr(0, eq)));
                if (key.empty()) add(c, lead, lead + eq + 1, SEV_ERROR, "toml-syntax", "Missing key before '='");
                else if (!keys[cur].insert(key).second) add(c, lead, lead + key.size(), SEV_ERROR, "toml-duplicate-key", "Duplicate key '" + key + "' in this table");
                sv v = trim(t.substr(eq + 1));
                if (v.empty() || v[0] == '#') add(c, lead + eq, lead + eq + 1, SEV_ERROR, "toml-syntax", "Missing value after '='");
                else if (starts(v, "\"\"\"") || starts(v, "'''")) {
                    std::string q(v.substr(0, 3));
                    if (v.find(q, 3) == sv::npos) ml = q;
                } else if (v[0] == '"' || v[0] == '\'') {
                    size_t p = 1;
                    while (p < v.size() && v[p] != v[0]) { if (v[p] == '\\' && v[0] == '"') p++; p++; }
                    if (p >= v.size()) add(c, lead + eq + 1, lead + t.size(), SEV_ERROR, "toml-string", "Unterminated string");
                } else if (v[0] == '[' || v[0] == '{') {
                    sv sc = strip_toml_comment(v);
                    int d = 0;
                    bool q1 = false, q2 = false;
                    for (size_t i = 0; i < sc.size(); i++) {
                        char ch = sc[i];
                        if (ch == '\\' && q2) { i++; continue; }
                        if (ch == '"' && !q1) q2 = !q2; else if (ch == '\'' && !q2) q1 = !q1;
                        else if (!q1 && !q2) { if (ch == '[' || ch == '{') d++; else if (ch == ']' || ch == '}') d--; }
                    }
                    if (d > 0) depth = d;
                } else {
                    sv sc = strip_toml_comment(v);
                    if (sc == "True" || sc == "False") add(c, lead + eq + 1, lead + t.size(), SEV_ERROR, "toml-boolean", "TOML booleans are lowercase: true / false");
                    else if (sc == "None" || sc == "null" || sc == "nil" || sc == "NULL") add(c, lead + eq + 1, lead + t.size(), SEV_WARN, "toml-null", "TOML has no null value");
                }
            }
        }
        if (e >= text.size()) break;
        advance();
    }
    c.cur = sv();
}

void xml_check(Ctx &c, sv text) {
    struct Open { std::string name; uint32_t line, col; };
    std::vector<Open> stack;
    uint32_t line = 0;
    size_t ls = 0;
    auto lc = [&](size_t p, uint32_t &l, size_t &col) { l = line; col = p - ls; };
    size_t i = 0, n = text.size();
    auto newline_scan = [&](size_t from, size_t to) { for (size_t k = from; k < to; k++) if (text[k] == '\n') { line++; ls = k + 1; } };
    size_t last = 0;
    auto sync = [&](size_t upto) { newline_scan(last, upto); last = upto; };
    while (i < n && !c.full()) {
        size_t lt = text.find('<', i);
        if (lt == sv::npos) break;
        // a bare & in text
        for (size_t a = text.find('&', i); a != sv::npos && a < lt; a = text.find('&', a + 1)) {
            size_t semi = text.find(';', a);
            bool ent = semi != sv::npos && semi - a <= 10 && semi < lt;
            if (!ent) { sync(a); c.line = line; c.cur = text.substr(ls, text.find('\n', ls) == sv::npos ? sv::npos : text.find('\n', ls) - ls); add(c, a - ls, a - ls + 1, SEV_ERROR, "xml-ampersand", "A bare '&' must be written &amp;"); }
        }
        sync(lt);
        c.line = line;
        size_t eol = text.find('\n', ls);
        c.cur = text.substr(ls, eol == sv::npos ? sv::npos : eol - ls);
        if (starts(text.substr(lt), "<!--")) { size_t e = text.find("-->", lt + 4); if (e == sv::npos) { add(c, lt - ls, lt - ls + 4, SEV_ERROR, "xml-comment", "Unterminated comment"); return; } i = e + 3; continue; }
        if (starts(text.substr(lt), "<![CDATA[")) { size_t e = text.find("]]>", lt); if (e == sv::npos) { add(c, lt - ls, lt - ls + 9, SEV_ERROR, "xml-cdata", "Unterminated CDATA section"); return; } i = e + 3; continue; }
        if (starts(text.substr(lt), "<?")) { size_t e = text.find("?>", lt); if (e == sv::npos) { add(c, lt - ls, lt - ls + 2, SEV_ERROR, "xml-pi", "Unterminated processing instruction"); return; } i = e + 2; continue; }
        if (starts(text.substr(lt), "<!")) {   /* DOCTYPE, possibly with an internal subset in [ ] */
            size_t e = lt + 2; int br = 0;
            while (e < n && !(text[e] == '>' && br == 0)) { if (text[e] == '[') br++; else if (text[e] == ']') br--; e++; }
            i = e + 1; continue;
        }
        bool closing = lt + 1 < n && text[lt + 1] == '/';
        size_t p = lt + (closing ? 2 : 1);
        size_t ns = p;
        while (p < n && !isspace((unsigned char)text[p]) && text[p] != '>' && text[p] != '/') p++;
        std::string name(text.substr(ns, p - ns));
        if (name.empty() || !(isalpha((unsigned char)name[0]) || name[0] == '_' || name[0] == ':')) { add(c, lt - ls, lt - ls + 1, SEV_ERROR, "xml-tag", "A '<' must start a tag; write &lt; for the character"); i = lt + 1; continue; }
        // attributes up to '>'
        bool self = false;
        size_t q = p;
        bool bad_attr = false;
        while (q < n && text[q] != '>') {
            if (text[q] == '"' || text[q] == '\'') { size_t e = text.find(text[q], q + 1); if (e == sv::npos) { add(c, q - ls, q - ls + 1, SEV_ERROR, "xml-attribute", "Unterminated attribute value"); return; } q = e + 1; continue; }
            if (text[q] == '=' ) { size_t v = q + 1; while (v < n && isspace((unsigned char)text[v])) v++; if (v < n && text[v] != '"' && text[v] != '\'' && !bad_attr) { add(c, v - ls < 0 ? 0 : (v >= ls ? v - ls : 0), (v >= ls ? v - ls : 0) + 1, SEV_ERROR, "xml-attribute", "An attribute value must be in quotes"); bad_attr = true; } }
            if (text[q] == '<') { add(c, lt - ls, lt - ls + 1, SEV_ERROR, "xml-tag", "Unclosed tag: missing '>'"); break; }
            q++;
        }
        if (q >= n) { add(c, lt - ls, lt - ls + 1, SEV_ERROR, "xml-tag", "Unclosed tag: missing '>'"); return; }
        self = q > lt && text[q - 1] == '/';
        if (closing) {
            if (stack.empty()) add(c, lt - ls, lt - ls + name.size() + 3, SEV_ERROR, "xml-unmatched", "Closing tag </" + name + "> has no opening tag");
            else if (stack.back().name != name) {
                add(c, lt - ls, lt - ls + name.size() + 3, SEV_ERROR, "xml-mismatch", "Expected </" + stack.back().name + "> but found </" + name + ">");
                bool known = false;
                for (const Open &o : stack) if (o.name == name) known = true;
                if (known) while (!stack.empty() && stack.back().name != name) stack.pop_back();
                if (!stack.empty()) stack.pop_back();
            } else stack.pop_back();
        } else if (!self) { uint32_t l; size_t col; lc(lt, l, col); stack.push_back({name, l, (uint32_t)col}); }
        i = q + 1;
    }
    sync(n);
    size_t shown = 0;
    for (size_t k = stack.size(); k-- > 0 && shown < 3; shown++) {
        c.line = stack[k].line;
        size_t s0 = 0; uint32_t l = 0;
        for (size_t p = 0; p < n && l < stack[k].line; p++) if (text[p] == '\n') { l++; s0 = p + 1; }
        size_t e = text.find('\n', s0);
        c.cur = text.substr(s0, e == sv::npos ? sv::npos : e - s0);
        add(c, stack[k].col, stack[k].col + stack[k].name.size() + 1, SEV_ERROR, "xml-unclosed", "<" + stack[k].name + "> is never closed");
    }
    c.cur = sv();
}

void markdown_check(Ctx &c, sv text, const std::string &path) {
    bool fence = false;
    uint32_t fence_line = 0;
    std::string fence_marker;
    std::string dir = path.rfind('/') == std::string::npos ? std::string(".") : path.substr(0, path.rfind('/'));
    int links_checked = 0;
    uint32_t ln = 0;
    size_t pos = 0;
    sv last_line;
    while (pos <= text.size() && !c.full()) {
        size_t e = text.find('\n', pos);
        if (e == sv::npos) e = text.size();
        sv line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        c.line = ln; c.cur = line;
        sv t = trim(line);
        if (starts(t, "```") || starts(t, "~~~")) {
            std::string mk(t.substr(0, 3));
            if (!fence) { fence = true; fence_line = ln; fence_marker = mk; }
            else if (mk == fence_marker) fence = false;
        } else if (!fence) {
            size_t h = 0;
            while (h < line.size() && line[h] == '#') h++;
            bool pp = false;   /* #include, #define, an issue number like #123 and a shebang are not headings */
            if (h == 1 && h < line.size()) {
                sv word = line.substr(1, std::min<size_t>(10, line.size() - 1));
                for (const char *w : {"include", "define", "pragma", "if", "ifdef", "ifndef", "endif", "else", "elif", "undef", "error", "warning", "!"}) if (starts(word, w)) pp = true;
                if (isdigit((unsigned char)line[1])) pp = true;
            }
            if (h > 0 && h <= 6 && h < line.size() && line[h] != ' ' && line[h] != '#' && line[h] != '\t' && !pp) {
                Diag &d = add(c, 0, h + 1, SEV_HINT, "md-heading", "A heading needs a space after the #");
                if (!d.code.empty()) fixit(d, h, h, " ");
            }
            // relative links: [text](path)
            size_t p = 0;
            bool code = false;
            while (p < line.size() && links_checked < 300) {
                if (line[p] == '`') { code = !code; p++; continue; }
                if (!code && line[p] == ']' && p + 1 < line.size() && line[p + 1] == '(') {
                    size_t close = line.find(')', p + 2);
                    if (close == sv::npos) break;
                    std::string target(line.substr(p + 2, close - p - 2));
                    size_t sp = target.find(' ');
                    if (sp != std::string::npos) target.resize(sp);
                    size_t q = target.find_first_of("#?");
                    std::string file = q == std::string::npos ? target : target.substr(0, q);
                    bool remote = file.find("://") != std::string::npos || starts(file, "mailto:") || starts(file, "tel:") || starts(file, "data:");
                    if (!file.empty() && !remote && file[0] != '<' && file.find('{') == std::string::npos && !path.empty()) {
                        links_checked++;
                        std::string full = file[0] == '/' ? file : dir + "/" + file;
                        struct stat st;
                        if (stat(full.c_str(), &st) != 0) add(c, p + 2, close, SEV_WARN, "md-broken-link", "'" + file + "' does not exist");
                    }
                    p = close;
                    continue;
                }
                p++;
            }
        }
        last_line = line;
        if (e >= text.size()) break;
        ln++;
        pos = e + 1;
    }
    if (fence) { c.line = fence_line; c.cur = sv(); add(c, 0, 3, SEV_WARN, "md-fence", "This code block is never closed"); }
    c.cur = sv();
}

void sql_line(Ctx &c, sv code, SqlState &st) {
    sv t = trim(code);
    if (t.empty()) return;
    std::string low;
    low.reserve(code.size());
    for (char ch : code) low += (char)tolower((unsigned char)ch);
    // select * and comparisons with NULL
    size_t sp = 0;
    while ((sp = low.find("select", sp)) != std::string::npos) {
        bool left = sp == 0 || !idc(low[sp - 1]);
        size_t e = sp + 6;
        size_t q = e;
        while (q < low.size() && low[q] == ' ') q++;
        if (left && (e >= low.size() || !idc(low[e])) && q < low.size() && low[q] == '*') { add(c, sp, q + 1, SEV_HINT, "sql-select-star", "SELECT * breaks when columns are added or reordered; list the columns"); break; }
        sp = e;
    }
    for (const char *pat : {"= null", "!= null", "<> null"}) {
        size_t p = low.find(pat);
        if (p != std::string::npos && (p + strlen(pat) >= low.size() || !idc(low[p + strlen(pat)]))) { add(c, p, p + strlen(pat), SEV_WARN, "sql-null-compare", "Comparing to NULL with = is never true; use IS NULL / IS NOT NULL"); break; }
    }
    if (!st.open) { st.open = true; st.start_line = c.line; st.start_col = code.find_first_not_of(" \t"); st.stmt.clear(); }
    st.stmt += low + " ";
    if (code.find(';') != sv::npos) {
        sv s = trim(st.stmt);
        bool del = starts(s, "delete"), upd = starts(s, "update");
        if (del || upd) {
            bool where = false;
            for (size_t p = 0; (p = s.find("where", p)) != sv::npos; p += 5) if ((p == 0 || !idc(s[p - 1])) && (p + 5 >= s.size() || !idc(s[p + 5]))) { where = true; break; }
            if (!where && !(s.find("limit") != sv::npos)) {
                Ctx cc{c.out, c.max};
                cc.line = st.start_line; cc.cur = sv();
                add(cc, st.start_col, st.start_col + (del ? 6 : 6), SEV_WARN, del ? "sql-delete-all" : "sql-update-all", std::string(del ? "DELETE" : "UPDATE") + " without WHERE changes every row");
            }
        }
        st.open = false;
        st.stmt.clear();
    }
}

void css_line(Ctx &c, sv code, CssState &st) {
    for (size_t i = 0; i < code.size(); i++) {
        char ch = code[i];
        if (ch == '{') st.blocks.emplace_back();
        else if (ch == '}') { if (!st.blocks.empty()) st.blocks.pop_back(); }
        else if (ch == '#' && !st.blocks.empty()) {
            size_t colon = code.rfind(':', i);
            size_t brace = code.rfind('{', i);
            if (colon == sv::npos || (brace != sv::npos && brace > colon)) continue;
            size_t j = i + 1;
            while (j < code.size() && isxdigit((unsigned char)code[j])) j++;
            size_t len = j - i - 1;
            if (len == 0 || (j < code.size() && (isalnum((unsigned char)code[j]) || code[j] == '-' || code[j] == '_'))) continue;
            if (len != 3 && len != 4 && len != 6 && len != 8) add(c, i, j, SEV_ERROR, "css-color", "A hex colour has 3, 4, 6 or 8 digits, this one has " + std::to_string(len));
        }
    }
}

void misc_line(Ctx &c, const char *lang, sv code) {
    std::string l = lang;
    auto call = [&](sv name, const char *code_id, const char *msg, int sev) {
        size_t p = 0;
        while ((p = code.find(name, p)) != sv::npos) {
            bool left = p == 0 || !(idc(code[p - 1]) || code[p - 1] == '.');
            if (left) { add(c, p, p + name.size(), sev, code_id, msg); return; }
            p += name.size();
        }
    };
    if (l == "Rust") {
        call("dbg!(", "rust-dbg", "Leftover dbg! call", SEV_WARN);
        call("todo!(", "rust-todo", "todo!() panics when it runs", SEV_WARN);
        call("unimplemented!(", "rust-unimplemented", "unimplemented!() panics when it runs", SEV_WARN);
    } else if (l == "Ruby") {
        call("binding.pry", "ruby-debugger", "Leftover debugger call", SEV_WARN);
        call("byebug", "ruby-debugger", "Leftover debugger call", SEV_WARN);
    } else if (l == "PHP") {
        call("var_dump(", "php-debug", "Leftover var_dump", SEV_WARN);
        call("print_r(", "php-debug", "Leftover print_r", SEV_HINT);
        call("eval(", "php-eval", "eval runs arbitrary code", SEV_WARN);
    } else if (l == "Java" || l == "Kotlin" || l == "C#") {
        size_t p = code.find("printStackTrace()");
        if (p != sv::npos) add(c, p, p + 17, SEV_HINT, "java-stack-trace", "printStackTrace() goes to stderr and skips your logging; log the exception");
        size_t cp = code.find("catch");
        if (cp != sv::npos) { size_t ob = code.find('{', cp); if (ob != sv::npos) { size_t k = ob + 1; while (k < code.size() && code[k] == ' ') k++; if (k < code.size() && code[k] == '}') add(c, cp, k + 1, SEV_WARN, "empty-catch", "An empty catch block hides errors"); } }
    } else if (l == "Go") {
        size_t p = code.find("_ = err");
        if (p != sv::npos) add(c, p, p + 7, SEV_HINT, "go-ignored-error", "The error is thrown away");
    }
}

}  // namespace lintp
