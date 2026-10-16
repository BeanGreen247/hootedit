/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_PREVIEW_H
#define HE_PREVIEW_H

#include <string>
#include <vector>

enum class PreviewFormat { None, Markdown, AsciiDoc, ReStructuredText, Mermaid };
enum class PreviewBlockKind { Paragraph, Heading, Code, Diagram, Quote, List, Rule, Table };

struct PreviewBlock {
    PreviewBlockKind kind = PreviewBlockKind::Paragraph;
    int level = 0;
    bool ordered = false;
    std::string text;
    std::string language;
    std::vector<std::string> lines;
    bool task = false;
    bool checked = false;
};

struct PreviewDiagnostic {
    size_t line = 0;
    std::string message;
};

PreviewFormat preview_format(const std::string &path);
std::vector<PreviewBlock> preview_parse(const std::string &source, PreviewFormat format);
std::vector<PreviewDiagnostic> preview_diagnose(const std::string &source, PreviewFormat format, const std::string &document_path);

#endif
