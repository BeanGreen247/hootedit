/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include "../src/preview.h"

TEST(Preview, DetectsSupportedExtensions) {
    EXPECT_EQ(preview_format("README.MD"), PreviewFormat::Markdown);
    EXPECT_EQ(preview_format("guide.adoc"), PreviewFormat::AsciiDoc);
    EXPECT_EQ(preview_format("guide.rst"), PreviewFormat::ReStructuredText);
    EXPECT_EQ(preview_format("workflow.mmd"), PreviewFormat::Mermaid);
    EXPECT_EQ(preview_format("workflow.mermaid"), PreviewFormat::Mermaid);
    EXPECT_EQ(preview_format("source.cpp"), PreviewFormat::None);
}

TEST(Preview, ParsesMarkdownBlocksAndFencedCode) {
    auto blocks = preview_parse("# Guide\n\nA **bold** paragraph.\n\n- first\n1. second\n\n```python\nprint(1)\n```\n", PreviewFormat::Markdown);
    ASSERT_EQ(blocks.size(), 5u);
    EXPECT_EQ(blocks[0].kind, PreviewBlockKind::Heading);
    EXPECT_EQ(blocks[0].level, 1);
    EXPECT_EQ(blocks[1].text, "A **bold** paragraph.");
    EXPECT_EQ(blocks[2].kind, PreviewBlockKind::List);
    EXPECT_EQ(blocks[3].kind, PreviewBlockKind::List);
    EXPECT_EQ(blocks[4].kind, PreviewBlockKind::Code);
    EXPECT_EQ(blocks[4].language, "python");
    ASSERT_EQ(blocks[4].lines.size(), 1u);
    EXPECT_EQ(blocks[4].lines[0], "print(1)");
}

TEST(Preview, ParsesAllMarkdownHeadingLevelsAndPreservesCase) {
    auto blocks = preview_parse("# P p\n## P p ##\n### P p ###\n#### P p\n##### P p\n###### P p\n# C#\n   ## Indented P p\nAlt-H1\n======\nAlt-H2\n------\n", PreviewFormat::Markdown);
    ASSERT_EQ(blocks.size(), 10u);
    for (size_t i = 0; i < 6; i++) {
        EXPECT_EQ(blocks[i].kind, PreviewBlockKind::Heading);
        EXPECT_EQ(blocks[i].level, (int)i + 1);
        EXPECT_EQ(blocks[i].text, "P p");
    }
    EXPECT_EQ(blocks[6].text, "C#");
    EXPECT_EQ(blocks[7].level, 2);
    EXPECT_EQ(blocks[7].text, "Indented P p");
    EXPECT_EQ(blocks[8].level, 1);
    EXPECT_EQ(blocks[9].level, 2);
}

TEST(Preview, ParsesAsciiDocSourceAndLists) {
    auto blocks = preview_parse("= Guide\n\n[source,python]\n----\nprint(1)\n----\n\n* item\n", PreviewFormat::AsciiDoc);
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].kind, PreviewBlockKind::Heading);
    EXPECT_EQ(blocks[1].kind, PreviewBlockKind::Code);
    EXPECT_EQ(blocks[1].language, "python");
    EXPECT_EQ(blocks[2].kind, PreviewBlockKind::List);
}

TEST(Preview, ParsesRstHeadingsDirectivesAndLists) {
    auto blocks = preview_parse("Guide\n=====\n\n.. code-block:: python\n\n   print(1)\n\n- item\n", PreviewFormat::ReStructuredText);
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].kind, PreviewBlockKind::Heading);
    EXPECT_EQ(blocks[0].level, 1);
    EXPECT_EQ(blocks[1].kind, PreviewBlockKind::Code);
    ASSERT_EQ(blocks[1].lines.size(), 1u);
    EXPECT_EQ(blocks[1].lines[0], "print(1)");
    EXPECT_EQ(blocks[2].kind, PreviewBlockKind::List);
}

TEST(Preview, DiagnosesBrokenLocalReferencesAndRawHtmlButNotCodeSamples) {
    std::string source = "![missing](assets/nope.png)\n\n<div>raw</div>\n\n```html\n<div>example</div>\n```\n";
    auto diagnostics = preview_diagnose(source, PreviewFormat::Markdown, "/docs/README.md");
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[0].line, 1u);
    EXPECT_NE(diagnostics[0].message.find("assets/nope.png"), std::string::npos);
    EXPECT_EQ(diagnostics[1].line, 3u);
    EXPECT_NE(diagnostics[1].message.find("raw HTML"), std::string::npos);
}

TEST(Preview, ParsesTablesWithDividerAndTaskLists) {
    auto blocks = preview_parse("| Name | Score |\n| :--- | ---: |\n| Ada | 10 |\n\n- [x] done\n- [ ] todo\n", PreviewFormat::Markdown);
    ASSERT_EQ(blocks.size(), 3u);
    ASSERT_EQ(blocks[0].kind, PreviewBlockKind::Table);
    ASSERT_EQ(blocks[0].lines.size(), 3u);
    EXPECT_EQ(blocks[0].lines[1], "| :--- | ---: |");
    EXPECT_EQ(blocks[1].kind, PreviewBlockKind::List);
    EXPECT_TRUE(blocks[1].task);
    EXPECT_TRUE(blocks[1].checked);
    EXPECT_EQ(blocks[1].text, "done");
    EXPECT_TRUE(blocks[2].task);
    EXPECT_FALSE(blocks[2].checked);
}

TEST(Preview, ParsesAsciiDocAndRstTables) {
    auto adoc = preview_parse("[options=\"header\"]\n|===\n|Name |Score\n|Ada |10\n|===\n", PreviewFormat::AsciiDoc);
    ASSERT_EQ(adoc.size(), 1u);
    ASSERT_EQ(adoc[0].kind, PreviewBlockKind::Table);
    EXPECT_EQ(adoc[0].level, 1);
    ASSERT_EQ(adoc[0].lines.size(), 2u);

    auto rst = preview_parse("+------+------+\n| Name | Score|\n+======+======+\n| Ada  | 10   |\n+------+------+\n", PreviewFormat::ReStructuredText);
    ASSERT_EQ(rst.size(), 1u);
    ASSERT_EQ(rst[0].kind, PreviewBlockKind::Table);
    EXPECT_EQ(rst[0].level, 1);
    ASSERT_EQ(rst[0].lines.size(), 2u);
}

TEST(Preview, ParsesFencedAndStandaloneMermaidAsDiagrams) {
    auto fenced = preview_parse("```mermaid\nflowchart TD\n  A[Start] --> B{Ready?}\n  B -->|yes| C[Done]\n```\n", PreviewFormat::Markdown);
    ASSERT_EQ(fenced.size(), 1u);
    EXPECT_EQ(fenced[0].kind, PreviewBlockKind::Diagram);
    EXPECT_EQ(fenced[0].lines.size(), 3u);
    auto standalone = preview_parse("sequenceDiagram\n  Alice->>Bob: Hello\n", PreviewFormat::Mermaid);
    ASSERT_EQ(standalone.size(), 1u);
    EXPECT_EQ(standalone[0].kind, PreviewBlockKind::Diagram);
}
