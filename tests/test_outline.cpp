/* The outline scan finds types, functions and markdown headings, with nesting from indentation, and skips calls and statements. */
#include <stdio.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "../src/outline.h"

#define CHECK(c) EXPECT_TRUE(c)

static std::vector<OItem> scan(const char *text, const char *path = "x.c") {
    Doc d;
    d.load_text(text);
    d.path = path;
    d.redetect();
    std::vector<OItem> v;
    outline_scan(d, v);
    return v;
}

static std::string names(const std::vector<OItem> &v) { std::string s; for (const OItem &i : v) { if (!s.empty()) s += ","; s += i.name; } return s; }


TEST(Outline, Case1) {

        auto v = scan("#include <x.h>\n\nstruct Point {\n    int x;\n};\n\nstatic int add(int a, int b) {\n    return foo(a) + b;\n}\n\nint main(void) {\n    printf(\"x\");\n    if (a) {\n    }\n    return 0;\n}\n");
        CHECK(names(v) == "Point,add,main");
        CHECK(v.size() == 3 && v[0].kind == 'C' && v[1].kind == 'F' && v[2].line == 10);
}

TEST(Outline, Case2) {

        auto v = scan("class A:\n    def f(self):\n        pass\n    def g(self):\n        pass\n\ndef top():\n    pass\n", "x.py");
        CHECK(names(v) == "A,f,g,top");
        CHECK(v[1].depth == 1 && v[3].depth == 0);
}

TEST(Outline, Case3) {

        auto v = scan("# Title\ntext\n## Sub\n### Deep\n#nospace\n", "x.md");
        CHECK(names(v) == "Title,Sub,Deep");
        CHECK(v[0].depth == 0 && v[2].depth == 2 && v[0].kind == 'H');
}

TEST(Outline, Case4) {

        auto v = scan("pub fn run() {}\nimpl Foo {\n    fn bar(&self) {}\n}\n", "x.rs");
        CHECK(names(v) == "run,Foo,bar");
}

TEST(Outline, Case5) {

        auto v = scan("class Foo;\nstruct Bar;\nint x = f(1);\nreturn g(2);\nint Foo::method(int a) const {\n}\n");
        CHECK(names(v) == "Foo::method");
}

TEST(Outline, Case6) {

        auto v = scan("func (s *Server) Start() error {\n}\nfunction hello() {\n}\n", "x.go");
        CHECK(names(v) == "Start,hello");
}
