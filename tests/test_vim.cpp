/* The vim engine edits a Doc like vim would: operators, motions, objects, registers, dot repeat, undo. */
#include <stdio.h>
#include <gtest/gtest.h>
#include <string>
#include "../src/vim.h"

#define CHECK(c) EXPECT_TRUE(c)

static std::string all(const Doc &d) { std::string s; d.buf.copy(0, d.buf.size(), s); return s; }

struct T {
    Doc d; Vim v; std::string clip;
    explicit T(const char *text) {
        d.load_text(text);
        v.set_clip = [this](const std::string &s) { clip = s; };
        v.get_clip = [this]() { return clip; };
        v.page_rows = [] { return 20; };
        v.reset(true);
    }
    void keys(const char *k) { for (const char *p = k; *p; p++) { if (*p == '\x1b') v.escape(d); else if (v.typing()) v.type_char(d, std::string(1, *p)); else v.text(d, std::string(1, *p)); } }
    std::string s() const { return all(d); }
};


TEST(Vim, Keysdw) {
 T t("one two three\nfour\n"); t.keys("dw"); CHECK(t.s() == "two three\nfour\n");
}

TEST(Vim, KeyswcwXXEsc) {
 T t("one two three\nfour\n"); t.keys("wcwXX\x1b"); CHECK(t.s() == "one XX three\nfour\n");
}

TEST(Vim, Keysddp) {
 T t("a\nb\nc\n"); t.keys("ddp"); CHECK(t.s() == "b\na\nc\n");
}

TEST(Vim, Keysyyjp) {
 T t("a\nb\nc\n"); t.keys("yyjp"); CHECK(t.s() == "a\nb\na\nc\n");
}

TEST(Vim, Keysfadi) {
 T t("x = foo(a, b);\n"); t.keys("fadi("); CHECK(t.s() == "x = foo();\n");
}

TEST(Vim, KeysfhciyoEsc) {
 T t("say \"hi there\" now\n"); t.keys("fhci\"yo\x1b"); CHECK(t.s() == "say \"yo\" now\n");
}

TEST(Vim, Keysdw2) {
 T t("a b c d\n"); t.keys("dw.."); CHECK(t.s() == "d\n");
}

TEST(Vim, Keysxu) {
 T t("abc\n"); t.keys("xu"); CHECK(t.s() == "abc\n"); t.keys("x\x12");
}

TEST(Vim, KeysAabcEsc) {
 T t("hello\n"); t.keys("Aabc\x1b"); CHECK(t.s() == "helloabc\n");
}

TEST(Vim, KeysofooEsc) {
 T t("hello\n"); t.keys("ofoo\x1b"); CHECK(t.s() == "hello\nfoo\n");
}

TEST(Vim, KeysJ) {
 T t("a\nb\n"); t.keys("J"); CHECK(t.s() == "a b\n");
}

TEST(Vim, Keysvee) {
 T t("one two\n"); t.keys("vee"); t.keys("d"); CHECK(t.s() == "\n");
}

TEST(Vim, KeysVjd) {
 T t("a\nb\nc\n"); t.keys("Vjd"); CHECK(t.s() == "c\n");
}

TEST(Vim, Keys3x) {
 T t("a\nb\nc\n"); t.keys("3x"); CHECK(t.s() == "\nb\nc\n");
}

TEST(Vim, Keys2dd) {
 T t("a\nb\nc\n"); t.keys("2dd"); CHECK(t.s() == "c\n");
}

TEST(Vim, Keysjdi) {
 T t("f(x) {\n    y;\n}\n"); t.keys("jdi{"); CHECK(t.s() == "f(x) {\n}\n");
}

TEST(Vim, Keysrx) {
 T t("ab\n"); t.keys("rx"); CHECK(t.s() == "xb\n");
}

TEST(Vim, KeysX) {
 T t("ab\n"); t.keys(">>"); CHECK(t.s() == "    ab\n");
}

TEST(Vim, KeysX2) {
 T t("a b\n"); t.keys("~~~"); CHECK(t.s() == "A B\n");
}

TEST(Vim, KeysgUiw) {
 T t("a b\n"); t.keys("gUiw"); CHECK(t.s() == "A b\n");
}

TEST(Vim, KeysgUU) {
 T t("hello world\n"); t.keys("gUU"); CHECK(t.s() == "HELLO WORLD\n");
}

TEST(Vim, Keysfhds) {
 T t("say \"hi\" ok\n"); t.keys("fhds\""); CHECK(t.s() == "say hi ok\n");
}

TEST(Vim, Keysfhcs) {
 T t("say \"hi\" ok\n"); t.keys("fhcs\"'"); CHECK(t.s() == "say 'hi' ok\n");
}

TEST(Vim, Keysysiw) {
 T t("word\n"); t.keys("ysiw)"); CHECK(t.s() == "(word)\n");
}

TEST(Vim, Keysyssb) {
 T t("word\n"); t.keys("yssb"); CHECK(t.s() == "(word)\n");
}

TEST(Vim, KeysqaAEscjq) {
 T t("a\nb\n"); t.keys("qaA!\x1bjq"); CHECK(t.s() == "a!\nb\n"); t.keys("@a"); CHECK(t.s() == "a!\nb!\n");
}

TEST(Vim, Keysayyjap) {
 T t("a\nb\n"); t.keys("\"ayyj\"ap"); CHECK(t.s() == "a\nb\na\n");
}

TEST(Vim, Keysdap) {
 T t("x\ny\n\nz\n"); t.keys("dap"); CHECK(t.s() == "z\n");
}

TEST(Vim, Keysmajjda) {
 T t("a\nb\nc\n"); t.keys("majjd'a"); CHECK(t.s() == "");
}

TEST(Vim, KeysX3) {
 T t("n 9 x\n"); t.keys(""); t.v.increment(t.d, 1); CHECK(t.s() == "n 10 x\n");
}

TEST(Vim, Keysgex) {
 T t("foo bar\n"); t.keys("$gex"); CHECK(t.s() == "fo bar\n");
}

TEST(Vim, KeysRXYEsc) {
 T t("abcdef\n"); t.keys("RXY\x1b"); CHECK(t.s() == "XYcdef\n"); t.keys("u"); CHECK(t.s() == "abcdef\n");
}

TEST(Vim, KeysRXYZEsc) {
 T t("abc\n"); t.keys("$RXYZ\x1b"); CHECK(t.s() == "abXYZ\n");
}

TEST(Vim, KeysRXY) {
 T t("abcdef\n"); t.keys("RXY"); t.v.backspace(t.d); t.keys("\x1b"); CHECK(t.s() == "Xbcdef\n");
}

TEST(Vim, KeysRXYEscl) {
 T t("abcdef\n"); t.keys("RXY\x1bl."); CHECK(t.s() == "XYXYef\n");
}

TEST(Vim, KeyslCtrlVjld) {
 T t("abcd\nefgh\nijkl\n"); t.keys("l\x16jld"); CHECK(t.s() == "ad\neh\nijkl\n");
}

TEST(Vim, KeyslCtrlVjlyp) {
 T t("abcd\nefgh\nijkl\n"); t.keys("l\x16jly$p"); CHECK(t.s() == "abcdbc\nefghfg\nijkl\n");
}

TEST(Vim, KeysCtrlVjjIXEsc) {
 T t("abcd\nefgh\nijkl\n"); t.keys("\x16jjIX\x1b"); CHECK(t.s() == "Xabcd\nXefgh\nXijkl\n");
}

TEST(Vim, KeysCtrlVjjAXEsc) {
 T t("abc\nde\nfgh\n"); t.keys("\x16jj$AX\x1b"); CHECK(t.s() == "abcX\ndeX\nfghX\n");
}

TEST(Vim, KeyslCtrlVjlcZEsc) {
 T t("abcd\nefgh\n"); t.keys("l\x16jlcZ\x1b"); CHECK(t.s() == "aZd\neZh\n");
}

TEST(Vim, KeysCtrlVjlrx) {
 T t("abcd\nefgh\n"); t.keys("\x16jlrx"); CHECK(t.s() == "xxcd\nxxgh\n");
}

TEST(Vim, KeysCtrlVjlU) {
 T t("abcd\nefgh\n"); t.keys("\x16jlU"); CHECK(t.s() == "ABcd\nEFgh\n");
}

TEST(Vim, KeysCtrlVjy) {
 T t("ab\ncd\n"); t.keys("\x16jy"); t.keys("$p"); CHECK(t.s() == "aba\ncdc\n");
}

TEST(Vim, Keysjdd) {
{ T t("a\nb\n"); t.keys("jdd"); CHECK(t.s() == "a\n"); }   /* the buffer ends with a newline: that is not an extra line to join */
}

TEST(Vim, Keysjdd2) {
 T t("a\nb"); t.keys("jdd"); CHECK(t.s() == "a");
}
