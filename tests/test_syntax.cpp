/* The lexer: expected token classes per language, language detection. */
#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include <vector>
#include "../src/syntax.h"

static std::string render(const Lang *l, const char *s, uint8_t in, uint8_t *out) {
    std::vector<uint8_t> t(strlen(s) + 1);
    *out = lex_line(l, s, strlen(s), in, t.data());
    std::string r;
    for (size_t i = 0; i < strlen(s); i++) r += "pcsnkTPoFKHID"[t[i]];   /* plain comment string number keyword Type Preproc punct Func Key Heading Ins Del */
    return r;
}
static void expect(const char *lang, const char *src, const char *want, uint8_t in = 0, int want_state = -1) {
    const Lang *l = nullptr;
    for (int i = 0; i < lang_count(); i++) if (!strcmp(lang_name(lang_at(i)), lang)) l = lang_at(i);
    ASSERT_NE(l, nullptr) << "no language " << lang;
    uint8_t out;
    std::string got = render(l, src, in, &out);
    EXPECT_TRUE(got == want && (want_state < 0 || out == want_state)) << lang << ": " << src << "\n  want " << want << "\n  got  " << got << " (state " << (int)out << ")";
}

TEST(Syntax, C) {
    expect("C", "int x = 42; // hi", "TTTpppopnnopccccc");
    expect("C", "#include <stdio.h>", "PPPPPPPPpsssssssss", 0);
    expect("C", "/* a", "cccc", 0, 1);
    expect("C", "b */ x", "ccccpp", 1, 0);
}
TEST(Syntax, Python) {
    expect("Python", "def f(x): return 'a'", "kkkpFopoopkkkkkkpsss");
    expect("Python", "x = \"\"\"abc", "ppopssssss", 0, 2);
    expect("Python", "abc\"\"\" + 1", "sssssspopn", 2, 0);
}
TEST(Syntax, DataAndMarkupFormats) {
    expect("JSON", "{\"a\": 1, \"b\": \"c\"}", "oKKKopnopKKKopssso");
    expect("YAML", "name: value # c", "KKKKopppppppccc");
    expect("Markdown", "# Title", "HHHHHHH");
    expect("Markdown", "```c", "kkkk", 0, 6);
    expect("Markdown", "int x;", "ssssss", 6, 6);
    expect("Diff", "+added", "IIIIII");
    expect("Diff", "-gone", "DDDDD");
    expect("HTML", "<a href=\"x\">t</a>", "kkpTTTTpssskpkkkk");
}
TEST(Syntax, LanguageDetection) {
    EXPECT_EQ(lang_detect("/x/main.cpp", nullptr, 0), lang_at(2));
    EXPECT_STREQ(lang_name(lang_detect("Makefile", nullptr, 0)), "Makefile");
    const char *sb = "#!/usr/bin/env python3\n";
    EXPECT_STREQ(lang_name(lang_detect("script", sb, strlen(sb) - 1)), "Python");
}
