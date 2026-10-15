/* The vim engine edits a Doc like vim would: operators, motions, objects, registers, dot repeat, undo. */
#include <stdio.h>
#include <string>
#include "../src/vim.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

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
    void keys(const char *k) { for (const char *p = k; *p; p++) { if (*p == '\x1b') v.escape(d); else if (v.mode == Vim::INSERT) { d.insert_text(p, 1); v.typed(std::string(1, *p)); } else v.text(d, std::string(1, *p)); } }
    std::string s() const { return all(d); }
};

int main() {
    { T t("one two three\nfour\n"); t.keys("dw"); CHECK(t.s() == "two three\nfour\n"); }
    { T t("one two three\nfour\n"); t.keys("wcwXX\x1b"); CHECK(t.s() == "one XX three\nfour\n"); }
    { T t("a\nb\nc\n"); t.keys("ddp"); CHECK(t.s() == "b\na\nc\n"); }
    { T t("a\nb\nc\n"); t.keys("yyjp"); CHECK(t.s() == "a\nb\na\nc\n"); }
    { T t("x = foo(a, b);\n"); t.keys("fadi("); CHECK(t.s() == "x = foo();\n"); }
    { T t("say \"hi there\" now\n"); t.keys("fhci\"yo\x1b"); CHECK(t.s() == "say \"yo\" now\n"); }
    { T t("a b c d\n"); t.keys("dw.."); CHECK(t.s() == "d\n"); }
    { T t("abc\n"); t.keys("xu"); CHECK(t.s() == "abc\n"); t.keys("x\x12"); }
    { T t("hello\n"); t.keys("Aabc\x1b"); CHECK(t.s() == "helloabc\n"); }
    { T t("hello\n"); t.keys("ofoo\x1b"); CHECK(t.s() == "hello\nfoo\n"); }
    { T t("a\nb\n"); t.keys("J"); CHECK(t.s() == "a b\n"); }
    { T t("one two\n"); t.keys("vee"); t.keys("d"); CHECK(t.s() == "\n"); }
    { T t("a\nb\nc\n"); t.keys("Vjd"); CHECK(t.s() == "c\n"); }
    { T t("a\nb\nc\n"); t.keys("3x"); CHECK(t.s() == "\nb\nc\n"); }
    { T t("a\nb\nc\n"); t.keys("2dd"); CHECK(t.s() == "c\n"); }
    { T t("f(x) {\n    y;\n}\n"); t.keys("jdi{"); CHECK(t.s() == "f(x) {\n}\n"); }
    { T t("ab\n"); t.keys("rx"); CHECK(t.s() == "xb\n"); }
    { T t("ab\n"); t.keys(">>"); CHECK(t.s() == "    ab\n"); }
    { T t("a b\n"); t.keys("~~~"); CHECK(t.s() == "A B\n"); }
    { T t("a b\n"); t.keys("gUiw"); CHECK(t.s() == "A b\n"); }
    { T t("hello world\n"); t.keys("gUU"); CHECK(t.s() == "HELLO WORLD\n"); }
    { T t("say \"hi\" ok\n"); t.keys("fhds\""); CHECK(t.s() == "say hi ok\n"); }
    { T t("say \"hi\" ok\n"); t.keys("fhcs\"'"); CHECK(t.s() == "say 'hi' ok\n"); }
    { T t("word\n"); t.keys("ysiw)"); CHECK(t.s() == "(word)\n"); }
    { T t("word\n"); t.keys("yssb"); CHECK(t.s() == "(word)\n"); }
    { T t("a\nb\n"); t.keys("qaA!\x1bjq"); CHECK(t.s() == "a!\nb\n"); t.keys("@a"); CHECK(t.s() == "a!\nb!\n"); }
    { T t("a\nb\n"); t.keys("\"ayyj\"ap"); CHECK(t.s() == "a\nb\na\n"); }
    { T t("x\ny\n\nz\n"); t.keys("dap"); CHECK(t.s() == "z\n"); }
    { T t("a\nb\nc\n"); t.keys("majjd'a"); CHECK(t.s() == ""); }
    { T t("n 9 x\n"); t.keys(""); t.v.increment(t.d, 1); CHECK(t.s() == "n 10 x\n"); }
    { T t("foo bar\n"); t.keys("$gex"); CHECK(t.s() == "fo bar\n"); }
    if (fails == 0) printf("vim ok\n");
    return fails ? 1 : 0;
}
