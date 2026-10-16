/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "preview.h"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>

namespace {

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool blank(const std::string &s) { return trim(s).empty(); }

std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        out.push_back(s.substr(p, e - p));
        p = e + (e < s.size());
    }
    if (s.empty() || s.back() == '\n') out.emplace_back();
    return out;
}

bool starts(const std::string &s, const char *p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

std::string strip_hashes(const std::string &s, size_t n) {
    std::string out = trim(s.substr(n));
    while (!out.empty() && out.back() == '#') out.pop_back();
    return trim(out);
}

bool md_fence(const std::string &s, char *mark, size_t *count, std::string *lang) {
    std::string t = trim(s);
    if (t.empty() || (t[0] != '`' && t[0] != '~')) return false;
    size_t n = 0;
    while (n < t.size() && t[n] == t[0]) n++;
    if (n < 3) return false;
    *mark = t[0];
    *count = n;
    *lang = trim(t.substr(n));
    return true;
}

bool underline(const std::string &s, char *mark) {
    std::string t = trim(s);
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '=' && c != '-' && c != '~' && c != '^' && c != '+' && c != '#') return false;
    for (char x : t) if (x != c) return false;
    *mark = c;
    return true;
}

bool list_marker(const std::string &s, int *indent, bool *ordered, std::string *text, PreviewFormat format) {
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    std::string t = s.substr(p);
    if (format == PreviewFormat::Markdown) {
        if (t.size() > 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && (t[1] == ' ' || t[1] == '\t')) {
            *indent = (int)p; *ordered = false; *text = trim(t.substr(2)); return true;
        }
        size_t n = 0;
        while (n < t.size() && std::isdigit((unsigned char)t[n])) n++;
        if (n && n + 1 < t.size() && (t[n] == '.' || t[n] == ')') && (t[n + 1] == ' ' || t[n + 1] == '\t')) {
            *indent = (int)p; *ordered = true; *text = trim(t.substr(n + 2)); return true;
        }
    } else if (format == PreviewFormat::AsciiDoc) {
        if (t.size() > 1 && (t[0] == '*' || t[0] == '-') && (t[1] == ' ' || t[1] == '\t')) {
            *indent = (int)p; *ordered = false; *text = trim(t.substr(2)); return true;
        }
        if (t.size() > 2 && t[0] == '.' && (t[1] == ' ' || t[1] == '\t')) {
            *indent = (int)p; *ordered = true; *text = trim(t.substr(2)); return true;
        }
    } else {
        if (t.size() > 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && (t[1] == ' ' || t[1] == '\t')) {
            *indent = (int)p; *ordered = false; *text = trim(t.substr(2)); return true;
        }
        size_t n = 0;
        while (n < t.size() && std::isdigit((unsigned char)t[n])) n++;
        if (n && n + 1 < t.size() && t[n] == '.' && (t[n + 1] == ' ' || t[n + 1] == '\t')) {
            *indent = (int)p; *ordered = true; *text = trim(t.substr(n + 2)); return true;
        }
    }
    return false;
}

bool is_md_rule(const std::string &s) {
    std::string t = trim(s);
    char c = 0;
    int n = 0;
    for (char x : t) if (x != ' ' && x != '\t') {
        if (!c) c = x;
        if (c != x || (x != '-' && x != '*' && x != '_')) return false;
        n++;
    }
    return n >= 3;
}

bool is_table_divider(const std::string &s) {
    bool pipe = false, dash = false;
    for (char c : s) {
        if (c == '|') pipe = true;
        else if (c == '-') dash = true;
        else if (c != ':' && c != ' ' && c != '\t') return false;
    }
    return pipe && dash;
}

void append_paragraph(std::vector<PreviewBlock> &out, std::string &text) {
    text = trim(text);
    if (!text.empty()) out.push_back({PreviewBlockKind::Paragraph, 0, false, std::move(text), {}, {}});
    text.clear();
}

} // namespace

PreviewFormat preview_format(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return PreviewFormat::None;
    std::string ext = path.substr(dot);
    for (char &c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".md" || ext == ".markdown" || ext == ".mdown" || ext == ".mkd") return PreviewFormat::Markdown;
    if (ext == ".adoc" || ext == ".asciidoc" || ext == ".asc") return PreviewFormat::AsciiDoc;
    if (ext == ".rst" || ext == ".rest") return PreviewFormat::ReStructuredText;
    return PreviewFormat::None;
}

std::vector<PreviewBlock> preview_parse(const std::string &source, PreviewFormat format) {
    std::vector<PreviewBlock> out;
    if (format == PreviewFormat::None) return out;
    std::vector<std::string> lines = split_lines(source);
    std::string paragraph;
    std::string pending_lang;
    bool table = false, code = false, rst_code = false;
    char fence = 0;
    size_t fence_len = 0;
    int rst_indent = 0;
    PreviewBlock block;
    auto close_table = [&] { if (table) { out.push_back(std::move(block)); block = {}; table = false; } };
    auto close_code = [&] { if (code) { while (!block.lines.empty() && blank(block.lines.back())) block.lines.pop_back(); out.push_back(std::move(block)); block = {}; code = false; } };

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &line = lines[i];
        if (format == PreviewFormat::Markdown && code) {
            char m = 0; size_t n = 0; std::string lang;
            if (md_fence(line, &m, &n, &lang) && m == fence && n >= fence_len) { close_code(); continue; }
            block.lines.push_back(line);
            continue;
        }
        if (format == PreviewFormat::AsciiDoc && code) {
            if (trim(line) == "----") { close_code(); continue; }
            block.lines.push_back(line);
            continue;
        }
        if (format == PreviewFormat::ReStructuredText && rst_code) {
            if (blank(line)) { if (!block.lines.empty()) block.lines.emplace_back(); continue; }
            int n = 0; while ((size_t)n < line.size() && line[(size_t)n] == ' ') n++;
            if (rst_indent < 0) { rst_indent = n; block.lines.push_back(line.substr((size_t)n)); continue; }
            if (n < rst_indent) { close_code(); rst_code = false; i--; continue; }
            block.lines.push_back(line.substr((size_t)rst_indent));
            continue;
        }
        if (blank(line)) { append_paragraph(out, paragraph); close_table(); continue; }

        if (format == PreviewFormat::Markdown) {
            char m = 0; size_t n = 0; std::string lang;
            if (md_fence(line, &m, &n, &lang)) {
                append_paragraph(out, paragraph); close_table();
                code = true; fence = m; fence_len = n;
                block.kind = PreviewBlockKind::Code; block.language = lang;
                continue;
            }
            size_t h = 0; while (h < line.size() && line[h] == '#') h++;
            if (h && h <= 6 && h < line.size() && line[h] == ' ') {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Heading, (int)h, false, strip_hashes(line, h), {}, {}});
                continue;
            }
            if (is_md_rule(line)) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Rule}); continue;
            }
            if (i + 1 < lines.size() && !blank(lines[i + 1]) && lines[i + 1].size() >= 2) {
                char c = lines[i + 1][0];
                std::string u = trim(lines[i + 1]);
                if ((c == '=' || c == '-') && u.size() >= 2 && u.find_first_not_of(c) == std::string::npos) {
                    append_paragraph(out, paragraph); close_table();
                    out.push_back({PreviewBlockKind::Heading, c == '=' ? 1 : 2, false, trim(line), {}, {}});
                    i++; continue;
                }
            }
            if (starts(trim(line), ">")) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Quote, 0, false, trim(line).substr(1), {}, {}}); continue;
            }
            int indent = 0; bool ordered = false; std::string item;
            if (list_marker(line, &indent, &ordered, &item, format)) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::List, indent / 2, ordered, item, {}, {}}); continue;
            }
            if (line.find('|') != std::string::npos && i + 1 < lines.size() && is_table_divider(lines[i + 1])) {
                append_paragraph(out, paragraph); close_table();
                table = true; block.kind = PreviewBlockKind::Table;
                block.lines.push_back(line);
                i++; // The divider is formatting, not a data row.
                continue;
            }
            if (table && line.find('|') != std::string::npos) { block.lines.push_back(line); continue; }
        } else if (format == PreviewFormat::AsciiDoc) {
            std::string t = trim(line);
            if (t == "----") {
                append_paragraph(out, paragraph); close_table();
                code = true; block.kind = PreviewBlockKind::Code; block.language = pending_lang; pending_lang.clear();
                continue;
            }
            if (starts(t, "[source,") && t.back() == ']') { pending_lang = t.substr(8, t.size() - 9); continue; }
            if (starts(t, "//")) continue;
            size_t h = 0; while (h < t.size() && t[h] == '=') h++;
            if (h && h <= 6 && h < t.size() && t[h] == ' ') {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Heading, (int)h, false, trim(t.substr(h)), {}, {}}); continue;
            }
            if (starts(t, "image::")) {
                append_paragraph(out, paragraph); close_table();
                std::string image = trim(t.substr(7));
                size_t alt = image.find('[');
                if (alt != std::string::npos) image.resize(alt);
                out.push_back({PreviewBlockKind::Quote, 0, false, "[Image: " + trim(image) + "]", {}, {}}); continue;
            }
            if (is_md_rule(t)) { append_paragraph(out, paragraph); close_table(); out.push_back({PreviewBlockKind::Rule}); continue; }
            int indent = 0; bool ordered = false; std::string item;
            if (list_marker(line, &indent, &ordered, &item, format)) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::List, indent / 2, ordered, item, {}, {}}); continue;
            }
        } else {
            std::string t = trim(line);
            if (starts(t, ".. code-block::") || starts(t, ".. sourcecode::")) {
                append_paragraph(out, paragraph); close_table();
                size_t pos = t.find("::");
                block.kind = PreviewBlockKind::Code;
                block.language = trim(t.substr(pos + 2));
                code = rst_code = true; rst_indent = -1;
                continue;
            }
            if (starts(t, ".. note::") || starts(t, ".. warning::") || starts(t, ".. important::")) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Quote, 0, false, trim(t.substr(t.find("::") + 2)), {}, {}}); continue;
            }
            if (starts(t, ".. image::")) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Quote, 0, false, "[Image: " + trim(t.substr(10)) + "]", {}, {}}); continue;
            }
            if (starts(t, ".. ")) continue;
            if (i + 1 < lines.size() && !blank(lines[i + 1])) {
                char c = 0;
                if (underline(lines[i + 1], &c)) {
                    append_paragraph(out, paragraph); close_table();
                    int level = c == '=' ? 1 : c == '-' ? 2 : c == '~' ? 3 : 4;
                    out.push_back({PreviewBlockKind::Heading, level, false, trim(line), {}, {}});
                    i++; continue;
                }
            }
            if (starts(t, "+----") || starts(t, "====") || starts(t, "----")) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::Rule}); continue;
            }
            int indent = 0; bool ordered = false; std::string item;
            if (list_marker(line, &indent, &ordered, &item, format)) {
                append_paragraph(out, paragraph); close_table();
                out.push_back({PreviewBlockKind::List, indent / 2, ordered, item, {}, {}}); continue;
            }
        }
        close_table();
        if (!paragraph.empty()) paragraph += ' ';
        paragraph += trim(line);
    }
    close_code();
    append_paragraph(out, paragraph);
    close_table();
    return out;
}

std::vector<PreviewDiagnostic> preview_diagnose(const std::string &source, PreviewFormat format, const std::string &document_path) {
    std::vector<PreviewDiagnostic> out;
    if (format == PreviewFormat::None) return out;
    std::vector<std::string> lines = split_lines(source);
    std::string dir = ".";
    size_t slash = document_path.find_last_of('/');
    if (slash != std::string::npos) dir = document_path.substr(0, slash);
    auto add_local_ref = [&](size_t line, std::string target) {
        target = trim(target);
        if (target.empty() || target[0] == '#' || starts(target, "//") || target.find("://") != std::string::npos ||
            starts(target, "mailto:") || starts(target, "data:")) return;
        if (target.size() > 1 && target[0] == '<' && target.back() == '>') target = target.substr(1, target.size() - 2);
        size_t suffix = target.find_first_of("?# ");
        if (suffix != std::string::npos) target.resize(suffix);
        if (target.empty()) return;
        std::string path = target[0] == '/' ? target : dir + "/" + target;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0)
            out.push_back({line, "local reference not found: " + target});
    };
    bool in_md_fence = false, in_adoc_code = false, in_rst_code = false;
    char md_fence_mark = 0;
    size_t md_fence_len = 0;
    int rst_code_indent = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &line = lines[i];
        if (format == PreviewFormat::Markdown) {
            char mark = 0; size_t count = 0; std::string language;
            if (md_fence(line, &mark, &count, &language)) {
                if (!in_md_fence) { in_md_fence = true; md_fence_mark = mark; md_fence_len = count; }
                else if (mark == md_fence_mark && count >= md_fence_len) in_md_fence = false;
                continue;
            }
            if (in_md_fence) continue;
        }
        if (format == PreviewFormat::AsciiDoc) {
            if (trim(line) == "----") { in_adoc_code = !in_adoc_code; continue; }
            if (in_adoc_code) continue;
        }
        if (format == PreviewFormat::ReStructuredText) {
            std::string t = trim(line);
            if (starts(t, ".. code-block::") || starts(t, ".. sourcecode::")) { in_rst_code = true; rst_code_indent = -1; continue; }
            if (in_rst_code) {
                if (blank(line)) continue;
                int indent = 0; while ((size_t)indent < line.size() && line[(size_t)indent] == ' ') indent++;
                if (rst_code_indent < 0) { rst_code_indent = indent; continue; }
                if (indent >= rst_code_indent) continue;
                in_rst_code = false;
            }
        }
        size_t tag = line.find('<');
        if (tag != std::string::npos && tag + 1 < line.size() &&
            (std::isalpha((unsigned char)line[tag + 1]) || line[tag + 1] == '/' || line[tag + 1] == '!')) {
            out.push_back({i + 1, "raw HTML is displayed as text in this native preview"});
        }
        if (format == PreviewFormat::Markdown) {
            size_t p = 0;
            while ((p = line.find("](", p)) != std::string::npos) {
                size_t end = line.find(')', p + 2);
                if (end == std::string::npos) break;
                add_local_ref(i + 1, line.substr(p + 2, end - p - 2));
                p = end + 1;
            }
            if (line.find("][") != std::string::npos || line.find("[^") != std::string::npos)
                out.push_back({i + 1, "reference-style links and footnotes are not rendered"});
        } else if (format == PreviewFormat::AsciiDoc) {
            size_t p = 0;
            while ((p = line.find("image::", p)) != std::string::npos) {
                size_t end = line.find('[', p + 7);
                add_local_ref(i + 1, line.substr(p + 7, end == std::string::npos ? std::string::npos : end - p - 7));
                p += 7;
            }
            p = 0;
            while ((p = line.find("link:", p)) != std::string::npos) {
                size_t end = line.find('[', p + 5);
                add_local_ref(i + 1, line.substr(p + 5, end == std::string::npos ? std::string::npos : end - p - 5));
                p += 5;
            }
            if (line.find("pass:[") != std::string::npos) out.push_back({i + 1, "AsciiDoc passthrough macros are not executed"});
        } else if (starts(trim(line), ".. image::")) {
            add_local_ref(i + 1, trim(line).substr(10));
        } else if (starts(trim(line), ".. toctree::") || starts(trim(line), ".. raw::")) {
            out.push_back({i + 1, "this reStructuredText directive is not rendered"});
        }
    }
    return out;
}
