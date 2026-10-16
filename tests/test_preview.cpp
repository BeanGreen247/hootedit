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
