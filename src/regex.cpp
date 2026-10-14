/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "regex.h"

#include <string.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include <algorithm>
#include <bitset>
#include <unordered_map>

namespace {

enum Op : uint8_t { OP_BYTE, OP_SPLIT, OP_JMP, OP_ASSERT, OP_MATCH };
enum Assert : uint8_t { AS_BOL, AS_EOL, AS_WORD, AS_NWORD };

struct Inst { Op op; Assert as; int x, y; };   /* BYTE: x = class index; SPLIT: try x first, then y; JMP: x; ASSERT: as */
using ByteSet = std::bitset<256>;

bool is_word(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* ---- syntax tree ---- */
enum NK { N_EMPTY, N_BYTES, N_CAT, N_ALT, N_STAR, N_PLUS, N_QUEST, N_REPEAT, N_ASSERT };
struct Node {
    NK k = N_EMPTY;
    int cls = 0, lo = 0, hi = 0;   /* N_BYTES: class; N_REPEAT: counts, hi < 0 = unbounded */
    bool greedy = true;
    Assert as = AS_BOL;
    std::vector<int> kids;
};

}  // namespace

struct Regex::Prog {
    std::vector<Inst> insts;
    std::vector<ByteSet> classes;
    bool dfa_ok = true;       /* no \b or \B */
    bool anchored_bol = false;
};

namespace {

struct Parser {
    const std::string &p;
    size_t i = 0;
    bool icase;
    std::string err;
    std::vector<Node> nodes;
    std::vector<ByteSet> classes;
    bool bad = false;

    Parser(const std::string &pat, bool ic) : p(pat), icase(ic) {}

    int mk(NK k) { nodes.emplace_back(); nodes.back().k = k; return (int)nodes.size() - 1; }
    int cls_of(const ByteSet &s) { classes.push_back(s); return (int)classes.size() - 1; }
    int bytes(const ByteSet &s) { int n = mk(N_BYTES); nodes[(size_t)n].cls = cls_of(s); return n; }
    int single(unsigned char c) {
        ByteSet s;
        s.set(c);
        if (icase) { if (c >= 'a' && c <= 'z') s.set((size_t)(c - 32)); else if (c >= 'A' && c <= 'Z') s.set((size_t)(c + 32)); }
        return bytes(s);
    }
    int cat2(int a, int b) { int n = mk(N_CAT); nodes[(size_t)n].kids = {a, b}; return n; }
    int alt2(int a, int b) { int n = mk(N_ALT); nodes[(size_t)n].kids = {a, b}; return n; }
    void fail(const char *m) { if (!bad) { bad = true; err = m; } }

    /* one UTF-8 character that is not in `excl` (ASCII bytes only); used for '.' and negated classes */
    int any_except(const ByteSet &excl) {
        ByteSet ascii;
        for (int c = 0; c < 128; c++) if (!excl.test((size_t)c)) ascii.set((size_t)c);
        ByteSet cont, l2, l3, l4;
        for (int c = 0x80; c <= 0xbf; c++) cont.set((size_t)c);
        for (int c = 0xc2; c <= 0xdf; c++) l2.set((size_t)c);
        for (int c = 0xe0; c <= 0xef; c++) l3.set((size_t)c);
        for (int c = 0xf0; c <= 0xf4; c++) l4.set((size_t)c);
        int two = cat2(bytes(l2), bytes(cont));
        int three = cat2(cat2(bytes(l3), bytes(cont)), bytes(cont));
        int four = cat2(cat2(cat2(bytes(l4), bytes(cont)), bytes(cont)), bytes(cont));
        return alt2(alt2(bytes(ascii), two), alt2(three, four));
    }

    static void add_range(ByteSet &s, int a, int b) { for (int c = a; c <= b; c++) s.set((size_t)c); }
    static void class_escape(char e, ByteSet &s) {   /* \d \w \s: positive form only */
        if (e == 'd') add_range(s, '0', '9');
        else if (e == 'w') { add_range(s, 'a', 'z'); add_range(s, 'A', 'Z'); add_range(s, '0', '9'); s.set('_'); }
        else if (e == 's') { s.set(' '); s.set('\t'); s.set('\n'); s.set('\r'); s.set('\f'); s.set('\v'); }
    }
    int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

    bool peek(char c) { return i < p.size() && p[i] == c; }

    int parse_alt() {
        int left = parse_cat();
        while (!bad && peek('|')) { i++; int right = parse_cat(); left = alt2(left, right); }
        return left;
    }

    int parse_cat() {
        std::vector<int> items;
        while (!bad && i < p.size() && p[i] != '|' && p[i] != ')') items.push_back(parse_repeat());
        if (items.empty()) return mk(N_EMPTY);
        if (items.size() == 1) return items[0];
        int n = mk(N_CAT);
        nodes[(size_t)n].kids = items;
        return n;
    }

    int parse_repeat() {
        int a = parse_atom();
        for (;;) {
            if (bad || i >= p.size()) return a;
            char c = p[i];
            int n;
            if (c == '*') { i++; n = mk(N_STAR); }
            else if (c == '+') { i++; n = mk(N_PLUS); }
            else if (c == '?') { i++; n = mk(N_QUEST); }
            else if (c == '{') {
                size_t j = i + 1;
                int lo = 0, hi = -1;
                bool ok = j < p.size() && isdigit((unsigned char)p[j]);
                while (j < p.size() && isdigit((unsigned char)p[j])) lo = lo * 10 + (p[j++] - '0');
                if (ok && j < p.size() && p[j] == ',') {
                    j++;
                    if (j < p.size() && isdigit((unsigned char)p[j])) { hi = 0; while (j < p.size() && isdigit((unsigned char)p[j])) hi = hi * 10 + (p[j++] - '0'); }
                } else hi = lo;
                if (!ok || j >= p.size() || p[j] != '}') return a;       /* not a quantifier: the '{' is a literal, handled by the caller */
                if (lo > 1000 || hi > 1000) { fail("repeat count too large"); return a; }
                if (hi >= 0 && hi < lo) { fail("bad repeat range"); return a; }
                i = j + 1;
                n = mk(N_REPEAT);
                nodes[(size_t)n].lo = lo;
                nodes[(size_t)n].hi = hi;
            } else return a;
            nodes[(size_t)n].kids = {a};
            if (peek('?')) { i++; nodes[(size_t)n].greedy = false; }
            a = n;
        }
    }

    int parse_class() {   /* after '[' */
        bool neg = false;
        if (peek('^')) { neg = true; i++; }
        ByteSet s;
        bool first = true;
        while (i < p.size() && (p[i] != ']' || first)) {
            first = false;
            unsigned char c = (unsigned char)p[i++];
            int lo = c;
            if (c >= 0x80) { fail("non-ASCII characters inside [...] are not supported"); return mk(N_EMPTY); }
            if (c == '\\' && i < p.size()) {
                char e = p[i++];
                if (e == 'd' || e == 'w' || e == 's') { class_escape(e, s); continue; }
                if (e == 'D' || e == 'W' || e == 'S') { ByteSet t; class_escape((char)(e + 32), t); for (int b = 0; b < 128; b++) if (!t.test((size_t)b)) s.set((size_t)b); continue; }
                if (e == 'n') lo = '\n'; else if (e == 't') lo = '\t'; else if (e == 'r') lo = '\r'; else if (e == 'f') lo = '\f'; else if (e == 'v') lo = '\v';
                else if (e == 'x' && i + 1 < p.size() && hexval(p[i]) >= 0 && hexval(p[i + 1]) >= 0) { lo = hexval(p[i]) * 16 + hexval(p[i + 1]); i += 2; }
                else lo = (unsigned char)e;
            }
            int hi = lo;
            if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']') {
                i++;
                unsigned char d = (unsigned char)p[i++];
                hi = d;
                if (d == '\\' && i < p.size()) { char e = p[i++]; hi = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : (unsigned char)e; }
                if (hi < lo) { fail("bad range in [...]"); return mk(N_EMPTY); }
            }
            add_range(s, lo, hi);
        }
        if (!peek(']')) { fail("missing ]"); return mk(N_EMPTY); }
        i++;
        if (icase) for (int c = 'a'; c <= 'z'; c++) { if (s.test((size_t)c) || s.test((size_t)(c - 32))) { s.set((size_t)c); s.set((size_t)(c - 32)); } }
        if (!neg) { ByteSet a; for (int c = 0; c < 128; c++) if (s.test((size_t)c)) a.set((size_t)c); return bytes(a); }
        return any_except(s);
    }

    int parse_atom() {
        unsigned char c = (unsigned char)p[i++];
        switch (c) {
        case '(': {
            if (peek('?')) {
                if (i + 1 < p.size() && p[i + 1] == ':') i += 2;
                else if (i + 2 < p.size() && p[i + 1] == 'i' && p[i + 2] == ')') { i += 3; icase = true; return mk(N_EMPTY); }
                else { fail("unsupported group syntax"); return mk(N_EMPTY); }
            }
            int n = parse_alt();
            if (!peek(')')) { fail("missing )"); return n; }
            i++;
            return n;
        }
        case ')': fail("unmatched )"); return mk(N_EMPTY);
        case '[': return parse_class();
        case '.': { ByteSet nl; nl.set('\n'); return any_except(nl); }
        case '^': { int n = mk(N_ASSERT); nodes[(size_t)n].as = AS_BOL; return n; }
        case '$': { int n = mk(N_ASSERT); nodes[(size_t)n].as = AS_EOL; return n; }
        case '*': case '+': case '?': fail("nothing to repeat"); return mk(N_EMPTY);
        case '\\': {
            if (i >= p.size()) { fail("trailing backslash"); return mk(N_EMPTY); }
            char e = p[i++];
            if (e == 'b' || e == 'B') { int n = mk(N_ASSERT); nodes[(size_t)n].as = e == 'b' ? AS_WORD : AS_NWORD; return n; }
            if (e == 'd' || e == 'w' || e == 's') { ByteSet s; class_escape(e, s); return bytes(s); }
            if (e == 'D' || e == 'W' || e == 'S') { ByteSet t; class_escape((char)(e + 32), t); return any_except(t); }
            if (e == 'n') return single('\n');
            if (e == 't') return single('\t');
            if (e == 'r') return single('\r');
            if (e == 'f') return single('\f');
            if (e == 'v') return single('\v');
            if (e == 'x' && i + 1 < p.size() && hexval(p[i]) >= 0 && hexval(p[i + 1]) >= 0) { int v = hexval(p[i]) * 16 + hexval(p[i + 1]); i += 2; return single((unsigned char)v); }
            return single((unsigned char)e);
        }
        default: break;
        }
        if (c >= 0xc0) {   /* a whole UTF-8 character is one atom so a quantifier applies to all of it */
            size_t l = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
            int n = single(c);
            for (size_t k = 1; k < l && i < p.size(); k++) n = cat2(n, single((unsigned char)p[i++]));
            return n;
        }
        return single(c);
    }
};

/* ---- compile tree to program ---- */
struct Compiler {
    const std::vector<Node> &nodes;
    Regex::Prog &prog;
    bool too_big = false;
    static const size_t LIMIT = 40000;

    Compiler(const std::vector<Node> &n, Regex::Prog &p) : nodes(n), prog(p) {}
    int emit(Inst i) { if (prog.insts.size() >= LIMIT) { too_big = true; return 0; } prog.insts.push_back(i); return (int)prog.insts.size() - 1; }
    int pc() const { return (int)prog.insts.size(); }

    void gen(int id) {
        if (too_big) return;
        const Node &n = nodes[(size_t)id];
        switch (n.k) {
        case N_EMPTY: break;
        case N_BYTES: emit({OP_BYTE, AS_BOL, n.cls, 0}); break;
        case N_CAT: for (int k : n.kids) gen(k); break;
        case N_ALT: {
            int sp = emit({OP_SPLIT, AS_BOL, 0, 0});
            prog.insts[(size_t)sp].x = pc();
            gen(n.kids[0]);
            int j = emit({OP_JMP, AS_BOL, 0, 0});
            prog.insts[(size_t)sp].y = pc();
            gen(n.kids[1]);
            prog.insts[(size_t)j].x = pc();
            break;
        }
        case N_STAR: star(n.kids[0], n.greedy); break;
        case N_PLUS: {
            int start = pc();
            gen(n.kids[0]);
            int sp = emit({OP_SPLIT, AS_BOL, 0, 0});
            if (n.greedy) { prog.insts[(size_t)sp].x = start; prog.insts[(size_t)sp].y = pc(); }
            else { prog.insts[(size_t)sp].x = pc(); prog.insts[(size_t)sp].y = start; }
            break;
        }
        case N_QUEST: {
            int sp = emit({OP_SPLIT, AS_BOL, 0, 0});
            int body = pc();
            gen(n.kids[0]);
            if (n.greedy) { prog.insts[(size_t)sp].x = body; prog.insts[(size_t)sp].y = pc(); }
            else { prog.insts[(size_t)sp].x = pc(); prog.insts[(size_t)sp].y = body; }
            break;
        }
        case N_REPEAT: {
            for (int k = 0; k < n.lo; k++) gen(n.kids[0]);
            if (n.hi < 0) { star(n.kids[0], n.greedy); break; }
            std::vector<int> splits;
            for (int k = n.lo; k < n.hi; k++) {
                int sp = emit({OP_SPLIT, AS_BOL, 0, 0});
                splits.push_back(sp);
                int body = pc();
                if (n.greedy) prog.insts[(size_t)sp].x = body; else prog.insts[(size_t)sp].y = body;
                gen(n.kids[0]);
            }
            for (int sp : splits) { if (n.greedy) prog.insts[(size_t)sp].y = pc(); else prog.insts[(size_t)sp].x = pc(); }
            break;
        }
        case N_ASSERT:
            emit({OP_ASSERT, n.as, 0, 0});
            if (n.as == AS_WORD || n.as == AS_NWORD) prog.dfa_ok = false;
            break;
        }
    }
    void star(int body, bool greedy) {
        int sp = emit({OP_SPLIT, AS_BOL, 0, 0});
        int b = pc();
        gen(body);
        emit({OP_JMP, AS_BOL, sp, 0});
        if (greedy) { prog.insts[(size_t)sp].x = b; prog.insts[(size_t)sp].y = pc(); }
        else { prog.insts[(size_t)sp].x = pc(); prog.insts[(size_t)sp].y = b; }
    }
};

/* longest run of fixed bytes in the top-level concatenation: every match must contain it. A class that is exactly {c, C} for a letter
 * counts too (case-insensitive patterns); the run is then returned lower-case with *icase set. */
void required_literal(const std::vector<Node> &nodes, const std::vector<ByteSet> &classes, int root, std::string *out, bool *whole, bool *icase) {
    std::vector<int> items;
    const Node &r = nodes[(size_t)root];
    if (r.k == N_CAT) items = r.kids; else items = {root};
    std::string best, cur;
    bool best_ic = false, cur_ic = false, all_lit = true, any_ic = false;
    for (int id : items) {
        const Node &n = nodes[(size_t)id];
        int c = -1;
        bool fold = false;
        if (n.k == N_BYTES) {
            const ByteSet &bs = classes[(size_t)n.cls];
            if (bs.count() == 1) { for (int k = 0; k < 256; k++) if (bs.test((size_t)k)) c = k; }
            else if (bs.count() == 2) {
                for (int k = 'a'; k <= 'z'; k++) if (bs.test((size_t)k) && bs.test((size_t)(k - 32))) { c = k; fold = true; }
            }
        }
        if (c >= 0) {
            if (fold) { cur_ic = true; any_ic = true; }
            cur += (char)c;
        } else {
            all_lit = false;
            if (cur.size() > best.size()) { best = cur; best_ic = cur_ic; }
            cur.clear();
            cur_ic = false;
        }
    }
    if (cur.size() > best.size()) { best = cur; best_ic = cur_ic; }
    *out = best;
    *icase = best_ic;
    *whole = all_lit && !best.empty();
    (void)any_ic;
}

}  // namespace

bool Regex::compile(const std::string &pattern, bool icase, std::string *err) {
    Parser ps(pattern, icase);
    int root = ps.parse_alt();
    if (!ps.bad && ps.i < pattern.size()) ps.fail("unmatched )");
    if (ps.bad) { if (err) *err = ps.err; return false; }
    auto pr = std::make_shared<Prog>();
    pr->classes = ps.classes;
    Compiler c(ps.nodes, *pr);
    c.gen(root);
    c.emit({OP_MATCH, AS_BOL, 0, 0});
    if (c.too_big) { if (err) *err = "pattern too large"; return false; }
    /* the program starts with ^ when every match must begin at the line start */
    pr->anchored_bol = !pr->insts.empty() && pr->insts[0].op == OP_ASSERT && pr->insts[0].as == AS_BOL;
    bool whole = false, ic = false;
    required_literal(ps.nodes, ps.classes, root, &lit_, &whole, &ic);
    /* a folded run that contains upper-case letters in the middle of a case-sensitive pattern would be wrong to treat as folded: only
     * accept the fold when the whole compile was case-insensitive */
    if (ic && !(icase || ps.icase)) { lit_.clear(); whole = false; ic = false; }
    literal_ = whole;
    lit_icase_ = ic;
    prog = pr;
    if (!pr->dfa_ok) {   /* \b and \B need the previous and next byte, which the DFA does not track: build a looser twin that accepts a superset of the lines */
        auto rp = std::make_shared<Prog>(*pr);
        for (Inst &in : rp->insts) if (in.op == OP_ASSERT && (in.as == AS_WORD || in.as == AS_NWORD)) { in.op = OP_JMP; in.x = (int)(&in - rp->insts.data()) + 1; }
        rp->dfa_ok = true;
        relaxed = std::make_shared<Regex>();
        relaxed->prog = rp;
        relaxed->lit_ = lit_;
        relaxed->lit_icase_ = lit_icase_;
        relaxed->literal_ = false;
    }
    return true;
}

/* ---- case-insensitive substring search: SSE2 "packed pair" prefilter (two rare needle bytes compared 16 positions at a time) ---- */

namespace {

int byte_rarity(unsigned char c) {   /* higher = rarer in source code and prose; a rough static table, only used to pick which needle bytes to test */
    static const char *order = "etaoinsrhldcumfpgwybvkxjqz";   /* common to rare */
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    if (c >= 'a' && c <= 'z') { const char *p = strchr(order, c); return 10 + (int)(p - order) * 3; }
    if (c == ' ' || c == '\t') return 0;
    if (c == '_' || c == '(' || c == ')' || c == ';' || c == ',' || c == '.' || c == '\n') return 12;
    if (c >= '0' && c <= '9') return 60;
    return 90;
}

struct Needle {
    std::string lo;          /* lower-case needle */
    size_t i = 0, j = 0;     /* the two probe positions */
    unsigned char mi = 0, mj = 0, ti = 0, tj = 0;   /* (byte | mask) == target */
    void init(const std::string &n) {
        lo = n;
        size_t best_i = 0, best_j = 0;
        int ri = -1, rj = -1;
        for (size_t k = 0; k < lo.size(); k++) { int r = byte_rarity((unsigned char)lo[k]); if (r > ri) { rj = ri; best_j = best_i; ri = r; best_i = k; } else if (r > rj) { rj = r; best_j = k; } }
        if (lo.size() == 1) best_j = best_i;
        i = best_i; j = best_j;
        auto setm = [&](size_t k, unsigned char *m, unsigned char *t) { unsigned char c = (unsigned char)lo[k]; *m = (c >= 'a' && c <= 'z') ? 0x20 : 0; *t = c; };
        setm(i, &mi, &ti); setm(j, &mj, &tj);
    }
    bool verify(const char *h) const {
        for (size_t k = 0; k < lo.size(); k++) { unsigned char c = (unsigned char)h[k]; if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32); if (c != (unsigned char)lo[k]) return false; }
        return true;
    }
};

const char *icase_find(const Needle &nd, const char *h, size_t n) {
    const size_t m = nd.lo.size();
    if (m == 0 || n < m) return nullptr;
    size_t p = 0;
    const size_t last = n - m;                 /* last valid start */
#ifdef __SSE2__
    const size_t reach = std::max(nd.i, nd.j);
    const __m128i mi = _mm_set1_epi8((char)nd.mi), ti = _mm_set1_epi8((char)nd.ti), mj = _mm_set1_epi8((char)nd.mj), tj = _mm_set1_epi8((char)nd.tj);
    while (p + 16 <= last + 1 && p + reach + 16 <= n) {
        __m128i a = _mm_loadu_si128((const __m128i *)(h + p + nd.i)), b = _mm_loadu_si128((const __m128i *)(h + p + nd.j));
        __m128i ea = _mm_cmpeq_epi8(_mm_or_si128(a, mi), ti), eb = _mm_cmpeq_epi8(_mm_or_si128(b, mj), tj);
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_and_si128(ea, eb));
        while (mask) {
            unsigned bit = (unsigned)__builtin_ctz(mask);
            mask &= mask - 1;
            if (p + bit <= last && nd.verify(h + p + bit)) return h + p + bit;
        }
        p += 16;
    }
#endif
    for (; p <= last; p++) if (nd.verify(h + p)) return h + p;
    return nullptr;
}

}  // namespace

/* ---- matcher ---- */

struct Matcher::Impl {
    const Regex::Prog &pr;
    std::string lit;
    bool literal;

    /* Pike VM scratch */
    struct Th { int pc, start; };
    std::vector<Th> clist, nlist;
    std::vector<uint32_t> mark;
    uint32_t gen = 0;
    std::vector<int> stack;

    /* lazy DFA */
    struct St { std::vector<int> pcs; bool match, end_accept; };
    std::vector<St> states;
    std::vector<int32_t> trans;
    std::vector<uint8_t> sflag;   /* per state: 1 = match, 2 = dead; read in the scan loop instead of the St struct */
    std::unordered_map<std::string, int> index;
    int s_bol = -1, s_mid = -1;
    uint8_t skip_mid[256];     /* bytes that leave the idle state unchanged: the scan loop runs over them one compare per byte */
    bool skip_ready = false;
    static const size_t MAX_STATES = 3000;

    bool lit_ic = false;
    Needle nd;
    std::unique_ptr<Matcher> relaxed;      /* DFA pre-filter for patterns with \b */

    Impl(const Regex &re) : pr(*re.prog), lit(re.literal()), literal(re.is_literal()) {
        mark.assign(pr.insts.size(), 0);
        if (re.relaxed) relaxed.reset(new Matcher(*re.relaxed));
        lit_ic = re.lit_icase() && !lit.empty();
        if (lit_ic) nd.init(lit);
    }

    /* next occurrence of the literal (or required literal) at or after h, honouring case folding */
    const char *find_lit(const char *h, size_t n) const {
        if (lit_ic) return icase_find(nd, h, n);
        return static_cast<const char *>(memmem(h, n, lit.data(), lit.size()));
    }

    /* transition table codes: >= 0 next state; UNKNOWN not computed yet; NEWLINE end of the line; <= -4 the next state is flagged
     * (matching or dead), its id is -(code + 4). One load and one branch per byte in the scan loop covers all four. */
    static constexpr int32_t UNKNOWN = -1, NEWLINE = -2;
    static int32_t enc(int id, uint8_t flag) { return flag ? -(4 + id) : id; }
    static int dec(int32_t v) { return v <= -4 ? -(v + 4) : v; }
    static constexpr int OVERFLOW_ID = -2147483647 - 1;

    /* ---- Pike VM ---- */
    bool assert_ok(Assert a, const char *s, size_t n, size_t pos) const {
        switch (a) {
        case AS_BOL: return pos == 0;
        case AS_EOL: return pos == n;
        case AS_WORD: case AS_NWORD: {
            bool before = pos > 0 && is_word((unsigned char)s[pos - 1]), after = pos < n && is_word((unsigned char)s[pos]);
            return (before != after) == (a == AS_WORD);
        }
        }
        return false;
    }

    void add(std::vector<Th> &list, int pc0, int start, const char *s, size_t n, size_t pos) {
        stack.clear();
        stack.push_back(pc0);
        while (!stack.empty()) {
            int pc = stack.back();
            stack.pop_back();
            if (mark[(size_t)pc] == gen) continue;
            mark[(size_t)pc] = gen;
            const Inst &in = pr.insts[(size_t)pc];
            switch (in.op) {
            case OP_JMP: stack.push_back(in.x); break;
            case OP_SPLIT: stack.push_back(in.y); stack.push_back(in.x); break;       /* x is tried first */
            case OP_ASSERT: if (assert_ok(in.as, s, n, pos)) stack.push_back(pc + 1); break;
            default: list.push_back({pc, start}); break;
            }
        }
    }

    bool pike(const char *s, size_t n, size_t from, size_t *ms, size_t *me) {
        clist.clear();
        bool matched = false;
        int best_s = 0;
        size_t best_e = 0;
        for (size_t pos = from; pos <= n; pos++) {
            gen++;
            nlist.clear();
            /* the threads already in clist were added for this position by the previous step; mark their pcs so a new start does not duplicate them */
            for (const Th &t : clist) mark[(size_t)t.pc] = gen;
            if (!matched) add(clist, 0, (int)pos, s, n, pos);   /* a new attempt at every position, lowest priority */
            if (clist.empty() && matched) break;
            gen++;
            for (size_t k = 0; k < clist.size(); k++) {
                Th t = clist[k];
                const Inst &in = pr.insts[(size_t)t.pc];
                if (in.op == OP_MATCH) { matched = true; best_s = t.start; best_e = pos; break; }   /* lower-priority threads are cut */
                if (in.op == OP_BYTE && pos < n && pr.classes[(size_t)in.x].test((unsigned char)s[pos])) add(nlist, t.pc + 1, t.start, s, n, pos + 1);
            }
            clist.swap(nlist);
            if (clist.empty() && (matched || pr.anchored_bol)) { if (matched || pos >= from) break; }
        }
        if (matched) { *ms = (size_t)best_s; *me = best_e; }
        return matched;
    }

    /* ---- DFA ---- */
    void closure(std::vector<int> &out, int pc0, bool bol, bool eol, std::vector<char> &seen) {
        stack.clear();
        stack.push_back(pc0);
        while (!stack.empty()) {
            int pc = stack.back();
            stack.pop_back();
            if (seen[(size_t)pc]) continue;
            seen[(size_t)pc] = 1;
            const Inst &in = pr.insts[(size_t)pc];
            switch (in.op) {
            case OP_JMP: stack.push_back(in.x); break;
            case OP_SPLIT: stack.push_back(in.y); stack.push_back(in.x); break;
            case OP_ASSERT:
                if (in.as == AS_BOL) { if (bol) stack.push_back(pc + 1); }
                else if (in.as == AS_EOL) { if (eol) stack.push_back(pc + 1); else out.push_back(pc); }   /* pending until the line ends */
                break;
            default: out.push_back(pc); break;
            }
        }
    }

    int intern(std::vector<int> &pcs) {
        std::sort(pcs.begin(), pcs.end());
        pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
        std::string key((const char *)pcs.data(), pcs.size() * sizeof(int));
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        St st;
        st.pcs = pcs;
        st.match = false;
        for (int pc : pcs) if (pr.insts[(size_t)pc].op == OP_MATCH) st.match = true;
        std::vector<int> endset;
        std::vector<char> seen(pr.insts.size(), 0);
        st.end_accept = st.match;
        if (!st.match) {
            for (int pc : pcs) if (pr.insts[(size_t)pc].op == OP_ASSERT) closure(endset, pc, false, true, seen);   /* pending $ resolved at the end of the line */
            for (int pc : endset) if (pr.insts[(size_t)pc].op == OP_MATCH) st.end_accept = true;
        }
        sflag.push_back((uint8_t)((st.match ? 1 : 0) | (st.pcs.empty() ? 2 : 0)));
        states.push_back(std::move(st));
        trans.resize(states.size() * 256, UNKNOWN);
        int id = (int)states.size() - 1;
        trans[(size_t)id * 256 + '\n'] = NEWLINE;
        index.emplace(std::move(key), id);
        return id;
    }

    void dfa_reset() {
        states.clear(); trans.clear(); index.clear(); sflag.clear(); skip_ready = false;
        std::vector<char> seen(pr.insts.size(), 0);
        std::vector<int> a;
        closure(a, 0, true, false, seen);
        s_bol = intern(a);
        std::fill(seen.begin(), seen.end(), 0);
        std::vector<int> b;
        closure(b, 0, false, false, seen);
        s_mid = intern(b);
    }

    /* computes the transition for an UNKNOWN entry and stores it; returns the real next state id, or OVERFLOW_ID when the cache is full */
    int step(int sid, unsigned char c) {
        int32_t cur = trans[(size_t)sid * 256 + c];
        if (cur != UNKNOWN) return dec(cur);
        std::vector<int> next;
        std::vector<char> seen(pr.insts.size(), 0);
        for (int pc : states[(size_t)sid].pcs) {
            const Inst &in = pr.insts[(size_t)pc];
            if (in.op == OP_BYTE && pr.classes[(size_t)in.x].test(c)) closure(next, pc + 1, false, false, seen);
        }
        closure(next, 0, false, false, seen);   /* unanchored: a new attempt starts after every byte (empty when the pattern starts with ^) */
        if (states.size() >= MAX_STATES) return OVERFLOW_ID;
        int id = intern(next);
        trans[(size_t)sid * 256 + c] = enc(id, sflag[(size_t)id]);
        return id;
    }

    /* returns true if the line s[0,n) matches; used by test() */
    bool dfa_test(const char *s, size_t n) {
        if (s_bol < 0) dfa_reset();
        int st = s_bol;
        if (sflag[(size_t)st] & 1) return true;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            int32_t nx = trans[(size_t)st * 256 + c];
            if (nx == UNKNOWN || nx == NEWLINE) {
                int id = nx == NEWLINE ? st : step(st, c);   /* a stray '\n' inside a "line": treat as an ordinary byte value of no meaning */
                if (id == OVERFLOW_ID) return pike_test(s, n);
                nx = trans[(size_t)st * 256 + c];
                if (nx == NEWLINE) return false;
            }
            if (nx <= -4) { int id = dec(nx); return (sflag[(size_t)id] & 1) != 0; }   /* matching state: found; dead state: cannot match */
            st = nx;
        }
        return states[(size_t)st].end_accept;
    }

    bool pike_test(const char *s, size_t n) { size_t a, b; return pike(s, n, 0, &a, &b); }
};

Matcher::Matcher(const Regex &re) : p(new Impl(re)) {}
Matcher::~Matcher() {}
uint64_t Matcher::dfa_states() const { return p->states.size(); }

bool Matcher::find(const char *s, size_t n, size_t from, size_t *ms, size_t *me) {
    if (p->literal && !p->lit.empty()) {
        if (from > n) return false;
        const char *h = p->find_lit(s + from, n - from);
        if (!h) return false;
        *ms = (size_t)(h - s);
        *me = *ms + p->lit.size();
        return true;
    }
    return p->pike(s, n, from, ms, me);
}

bool Matcher::test(const char *s, size_t n) {
    if (p->literal && !p->lit.empty()) return p->find_lit(s, n) != nullptr;
    if (p->pr.dfa_ok) return p->dfa_test(s, n);
    if (p->relaxed && !p->relaxed->test(s, n)) return false;       /* cheap reject; only lines that pass go to the exact matcher */
    return p->pike_test(s, n);
}

void Matcher::scan(const char *buf, size_t n, const std::function<bool(size_t, size_t)> &cb) {
    Impl &m = *p;
    auto line_end = [&](size_t from) { const void *e = memchr(buf + from, '\n', n - from); return e ? (size_t)((const char *)e - buf) : n; };
    auto line_start = [&](size_t at) { while (at > 0 && buf[at - 1] != '\n') at--; return at; };   /* bounded by one line */

    /* whole-buffer DFA from a line start: one pass; newline, match and dead states are encoded in the table, so a byte costs one load and one branch */
    auto dfa_scan = [&](size_t start) {
        if (m.s_bol < 0) m.dfa_reset();
        if (m.sflag[(size_t)m.s_bol] & 1) {   /* the pattern matches the empty string: every line matches */
            size_t ls = start;
            while (ls < n) { size_t le = line_end(ls); if (!cb(ls, le)) return; ls = le + 1; }
            return;
        }
        if (!m.skip_ready) {
            for (int c = 0; c < 256; c++) { int t = c == '\n' ? -3 : m.step(m.s_mid, (unsigned char)c); m.skip_mid[c] = t == m.s_mid; }
            m.skip_ready = true;
        }
        const uint8_t *skip = m.skip_mid;
        const int32_t *tr = m.trans.data();
        const uint8_t *fl = m.sflag.data();
        int st = m.s_bol;
        size_t ls = start;
        for (size_t i = start; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (st == m.s_mid && skip[c]) {   /* idle state: run to the first byte that can start or continue a match */
                size_t j = i + 1;
                while (j < n && skip[(unsigned char)buf[j]]) j++;
                i = j - 1;
                continue;
            }
            int32_t nx = tr[(size_t)st * 256 + c];
            if (__builtin_expect(nx >= 0, 1)) { st = nx; continue; }
            if (nx == Impl::NEWLINE) {
                if (m.states[(size_t)st].end_accept && !cb(ls, i)) return;
                st = m.s_bol;
                ls = i + 1;
                continue;
            }
            if (nx == Impl::UNKNOWN) {
                int id = m.step(st, c);
                if (id == Impl::OVERFLOW_ID) {   /* DFA cache overflowed: this line goes through the Pike VM, then the cache restarts */
                    size_t le = line_end(i);
                    size_t a, b;
                    if (m.pike(buf + ls, le - ls, 0, &a, &b) && !cb(ls, le)) return;
                    m.dfa_reset();
                    m.skip_ready = false;
                    for (int k = 0; k < 256; k++) { int t = k == '\n' ? -3 : m.step(m.s_mid, (unsigned char)k); m.skip_mid[k] = t == m.s_mid; }
                    m.skip_ready = true;
                    tr = m.trans.data(); fl = m.sflag.data();
                    st = m.s_bol;
                    i = le;
                    ls = le + 1;
                    continue;
                }
                tr = m.trans.data(); fl = m.sflag.data();   /* the tables may have moved */
                nx = tr[(size_t)st * 256 + c];
                if (nx >= 0) { st = nx; continue; }
            }
            int id = Impl::dec(nx);                          /* flagged: matching (1) or dead (2) */
            size_t le = line_end(i);
            if ((fl[(size_t)id] & 1) && !cb(ls, le)) return;
            i = le;
            st = m.s_bol;
            ls = le + 1;
        }
        if (ls < n && m.states[(size_t)st].end_accept) cb(ls, n);   /* last line without a trailing newline */
    };
    /* assertions the DFA cannot do (\b): Pike VM line by line */
    auto pike_scan = [&](size_t start) {
        if (m.relaxed) {   /* the looser DFA picks the candidate lines, the Pike VM confirms them with the exact word-boundary rules */
            m.relaxed->scan(buf + start, n - start, [&](size_t ls, size_t le) {
                size_t a, b;
                if (m.pike(buf + start + ls, le - ls, 0, &a, &b)) return cb(start + ls, start + le);
                return true;
            });
            return;
        }
        size_t ls = start;
        while (ls < n) {
            size_t le = line_end(ls);
            size_t a, b;
            if (m.pike(buf + ls, le - ls, 0, &a, &b) && !cb(ls, le)) return;
            ls = le + 1;
        }
    };
    auto automaton_scan = [&](size_t start) { if (m.pr.dfa_ok) dfa_scan(start); else pike_scan(start); };

    /* 1) the pattern is a plain string: a substring search finds candidates, no automaton at all */
    if (m.literal && !m.lit.empty()) {
        size_t pos = 0;
        while (pos < n) {
            const char *h = m.find_lit(buf + pos, n - pos);
            if (!h) return;
            size_t at = (size_t)(h - buf);
            size_t ls = line_start(at), le = line_end(at);
            if (!cb(ls, le)) return;
            pos = le + 1;
        }
        return;
    }
    /* 2) a required literal narrows the search to candidate lines; the automaton only looks at those. If candidates turn out to be
     *    dense (the literal is too common to skip much), the rest of the buffer goes through the automaton directly. */
    if (m.lit.size() >= 2 && !m.literal) {
        size_t pos = 0, cands = 0;
        while (pos < n) {
            const char *h = m.find_lit(buf + pos, n - pos);
            if (!h) return;
            size_t at = (size_t)(h - buf);
            size_t ls = line_start(at), le = line_end(at);
            if (test(buf + ls, le - ls)) { if (!cb(ls, le)) return; }
            pos = le + 1;
            if (++cands == 512 && pos / cands < 300) { automaton_scan(pos); return; }   /* one candidate line per ~300 bytes: no real saving */
        }
        return;
    }
    automaton_scan(0);
}
