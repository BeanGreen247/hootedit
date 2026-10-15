/* The outline scan finds types, functions and markdown headings, with nesting from indentation, and skips calls and statements. */
#include <stdio.h>
#include <string>
#include <vector>
#include "../src/outline.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

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

int main() {
    {
        auto v = scan("#include <x.h>\n\nstruct Point {\n    int x;\n};\n\nstatic int add(int a, int b) {\n    return foo(a) + b;\n}\n\nint main(void) {\n    printf(\"x\");\n    if (a) {\n    }\n    return 0;\n}\n");
        CHECK(names(v) == "Point,add,main");
        CHECK(v.size() == 3 && v[0].kind == 'C' && v[1].kind == 'F' && v[2].line == 10);
    }
    {
        auto v = scan("class A:\n    def f(self):\n        pass\n    def g(self):\n        pass\n\ndef top():\n    pass\n", "x.py");
        CHECK(names(v) == "A,f,g,top");
        CHECK(v[1].depth == 1 && v[3].depth == 0);
    }
    {
        auto v = scan("# Title\ntext\n## Sub\n### Deep\n#nospace\n", "x.md");
        CHECK(names(v) == "Title,Sub,Deep");
        CHECK(v[0].depth == 0 && v[2].depth == 2 && v[0].kind == 'H');
    }
    {
        auto v = scan("pub fn run() {}\nimpl Foo {\n    fn bar(&self) {}\n}\n", "x.rs");
        CHECK(names(v) == "run,Foo,bar");
    }
    {
        auto v = scan("class Foo;\nstruct Bar;\nint x = f(1);\nreturn g(2);\nint Foo::method(int a) const {\n}\n");
        CHECK(names(v) == "Foo::method");
    }
    {
        auto v = scan("func (s *Server) Start() error {\n}\nfunction hello() {\n}\n", "x.go");
        CHECK(names(v) == "Start,hello");
    }
    if (fails == 0) printf("outline ok\n");
    return fails ? 1 : 0;
}
