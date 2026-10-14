/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <malloc.h>
#include <math.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "appicon.h"
#include "doc.h"
#include "engine/atlas.h"
#include "engine/font.h"
#include "engine/gl.h"
#include "engine/render.h"
#include "fuzzy.h"
#include "git.h"
#include "icons.h"
#include "regex.h"
#include "search.h"
#include "lestrix/ui/fleetwm.h"
#include "lestrix/ui/tcore.h"
#include "lestrix/ui/termview.h"
#include "lestrix/ui/workpool.h"
#include "lestrix/ui/uitheme.h"
#include "syntax.h"
#include "tree.h"

static Uint32 g_ev_wake;
static std::atomic<int> g_wake_pending;
/* thread-safe: other threads (terminal readers, the fleetwm watcher, the search workers) call this to get the UI loop to run once */
void app_wake(void) {
    int exp = 0;
    if (g_ev_wake && g_wake_pending.compare_exchange_strong(exp, 1)) {
        SDL_Event e;
        SDL_zero(e);
        e.type = g_ev_wake;
        SDL_PushEvent(&e);
    }
}

namespace {

/* Colours come from the active theme: the same built-in themes as Lestrix (vendored uitheme.cpp) and the same "follow fleetwm" mode.
 * apply_theme() fills these in; the defaults below only matter for the first frame before a theme is applied. */
uint32_t C_CHROME, C_MENUBAR, C_EDITOR, C_BORDER, C_SELROW, C_HOVER, C_ACCENT, C_FIELD, C_GROUP, C_UI, C_UI_DIM, C_UI_FAINT, C_FG, C_GUTTER, C_GUTTER_CUR,
         C_CURSOR, C_MENU, C_MENU_BORDER, C_MENU_SEL, C_SEL, C_BOX, C_BOX_LINE, C_RED, C_GREEN, C_ORANGE, C_ON_ACCENT,
         C_CURLINE, C_GUIDE, C_GUIDE_ACT, C_OCC, C_BRACKET, C_SB_THUMB, C_SB_HOT;
const uint32_t C_SHADE = RGBA(0, 0, 0, 120);
uint32_t TOKCOL[T_COUNT];   /* indexed by Tok */
const double BLINK_FOR = 10.0;   /* the cursor stops blinking this long after the last input: an idle editor draws nothing */
const int TAB_W = 4;

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

long proc_rss_kb() {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

long proc_cpu_ticks() {
    FILE *f = fopen("/proc/self/stat", "r");
    long ut = 0, st = 0;
    if (f) {
        char b[1024];
        size_t n = fread(b, 1, sizeof b - 1, f);
        b[n] = 0;
        char *rp = strrchr(b, ')');
        if (rp && sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st) != 2) ut = st = 0;
        fclose(f);
    }
    return ut + st;
}

void fmt_count(double v, char *out, size_t cap) {
    if (v >= 1e9) snprintf(out, cap, "%.2fG", v / 1e9);
    else if (v >= 1e6) snprintf(out, cap, "%.2fM", v / 1e6);
    else if (v >= 1e4) snprintf(out, cap, "%.1fk", v / 1e3);
    else snprintf(out, cap, "%.0f", v);
}

std::string fmt_mb(double bytes) {
    char b[32];
    if (bytes >= 1048576) snprintf(b, sizeof b, "%.1f MB", bytes / 1048576); else snprintf(b, sizeof b, "%.0f KB", bytes / 1024);
    return b;
}

std::string fmt_ms(double ms);
std::string sfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string sfmt(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

size_t u8_len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1; }
uint32_t u8_decode(const char *s, size_t n, size_t *adv) {
    unsigned char c = (unsigned char)s[0];
    size_t l = u8_len(c);
    if (l > n) l = 1;
    *adv = l;
    if (l == 1) return c;
    uint32_t cp = c & (0xff >> (l + 1));
    for (size_t i = 1; i < l; i++) {
        if (((unsigned char)s[i] & 0xc0) != 0x80) { *adv = 1; return c; }
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3f);
    }
    return cp;
}

int col_of(const std::string &t, size_t byte) {
    int col = 0;
    for (size_t i = 0; i < t.size() && i < byte;) {
        size_t a;
        uint32_t cp = u8_decode(t.data() + i, t.size() - i, &a);
        col = cp == '\t' ? (col / TAB_W + 1) * TAB_W : col + 1;
        i += a;
    }
    return col;
}

size_t byte_of(const std::string &t, int col) {
    int c = 0;
    size_t i = 0;
    while (i < t.size()) {
        size_t a;
        uint32_t cp = u8_decode(t.data() + i, t.size() - i, &a);
        int nc = cp == '\t' ? (c / TAB_W + 1) * TAB_W : c + 1;
        if (nc > col) break;
        c = nc;
        i += a;
    }
    return i;
}

uint64_t fnv1a(const void *p, size_t n, uint64_t h) {
    const unsigned char *s = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h;
}

std::string lower_ascii(std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }

/* ---- actions: one table drives the menus, the shortcuts and the --script test hook ---- */

enum Act {
    A_NONE, A_NEW, A_OPEN_FILE, A_OPEN_FOLDER, A_SAVE, A_SAVEAS, A_CLOSE, A_QUIT, A_UNDO, A_REDO, A_CUT, A_COPY, A_PASTE, A_SELALL, A_FIND, A_QUICK,
    A_TOGGLE_SIDEBAR, A_PIN, A_VIEW_EXPLORER, A_VIEW_GIT, A_LANG, A_STATS, A_NEXT_TAB, A_PREV_TAB, A_ABOUT, A_REFRESH, A_THEME, A_SEARCH, A_TOG_REGEX, A_TOG_CASE, A_TOG_WORD, A_TERM_TOGGLE, A_TERM_NEW, A_TERM_KILL, A_TERM_PICK, A_TERM_EXTERNAL, A_COUNT
};
const char *ACT_NAMES[A_COUNT] = {"none", "new", "open_file", "open_folder", "save", "saveas", "close", "quit", "undo", "redo", "cut", "copy", "paste", "selall", "find", "quick",
                                  "toggle_sidebar", "pin", "view_explorer", "view_git", "lang", "stats", "next_tab", "prev_tab", "about", "refresh", "theme", "search", "tog_regex", "tog_case", "tog_word", "term_toggle", "term_new", "term_kill", "term_pick", "term_external"};

const char *ACT_LABEL[A_COUNT] = {"", "new tab", "open-file dialog", "open-folder dialog", "save", "save-as dialog", "close tab", "quit", "undo", "redo", "cut", "copy", "paste", "select all",
                                  "find box", "quick open", "toggle sidebar", "pin sidebar", "files tab", "git tab", "language picker", "stats panel", "next tab", "previous tab",
                                  "about", "refresh", "theme picker", "search tab", "toggle regex", "toggle case", "toggle word", "terminal panel", "new terminal", "kill terminal", "terminal picker", "external terminal"};

struct MenuItem { const char *label, *keys; int act; };   /* act A_NONE with a label = disabled, empty label = separator */
struct Menu { const char *name; std::vector<MenuItem> items; };
const std::vector<Menu> MENUS = {
    {"File", {{"New File", "Ctrl+N", A_NEW}, {"Open File...", "Ctrl+O", A_OPEN_FILE}, {"Open Folder...", "", A_OPEN_FOLDER}, {"Quick Open...", "Ctrl+P", A_QUICK}, {"", "", A_NONE},
              {"Save", "Ctrl+S", A_SAVE}, {"Save As...", "Ctrl+Shift+S", A_SAVEAS}, {"", "", A_NONE}, {"Close Tab", "Ctrl+W", A_CLOSE}, {"Exit", "Ctrl+Q", A_QUIT}}},
    {"Edit", {{"Undo", "Ctrl+Z", A_UNDO}, {"Redo", "Ctrl+Y", A_REDO}, {"", "", A_NONE}, {"Cut", "Ctrl+X", A_CUT}, {"Copy", "Ctrl+C", A_COPY}, {"Paste", "Ctrl+V", A_PASTE},
              {"Select All", "Ctrl+A", A_SELALL}, {"", "", A_NONE}, {"Find", "Ctrl+F", A_FIND}, {"Find in Files", "Ctrl+Shift+F", A_SEARCH}}},
    {"View", {{"Files", "Ctrl+Shift+E", A_VIEW_EXPLORER}, {"Search", "Ctrl+Shift+F", A_SEARCH}, {"Git", "Ctrl+Shift+G", A_VIEW_GIT}, {"", "", A_NONE}, {"Toggle Sidebar", "Ctrl+B", A_TOGGLE_SIDEBAR},
              {"Pin / Unpin Sidebar", "", A_PIN}, {"", "", A_NONE}, {"Next Tab", "Ctrl+Tab", A_NEXT_TAB}, {"Previous Tab", "Ctrl+Shift+Tab", A_PREV_TAB}, {"", "", A_NONE},
              {"Language Mode...", "Ctrl+Shift+L", A_LANG}, {"Color Theme...", "Ctrl+Shift+T", A_THEME}, {"", "", A_NONE}, {"Cache Statistics", "Ctrl+Shift+I", A_STATS}}},
    {"Terminal", {{"New Terminal", "Ctrl+Shift+`", A_TERM_NEW}, {"Toggle Terminal", "Ctrl+`", A_TERM_TOGGLE}, {"Kill Terminal", "", A_TERM_KILL}, {"", "", A_NONE},
                  {"Choose Terminal...", "", A_TERM_PICK}, {"Open External Terminal Here", "", A_TERM_EXTERNAL}}},
    {"Help", {{"About Hootedit", "", A_ABOUT}}},
};

struct Hit { float x, y, w, h; int kind, id; };
enum HitKind { H_TAB = 1, H_TABCLOSE, H_BOX, H_ACT, H_MENU, H_MENUITEM, H_PICKITEM, H_QUICKITEM, H_BTN_SAVE, H_BTN_DISCARD, H_BTN_CANCEL, H_TREE, H_ROOT, H_ACTIVITY, H_PIN, H_GITROW, H_SIDEBAR, H_SIDEBOX, H_SIDEHIT, H_SRES, H_TERMTAB, H_SPLIT, H_VSCROLL, H_FD_ITEM, H_FD_OK, H_FD_CANCEL, H_FD_PLACE, H_FD_HIDDEN, H_TABSPACE };
enum Mode { M_EDIT, M_FIND, M_PICK, M_CONFIRM, M_PROMPT, M_MENU, M_QUICK, M_SIDE, M_FILEDLG };
enum FdKind { FD_OPEN_FILE, FD_OPEN_FOLDER, FD_SAVE };
enum PickKind { PICK_LANG, PICK_THEME, PICK_TERMINAL };
enum Pending { P_NONE, P_CLOSE, P_QUIT };
enum PromptKind { PK_SAVEAS, PK_OPEN_FILE, PK_OPEN_FOLDER };
enum View { V_EXPLORER, V_SEARCH, V_GIT };

struct Ed {
    SDL_Window *win = nullptr;
    SDL_GLContext gl = nullptr;
    Atlas *atlas = nullptr;
    Renderer *r = nullptr;
    Font *font = nullptr, *big = nullptr, *ui = nullptr;
    char gl_desc[256] = "";
    float scale = 1.f;
    int W = 0, H = 0, cw = 8, ch = 16, asc = 12, rowh = 19, rowasc = 14;   /* editor font cell, line pitch and baseline inside a row */
    int uh = 16, uasc = 12;                                                 /* UI font */
    double now = 0;

    std::vector<std::unique_ptr<Doc>> docs;
    int active = 0, next_untitled = 1;
    Mode mode = M_EDIT;
    Pending pending = P_NONE;
    PromptKind prompt_kind = PK_SAVEAS;
    int confirm_idx = 0;
    std::string find_q, find_msg, prompt_q, quick_q;
    struct PItem { std::string label; int id; bool current; };
    PickKind pick_kind = PICK_LANG;
    std::string pick_filter;
    int pick_sel = 0, pick_scroll = 0;
    std::vector<PItem> pick_items;
    /* terminal panel (the Lestrix terminal engine) */
    struct Term { TermCore *core = nullptr; std::string title = "shell"; bool dead = false; };
    std::vector<std::unique_ptr<Term>> terms;
    int term_active = -1;
    bool term_open = false, term_focus = false, term_drag = false, split_drag = false, sb_drag = false;
    const Doc *br_doc = nullptr;                     /* bracket match cache: which document, cursor and revision it was computed for */
    size_t br_cur = ~(size_t)0;
    uint64_t br_rev = 0;
    long br_a = -1, br_b = -1;
    int panel_logical = 260;                         /* panel height in logical pixels; saved in settings */
    std::string term_choice = "builtin";             /* "builtin" or "external:<command>" */
    struct ExtTerm { std::string name, dirflag; };
    std::vector<ExtTerm> term_ext;
    float term_rx = 0, term_ry = 0, term_rw = 0, term_rh = 0;   /* body rectangle of the last frame, for mouse routing */
    /* file browser dialog (Open File, Open Folder, Save As) */
    struct FdEntry { std::string name; bool dir; };
    struct FileDlg {
        FdKind kind = FD_OPEN_FILE;
        std::string dir, filter, name, msg;
        std::vector<FdEntry> all;
        int sel = 0, scroll = 0;
        bool hidden = false, name_focus = false, overwrite_armed = false, picked = false;   /* picked: the user chose a row (arrow keys or click), not just the default */
    } fd;
    const UiTheme *theme = nullptr;
    bool follow_fleet = false;
    FleetCfg fleet;
    bool fleet_watch = false;
    double next_fleet = 0;
    TermPalette pal = {};
    int menu_open = -1, menu_sel = 0;
    std::vector<std::string> quick_files, quick_hits, side_hits;
    std::vector<std::vector<int>> quick_pos;          /* matched character positions of each quick-open hit, for highlighting */
    std::string side_q;
    /* workspace search */
    Searcher search;
    std::string sq;
    bool sq_regex = false, sq_case = false, sq_word = false;
    bool find_regex = false, find_case = false;       /* the same toggles for the in-file find */
    std::shared_ptr<const std::vector<std::string>> sfiles;
    std::vector<SearchHit> sres;
    SearchStats sstat;
    struct SRow { int file; int hit; int count; };    /* hit < 0: a file header */
    std::vector<SRow> srows;
    uint64_t s_seen_scanned = ~0ull, s_seen_hits = ~0ull;
    bool s_seen_done = false, s_active = false;
    int sel_hit = -1;
    std::vector<std::vector<int>> side_pos;           /* highlight positions for the files-tab fuzzy results */
    int quick_sel = 0;
    bool follow = true, dragging = false;
    std::vector<Hit> hits;
    int hover_kind = 0, hover_id = -1;
    float mx = -1, my = -1;
    std::string title_last;
    bool blank_tab = false;                          /* the placeholder document has been turned into a real empty tab (double click or Ctrl+N) */
    float hit_floor = 1e9f;                          /* hit boxes are cut off at this y: nothing behind the status bar can be clicked */
    std::string git_for;                             /* the folder the Git tab was last computed for */

    /* workspace and sidebar */
    Tree tree;
    GitInfo git;
    View view = V_EXPLORER;
    bool pinned = true, side_open = true, root_open = true;
    double side_close_at = 0;
    float side_scroll = 0;

    bool dirty = true, running = true, show_stats = false, blink_on = true, focused = true;
    std::string status_msg;
    double last_active = 0, last_blink = 0, next_hud = 0;

    /* row cache: finished instances for one line at local origin, keyed by text + language + lexer state + font + atlas generation + left column */
    struct Row { std::vector<RInst> v; };
    std::unordered_map<uint64_t, Row> rows;
    std::vector<uint8_t> tok;

    uint64_t frames = 0, prev_frames = 0;
    double prev_time = 0, frame_cost = 0;
    long prev_ticks = 0;
    CacheStats prev_cache = {};
    char hud[192] = "", hud_last[192] = "";
    /* operation timing: from the input event to the frame that shows its result being submitted to GL (vsync and scan-out are not included) */
    struct OpRec { std::string name; double ms; int n; double build_ms; double work_ms; };
    double last_build_ms = 0, op_work = 0;
    std::vector<OpRec> ops;
    bool op_pending = false;
    std::string op_name, op_detail;
    double op_t0 = 0, ev_t0 = 0;
    bool timing = false;                 /* --bench: split the frame cost by section */
    double t_top = 0, t_rows = 0, t_gutter = 0, t_status = 0, t_flush = 0;
};

Ed E;
volatile sig_atomic_t g_print_stats;
bool g_scripted = false, g_timed = false;
int g_clicks = 1;   /* click count of the latest mouse press, for double-click handling */
const Uint32 SCRIPT_MOUSE = 0x5c11b7u;
void on_usr1(int) { g_print_stats = 1; }

Doc &D() { return *E.docs[(size_t)E.active]; }
/* no file and nothing typed: the start screen shows, with no tab strip */
bool splash() { return E.docs.size() == 1 && D().pristine() && !E.blank_tab; }
Ed::Term *cur_term();
std::vector<int> fd_view();
void git_refresh();
void git_follow_active();
void launch_external(std::string name);
void detect_externals();
int S(float n) { return (int)lroundf(n * E.scale); }

std::string cache_stats_text(bool all_ops = false) {
    auto pct = [](uint64_t hit, uint64_t miss) { return hit + miss ? 100.0 * (double)hit / (double)(hit + miss) : 0.0; };
    std::string o = sfmt("Glyph atlas: %u glyphs cached, ASCII %d/512 filled, texture %dx%d, cleared %u time(s)\n", (unsigned)atlas_count(E.atlas),
                         atlas_ascii_filled(E.atlas), atlas_size(E.atlas), atlas_size(E.atlas), (unsigned)atlas_generation(E.atlas));
    o += sfmt("Glyph cache: %llu hits, %llu misses (%.1f%% hit), %llu dropped by atlas clears\n", (unsigned long long)sd_cache.glyph_hit,
              (unsigned long long)sd_cache.glyph_miss, pct(sd_cache.glyph_hit, sd_cache.glyph_miss), (unsigned long long)sd_cache.glyph_recycle);
    o += sfmt("Row cache: %llu hits, %llu misses (%.1f%% hit), %llu evicted when the cache filled\n", (unsigned long long)sd_cache.row_hit,
              (unsigned long long)sd_cache.row_miss, pct(sd_cache.row_hit, sd_cache.row_miss), (unsigned long long)sd_cache.row_recycle);
    o += sfmt("Frames: %llu drawn, average cost %.2f ms (%s)\n", (unsigned long long)E.frames, E.frame_cost * 1000.0, E.gl_desc);
    Doc &d = D();
    if (!E.ops.empty()) {
        o += "Recent operations (input event to frame submitted):\n";
        for (size_t i = E.ops.size(); i-- > (all_ops || E.ops.size() <= 10 ? 0 : E.ops.size() - 10);) o += "  " + fmt_ms(E.ops[i].ms) + (E.ops[i].work_ms >= 0 ? "  [work " + fmt_ms(E.ops[i].work_ms) + "]" : std::string()) + (E.ops[i].build_ms >= 0 ? "  [frame build " + fmt_ms(E.ops[i].build_ms) + "]" : std::string()) + "   " + E.ops[i].name + (E.ops[i].n > 1 ? sfmt("   (x%d, last shown)", E.ops[i].n) : std::string()) + "\n";
    }
    o += sfmt("Document: %s, %zu bytes, %zu lines, %zu pieces; language %s%s; %zu tab(s); memory %s", d.name().c_str(), d.buf.size(), d.buf.lines(), d.buf.pieces(),
              lang_name(d.lang), d.lang_forced ? " (picked)" : " (auto)", E.docs.size(), fmt_mb((double)proc_rss_kb() * 1024).c_str());
    return o;
}

void redraw() { E.dirty = true; }
void touch() { E.last_active = E.now; E.blink_on = true; E.last_blink = E.now; redraw(); }

std::string fmt_ms(double ms) {
    char b[32];
    if (ms < 10) snprintf(b, sizeof b, "%.2f ms", ms);
    else if (ms < 1000) snprintf(b, sizeof b, "%.0f ms", ms);
    else snprintf(b, sizeof b, "%.2f s", ms / 1000);
    return b;
}

struct WorkTimer {   /* adds the time spent in the real work of an operation, apart from drawing the result */
    double t0 = now_s();
    ~WorkTimer() { E.op_work += (now_s() - t0) * 1000.0; }
};

/* start timing an operation; it ends when the next frame has been built and submitted */
void begin_op(const std::string &name, const std::string &detail = "") {
    E.op_pending = true;
    E.op_work = 0;
    E.op_name = name;
    E.op_detail = detail;
    E.op_t0 = E.ev_t0 > 0 ? E.ev_t0 : now_s();
}

void record_op(const std::string &name, double ms, double build_ms = -1, double work_ms = -1) {
    if (!E.ops.empty() && E.ops.back().name == name) { E.ops.back().ms = ms; E.ops.back().n++; E.ops.back().build_ms = build_ms; E.ops.back().work_ms = work_ms; }   /* repeated keystrokes keep one line */
    else { E.ops.push_back({name, ms, 1, build_ms, work_ms}); if (E.ops.size() > 64) E.ops.erase(E.ops.begin()); }
}

void end_op_if_pending() {
    if (!E.op_pending) return;
    double ms = (now_s() - E.op_t0) * 1000.0;
    record_op(E.op_name + (E.op_detail.empty() ? "" : " (" + E.op_detail + ")"), ms, E.last_build_ms, E.op_work > 0 ? E.op_work : -1);
    E.op_pending = false;
    E.op_t0 = 0;
}

/* ---- settings (the part that must survive a restart: pin state and sidebar view) ---- */

std::string settings_path() {
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    std::string dir = x && *x ? x : std::string(h ? h : ".") + "/.config";
    return dir + "/hootedit/settings.ini";
}

void settings_load() {
    FILE *f = fopen(settings_path().c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "sidebar_pinned")) E.pinned = atoi(v) != 0;
        else if (!strcmp(line, "sidebar_view")) E.view = !strcmp(v, "git") ? V_GIT : !strcmp(v, "search") ? V_SEARCH : V_EXPLORER;
        else if (!strcmp(line, "theme")) E.theme = ui_theme_find(v);
        else if (!strcmp(line, "follow_fleetwm")) E.follow_fleet = atoi(v) != 0;
        else if (!strcmp(line, "terminal")) E.term_choice = v;
        else if (!strcmp(line, "panel_height")) E.panel_logical = std::max(80, std::min(1200, atoi(v)));
    }
    fclose(f);
}

void settings_save() {
    std::string p = settings_path();
    size_t s = p.rfind('/');
    std::string dir = p.substr(0, s);
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    FILE *f = fopen(p.c_str(), "w");
    if (!f) return;
    fprintf(f, "sidebar_pinned=%d\nsidebar_view=%s\ntheme=%s\nfollow_fleetwm=%d\nterminal=%s\npanel_height=%d\n", E.pinned ? 1 : 0, E.view == V_GIT ? "git" : E.view == V_SEARCH ? "search" : "explorer", E.theme ? E.theme->name : "Xylonic Dark", E.follow_fleet ? 1 : 0, E.term_choice.c_str(), E.panel_logical);
    fclose(f);
}

/* ---- documents and tabs ---- */

Doc &new_doc(bool untitled = true) {
    E.docs.push_back(std::make_unique<Doc>());
    if (untitled) E.docs.back()->untitled = E.next_untitled++;
    return *E.docs.back();
}

void set_active(int i) {
    if (i < 0 || i >= (int)E.docs.size()) return;
    E.active = i;
    E.follow = true;
    E.find_msg.clear();
    E.status_msg.clear();
    std::string t = D().name() + " - Hootedit";
    if (t != E.title_last) { E.title_last = t; SDL_SetWindowTitle(E.win, t.c_str()); }
    if (!D().path.empty() && E.tree.root().size() && D().path.compare(0, E.tree.root().size(), E.tree.root()) == 0) E.tree.reveal(D().path);
    git_follow_active();
    redraw();
}

void remove_doc(int i) {
    E.docs.erase(E.docs.begin() + i);
    if (E.docs.empty()) { new_doc(); E.blank_tab = false; }   /* the last tab is gone: back to the start screen */
    set_active(std::min(E.active > i ? E.active - 1 : E.active, (int)E.docs.size() - 1));
}

/* when the workspace itself is not a repository, the Git tab follows the file in the active tab */
void git_follow_active() {
    if (E.git.repo && E.git_for == E.tree.root()) return;
    if (D().path.empty()) return;
    std::string dir = D().path.substr(0, D().path.rfind('/'));
    if (dir != E.git_for) git_refresh();
}

void git_refresh() {
    std::string ws = E.tree.root().empty() ? "." : E.tree.root();
    E.git = git_query(ws);
    E.git_for = ws;
    if (!E.git.repo && !E.docs.empty() && !D().path.empty()) {   /* the workspace is no repository: show the one the open file belongs to */
        std::string dir = D().path.substr(0, D().path.rfind('/'));
        GitInfo g2 = git_query(dir);
        if (g2.repo) { E.git = g2; E.git_for = dir; }
    }
    E.quick_files.clear();
    E.sfiles.reset();                           /* the file list is rebuilt on the next search or quick open */
    redraw();
}

void open_path(const std::string &path) {
    for (size_t i = 0; i < E.docs.size(); i++) if (E.docs[i]->path == path) { set_active((int)i); return; }
    bool replace_first = E.docs.size() == 1 && D().pristine();
    Doc &d = new_doc(false);
    double t_open = now_s();
    bool opened = d.open(path.c_str());
    t_open = (now_s() - t_open) * 1000.0;
    E.op_work = 0;
    if (opened) { size_t sl = path.rfind('/'); E.op_work = 0; begin_op("open " + (sl == std::string::npos ? path : path.substr(sl + 1)), "map + index " + fmt_ms(t_open) + ", " + fmt_mb((double)d.buf.size()) + ", " + std::to_string(d.buf.lines()) + " lines"); E.op_work = t_open; }
    if (!opened) { E.docs.pop_back(); E.status_msg = "cannot open " + path; redraw(); return; }
    if (replace_first) E.docs.erase(E.docs.begin());
    set_active((int)E.docs.size() - 1);
}

void set_workspace(const std::string &dir) {
    char real[4096];
    std::string d = realpath(dir.c_str(), real) ? real : dir;
    E.tree.set_root(d);
    E.side_scroll = 0;
    git_refresh();
}

void open_diff(const GitChange &c) {
    std::string txt = git_diff(E.git.root, c.path, false);
    if (txt.empty()) txt = git_diff(E.git.root, c.path, true);
    if (txt.empty()) { open_path(E.git.root + "/" + c.path); return; }   /* untracked: no diff, show the file */
    bool replace_first = E.docs.size() == 1 && D().pristine();
    Doc &d = new_doc(false);
    d.title = c.path.substr(c.path.rfind('/') == std::string::npos ? 0 : c.path.rfind('/') + 1) + " (diff)";
    d.load_text(txt);
    d.set_language(nullptr, false);
    for (int i = 0; i < lang_count(); i++) if (!strcmp(lang_name(lang_at(i)), "Diff")) d.set_language(lang_at(i), true);
    if (replace_first) E.docs.erase(E.docs.begin());
    set_active((int)E.docs.size() - 1);
}

void close_confirmed() {
    int i = E.confirm_idx;
    E.mode = M_EDIT;
    E.pending = P_NONE;
    remove_doc(i);
}

void request_quit();

void open_filedlg(FdKind k);

void begin_prompt(PromptKind k) {
    begin_op(k == PK_SAVEAS ? "save-as dialog" : k == PK_OPEN_FILE ? "open-file dialog" : "open-folder dialog");
    open_filedlg(k == PK_SAVEAS ? FD_SAVE : k == PK_OPEN_FILE ? FD_OPEN_FILE : FD_OPEN_FOLDER);
    return;
    Doc &d = D();
    E.prompt_kind = k;
    char cwd[1024];
    std::string base = E.tree.root().empty() ? (getcwd(cwd, sizeof cwd) ? cwd : ".") : E.tree.root();
    if (k == PK_SAVEAS) E.prompt_q = d.path.empty() ? base + "/" : d.path;
    else E.prompt_q = base + "/";
    E.mode = M_PROMPT;
    redraw();
}

void after_save() {
    E.mode = M_EDIT;
    if (E.pending == P_CLOSE) { close_confirmed(); }
    else if (E.pending == P_QUIT) { E.pending = P_NONE; remove_doc(E.confirm_idx); request_quit(); }
    if (E.git.repo) git_refresh();
    redraw();
}

void ask_confirm(int idx, Pending p) {
    set_active(idx);
    E.confirm_idx = idx;
    E.pending = p;
    E.mode = M_CONFIRM;
    redraw();
}

void close_tab(int i) {
    if (E.docs[(size_t)i]->modified()) ask_confirm(i, P_CLOSE);
    else { E.confirm_idx = i; remove_doc(i); }
}

void request_quit() {
    for (size_t i = 0; i < E.docs.size(); i++)
        if (E.docs[i]->modified()) { ask_confirm((int)i, P_QUIT); return; }
    E.running = false;
}

void save_active() {
    Doc &d = D();
    if (d.path.empty()) { begin_prompt(PK_SAVEAS); return; }
    bool ok = d.save(nullptr);
    E.status_msg = ok ? "saved " + d.path : "SAVE FAILED, buffer kept: " + d.path;
    if (ok && E.git.repo) git_refresh();
    redraw();
}

std::string expand_path(std::string p) {
    if (!p.empty() && p[0] == '~' && getenv("HOME")) p = std::string(getenv("HOME")) + p.substr(1);
    if (!p.empty() && p[0] != '/') { char cwd[1024]; if (getcwd(cwd, sizeof cwd)) p = std::string(cwd) + "/" + p; }
    return p;
}

void finish_prompt() {
    std::string p = expand_path(E.prompt_q);
    if (E.prompt_kind == PK_SAVEAS) {
        if (p.empty() || p.back() == '/') { E.status_msg = "type a file name"; redraw(); return; }
        Doc &d = *E.docs[(size_t)(E.pending == P_NONE ? E.active : E.confirm_idx)];
        if (!d.save(p.c_str())) { E.status_msg = "SAVE FAILED: " + p; redraw(); return; }
        E.status_msg = "saved " + p;
        std::string t = d.name() + " - Hootedit";
        E.title_last = t;
        SDL_SetWindowTitle(E.win, t.c_str());
        E.rows.clear();
        after_save();
        return;
    }
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { E.status_msg = "not found: " + p; redraw(); return; }
    E.mode = M_EDIT;
    E.status_msg.clear();
    if (E.prompt_kind == PK_OPEN_FOLDER) { if (S_ISDIR(st.st_mode)) set_workspace(p); else E.status_msg = "not a folder: " + p; }
    else { if (S_ISREG(st.st_mode)) open_path(p); else E.status_msg = "not a file: " + p; }
    redraw();
}

/* ---- search (in the file, from the command box) ---- */

bool find_from(size_t start) {
    WorkTimer wt;
    Doc &d = D();
    if (E.find_q.empty()) { E.find_msg.clear(); return false; }
    bool icase = !E.find_case;
    if (icase) for (unsigned char c : E.find_q) if (c >= 'A' && c <= 'Z') icase = false;   /* smart case */
    Regex re;
    std::string err;
    if (!re.compile(E.find_regex ? E.find_q : Searcher::escape_literal(E.find_q), icase, &err)) { E.find_msg = "invalid: " + err; return false; }
    Matcher m(re);
    std::string t;
    size_t l0 = d.buf.line_of(start), nl = d.buf.lines();
    for (size_t k = 0; k <= nl; k++) {
        size_t line = (l0 + k) % nl;
        d.buf.line_text(line, t);
        size_t base = d.buf.line_start(line), from = 0;
        if (k == 0) from = start > base ? start - base : 0;
        while (from <= t.size()) {
            size_t ms, me;
            if (!m.find(t.data(), t.size(), from, &ms, &me)) break;
            if (me == ms) { from = ms + 1; continue; }                       /* an empty match is not something to select */
            if (k == nl && base + ms >= start) break;                       /* wrapped all the way round to where we began */
            d.break_group();
            d.anchor = base + ms;
            d.move_to(base + me, true, false);
            E.follow = true;
            E.find_msg.clear();
            return true;
        }
    }
    E.find_msg = "No results";
    return false;
}

/* ---- workspace search (sidebar SEARCH tab) ---- */

void ensure_files() {
    if (!E.sfiles) E.sfiles = std::make_shared<const std::vector<std::string>>(list_workspace_files(E.tree.root(), E.git.repo));
}

void start_search() {
    E.s_active = !E.sq.empty();
    E.sel_hit = -1;
    E.side_scroll = 0;
    if (!E.s_active) { E.search.cancel(); E.sres.clear(); E.srows.clear(); E.sstat = SearchStats(); redraw(); return; }
    ensure_files();
    SearchQuery q;
    q.text = E.sq;
    q.regex = E.sq_regex;
    q.word = E.sq_word;
    q.casing = E.sq_case ? SearchQuery::SENSITIVE : SearchQuery::SMART;
    E.search.start(E.sfiles, q, app_wake);
    E.s_seen_scanned = ~0ull; E.s_seen_hits = ~0ull; E.s_seen_done = false;
    redraw();
}

void search_poll() {
    if (!E.s_active) return;
    SearchStats st = E.search.stats();
    if (st.files_scanned == E.s_seen_scanned && st.hits == E.s_seen_hits && st.done == E.s_seen_done) return;
    E.s_seen_scanned = st.files_scanned; E.s_seen_hits = st.hits; E.s_seen_done = st.done;
    E.search.results(E.sres);
    E.srows.clear();
    for (size_t i = 0; i < E.sres.size();) {
        size_t j = i;
        while (j < E.sres.size() && E.sres[j].file == E.sres[i].file) j++;
        E.srows.push_back({(int)E.sres[i].file, -1, (int)(j - i)});
        for (size_t k = i; k < j; k++) E.srows.push_back({(int)E.sres[k].file, (int)k, 0});
        i = j;
    }
    if (st.done && E.s_active && st.error.empty()) record_op("search \"" + E.sq.substr(0, 24) + "\" (" + std::to_string(st.hits) + " hits, " + std::to_string(st.files_total) + " files, " + fmt_mb((double)st.bytes) + ")", st.ms, -1, st.ms);
    E.sstat = st;
    redraw();
}

/* ---- fuzzy file filter shared by quick open and the files tab ---- */

/* the best `cap` files for query q (fzf-style scoring on the path relative to the workspace), with matched character positions in that relative path */
void fuzzy_filter(const std::vector<std::string> &files, const std::string &q, size_t cap, std::vector<std::string> &out, std::vector<std::vector<int>> &pos) {
    out.clear();
    pos.clear();
    size_t rl = E.tree.root().size() + 1;
    FuzzyQuery fq;
    fq.parse(q);
    struct Cand { int score; size_t idx; };
    std::vector<Cand> c;
    for (size_t i = 0; i < files.size(); i++) {
        const std::string &f = files[i];
        const char *rel = f.c_str() + std::min(rl, f.size());
        int sc = 0;
        if (fq.match(rel, f.size() - std::min(rl, f.size()), &sc, nullptr)) c.push_back({sc, i});
    }
    size_t keep = std::min(c.size(), cap);
    std::partial_sort(c.begin(), c.begin() + (long)keep, c.end(), [&](const Cand &a, const Cand &b) {
        return a.score != b.score ? a.score > b.score : files[a.idx].size() != files[b.idx].size() ? files[a.idx].size() < files[b.idx].size() : files[a.idx] < files[b.idx];
    });
    for (size_t i = 0; i < keep; i++) {
        const std::string &f = files[c[i].idx];
        const char *rel = f.c_str() + std::min(rl, f.size());
        std::vector<int> p;
        int sc;
        fq.match(rel, f.size() - std::min(rl, f.size()), &sc, &p);
        out.push_back(f);
        pos.push_back(std::move(p));
    }
}

/* ---- quick open (Ctrl+P) ---- */

void quick_update() {
    WorkTimer wt;
    if (E.quick_files.empty()) { ensure_files(); E.quick_files = *E.sfiles; }
    fuzzy_filter(E.quick_files, E.quick_q, 12, E.quick_hits, E.quick_pos);
    E.quick_sel = std::min(E.quick_sel, std::max(0, (int)E.quick_hits.size() - 1));
}

/* ---- drawing helpers ---- */

float text_width(Font *f, const char *s, int style = FS_REGULAR) {
    float w = 0;
    for (size_t i = 0; s[i];) {
        size_t a;
        uint32_t cp = u8_decode(s + i, strlen(s + i), &a);
        i += a;
        const AtlasGlyph *g = atlas_glyph(E.atlas, f, cp, style);   /* the atlas caches the advance; font_advance would run FT_Load_Glyph every call */
        w += g ? g->adv : (float)font_cell_w(f);
    }
    return w;
}

/* draws s clipped to maxw; returns the pen x after the last drawn glyph */
float draw_text(Font *f, float x, float y_baseline, const char *s, uint32_t rgba, float maxw, int style = FS_REGULAR) {
    float pen = x;
    for (size_t i = 0; s[i];) {
        size_t a;
        uint32_t cp = u8_decode(s + i, strlen(s + i), &a);
        i += a;
        const AtlasGlyph *g = atlas_glyph(E.atlas, f, cp, style);
        if (!g) continue;
        if (pen + g->adv > x + maxw) break;
        if (!g->blank) {
            RInst inst;
            r_make_glyph(E.r, &inst, g, pen, y_baseline, rgba);
            r_push(E.r, &inst, 1);
        }
        pen += g->adv;
    }
    return pen;
}

/* like draw_text, but characters whose byte offset (plus `shift`) is in the sorted list `pos` are drawn in `hl` */
float draw_text_hl(Font *f, float x, float y_baseline, const std::string &s, const std::vector<int> &pos, int shift, uint32_t rgba, uint32_t hl, float maxw) {
    float pen = x;
    for (size_t i = 0; i < s.size();) {
        size_t a;
        uint32_t cp = u8_decode(s.data() + i, s.size() - i, &a);
        bool on = std::binary_search(pos.begin(), pos.end(), (int)i + shift);
        i += a;
        const AtlasGlyph *g = atlas_glyph(E.atlas, f, cp, on ? FS_BOLD : FS_REGULAR);
        if (!g) continue;
        if (pen + g->adv > x + maxw) break;
        if (!g->blank) {
            RInst inst;
            r_make_glyph(E.r, &inst, g, pen, y_baseline, on ? hl : rgba);
            r_push(E.r, &inst, 1);
        }
        pen += g->adv;
    }
    return pen;
}

void draw_icon(Icon ic, float x, float y, int size, uint32_t rgba) {
    const AtlasGlyph *g = icon_glyph(E.atlas, ic, size);
    if (!g || g->blank) return;
    RInst inst;
    r_make_glyph(E.r, &inst, g, x, y, rgba);
    r_push(E.r, &inst, 1);
}

void hit(float x, float y, float w, float h, int kind, int id) {
    if (y >= E.hit_floor) return;
    if (y + h > E.hit_floor) h = E.hit_floor - y;   /* a row that peeks under the status bar must not be clickable there */
    E.hits.push_back({x, y, w, h, kind, id});
}
float ui_base(float y, float h) { return y + (h - (float)E.uh) / 2 + (float)E.uasc; }
bool hovered(int kind, int id) { return E.hover_kind == kind && E.hover_id == id; }

uint32_t lang_color(const Lang *l) {
    const char *n = lang_name(l);
    if (!strcmp(n, "C") || !strcmp(n, "C++")) return RGBA(86, 156, 214, 255);
    if (!strcmp(n, "Python")) return RGBA(86, 156, 214, 255);
    if (!strcmp(n, "JavaScript") || !strcmp(n, "TypeScript") || !strcmp(n, "Lua")) return RGBA(220, 200, 90, 255);
    if (!strcmp(n, "Shell") || !strcmp(n, "Makefile") || !strcmp(n, "CMake")) return RGBA(137, 209, 133, 255);
    if (!strcmp(n, "Markdown") || !strcmp(n, "Plain Text")) return RGBA(120, 170, 220, 255);
    if (!strcmp(n, "JSON") || !strcmp(n, "YAML") || !strcmp(n, "TOML") || !strcmp(n, "INI")) return RGBA(203, 170, 100, 255);
    if (!strcmp(n, "HTML") || !strcmp(n, "XML") || !strcmp(n, "CSS")) return RGBA(228, 120, 80, 255);
    if (!strcmp(n, "Rust") || !strcmp(n, "Go") || !strcmp(n, "Java") || !strcmp(n, "Ruby")) return RGBA(222, 140, 100, 255);
    return RGBA(150, 160, 175, 255);
}

uint32_t file_color(const std::string &name) { return lang_color(lang_detect(name.c_str(), nullptr, 0)); }

/* ---- layout ---- */

struct Layout {
    int menu_h, tab_h, status_h, side_w, side_x1;   /* side_w: width the docked sidebar takes from the editor; side_x1: right edge when drawn (docked or floating) */
    bool docked, floating;
    int ed_x, tab_y, text_y, bar_y, ed_bottom, panel_y, panel_h, gutter, text_x, rows, cols;
};

Layout layout() {
    Layout l;
    l.menu_h = S(28); l.tab_h = S(34); l.status_h = S(22);
    int sw = std::min(S(296), (int)((float)E.W * 0.32f));
    l.docked = E.pinned && E.side_open;
    l.floating = !E.pinned && E.side_open;
    l.side_w = l.docked ? sw : 0;
    l.side_x1 = E.side_open ? sw : 0;
    l.ed_x = l.side_w;
    l.tab_y = l.menu_h;
    l.text_y = l.menu_h + l.tab_h;
    l.bar_y = E.H - l.status_h;
    l.panel_h = E.term_open ? std::min(S(E.panel_logical), std::max(S(80), l.bar_y - l.text_y - S(100))) : 0;
    l.panel_y = l.bar_y - l.panel_h;
    l.ed_bottom = l.panel_y;
    int digits = 3;
    for (size_t n = D().buf.lines(); n >= 1000; n /= 10) digits++;
    l.gutter = S(79) + (digits - 3) * E.cw;
    l.text_x = l.ed_x + l.gutter;
    l.rows = std::max(0, (l.ed_bottom - l.text_y) / E.rowh);
    l.cols = std::max(0, (E.W - l.text_x) / E.cw);
    return l;
}

void ensure_visible(const Layout &L) {
    Doc &d = D();
    size_t l = d.buf.line_of(d.cur);
    if (l < d.top) d.top = l;
    else if (L.rows > 0 && l >= d.top + (size_t)L.rows) d.top = l - (size_t)L.rows + 1;
    int c = d.col_of_byte(l, d.cur - d.buf.line_start(l));
    if (c < d.left) d.left = c;
    else if (L.cols > 0 && c >= d.left + L.cols) d.left = c - L.cols + 1;
}

const std::vector<RInst> &row_instances(const std::string &text, int cols, const Lang *lang, uint8_t state) {
    Doc &d = D();
    uint64_t key = fnv1a(text.data(), text.size(), 1469598103934665603ull);
    int lid = lang_id(lang);
    uint32_t gen = atlas_generation(E.atlas), fid = font_id(E.font);
    key = fnv1a(&d.left, sizeof d.left, key);
    key = fnv1a(&cols, sizeof cols, key);
    key = fnv1a(&gen, sizeof gen, key);
    key = fnv1a(&fid, sizeof fid, key);
    key = fnv1a(&lid, sizeof lid, key);
    key = fnv1a(&state, sizeof state, key);
    auto it = E.rows.find(key);
    if (it != E.rows.end()) { sd_cache.row_hit++; return it->second.v; }
    sd_cache.row_miss++;
    if (E.rows.size() >= 4096) { sd_cache.row_recycle += E.rows.size(); E.rows.clear(); }
    Ed::Row &row = E.rows[key];
    if (E.tok.size() < text.size() + 1) E.tok.resize(text.size() + 64);
    if (lang != lang_plain()) lex_line(lang, text.data(), text.size(), state, E.tok.data());
    else memset(E.tok.data(), 0, text.size());
    int col = 0;
    for (size_t i = 0; i < text.size() && col < d.left + cols;) {
        size_t a, i0 = i;
        uint32_t cp = u8_decode(text.data() + i, text.size() - i, &a);
        i += a;
        if (cp == '\t') { col = (col / TAB_W + 1) * TAB_W; continue; }
        if (col >= d.left && cp != ' ') {
            const AtlasGlyph *g = atlas_glyph(E.atlas, E.font, cp, FS_REGULAR);
            if (g && !g->blank) {
                RInst inst;
                r_make_glyph(E.r, &inst, g, (float)((col - d.left) * E.cw), (float)E.rowasc, TOKCOL[E.tok[i0]]);
                row.v.push_back(inst);
            }
        }
        col++;
    }
    return row.v;
}

/* ---- pickers: one list UI for languages, themes and terminals (type to filter, Up/Down, Enter) ---- */

std::vector<int> pick_view() {   /* indices into E.pick_items that match the filter */
    std::vector<int> v;
    std::string f = lower_ascii(E.pick_filter);
    for (size_t i = 0; i < E.pick_items.size(); i++)
        if (f.empty() || lower_ascii(E.pick_items[i].label).find(f) != std::string::npos) v.push_back((int)i);
    return v;
}

const char *git_label(char x, char y, uint32_t *col) {
    char c = x != ' ' && x != '?' ? x : y;
    if (x == '?' && y == '?') { *col = C_GREEN; return "U"; }
    switch (c) {
    case 'M': *col = C_ORANGE; return "M";
    case 'A': *col = C_GREEN; return "A";
    case 'D': *col = C_RED; return "D";
    case 'R': *col = C_GREEN; return "R";
    default: *col = C_UI_DIM; return "?";
    }
}

/* ---- the sidebar, in the Lestrix style: a rail with the pin and rotated tabs, then a filter box and grouped lists ---- */

/* Text turned 90 degrees (reads bottom to top) as one cached atlas bitmap: the renderer only draws upright quads. */
const AtlasGlyph *vlabel(const char *text, Font *f, int style) {
    uint32_t fid = 0xfffd0000u | (font_id(f) & 0xffffu);
    uint32_t key = (uint32_t)fnv1a(text, strlen(text), 1469598103934665603ull + (uint64_t)style);
    if (const AtlasGlyph *g = atlas_find_custom(E.atlas, fid, key)) return g;
    int h = font_cell_h(f), asc = font_ascent(f), w = 2;
    for (size_t i = 0; text[i];) { size_t a; uint32_t cp = u8_decode(text + i, strlen(text + i), &a); i += a; const AtlasGlyph *g = atlas_glyph(E.atlas, f, cp, style); w += g ? (int)ceilf(g->adv) : font_cell_w(f); }
    std::vector<uint8_t> canvas((size_t)w * (size_t)h, 0), rot((size_t)w * (size_t)h, 0);
    int pen = 1;
    for (size_t i = 0; text[i];) {
        size_t a;
        uint32_t cp = u8_decode(text + i, strlen(text + i), &a);
        i += a;
        GlyphBmp b;
        if (font_glyph(f, cp, style, &b) && b.buf) {
            for (int y = 0; y < b.h; y++)
                for (int x = 0; x < b.w; x++) {
                    int cx = pen + b.left + x, cy = asc - b.top + y;
                    if (cx >= 0 && cx < w && cy >= 0 && cy < h) { uint8_t &d = canvas[(size_t)cy * (size_t)w + (size_t)cx]; d = std::max(d, b.buf[(size_t)y * (size_t)b.w + (size_t)x]); }
                }
        }
        pen += (int)ceilf(b.adv);
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) rot[(size_t)(w - 1 - x) * (size_t)h + (size_t)y] = canvas[(size_t)y * (size_t)w + (size_t)x];
    return atlas_custom(E.atlas, fid, key, rot.data(), h, w);
}

void side_filter_update() {
    WorkTimer wt;
    if (E.quick_files.empty()) { ensure_files(); E.quick_files = *E.sfiles; }
    fuzzy_filter(E.quick_files, E.side_q, 400, E.side_hits, E.side_pos);
}

const char *git_group(const GitChange &c) { return c.x == '?' ? "UNTRACKED" : c.x != ' ' ? "STAGED" : "CHANGES"; }

uint32_t git_dot(const GitChange &c) {
    char k = c.x != ' ' && c.x != '?' ? c.x : c.y;
    if (c.x == '?') return C_GREEN;
    return k == 'M' ? C_ORANGE : k == 'A' || k == 'R' ? C_GREEN : k == 'D' ? C_RED : C_UI_DIM;
}

void draw_dot(float cx, float cy, int size, uint32_t rgba) { draw_icon(IC_DOT, cx - (float)size / 2, cy - (float)size / 2, size, rgba); }

void draw_sidebar(const Layout &L) {
    float sw = (float)L.side_x1, top = (float)L.menu_h, bot = (float)L.bar_y;
    float rail = (float)S(29);
    r_rect(E.r, 0, top, sw, bot - top, C_CHROME);
    r_rect(E.r, rail, top, 1, bot - top, C_BORDER);
    r_rect(E.r, sw - 1, top, 1, bot - top, C_BORDER);
    hit(0, top, sw, bot - top, H_SIDEBAR, 0);

    /* rail: pin on top, then the rotated tabs */
    float pb = (float)S(20), pbx = (rail - pb) / 2, pby = top + (float)S(8);
    r_rect(E.r, pbx, pby, pb, pb, E.pinned ? C_ACCENT : C_BORDER);
    r_rect(E.r, pbx + 1, pby + 1, pb - 2, pb - 2, E.pinned ? C_ACCENT : hovered(H_PIN, 0) ? C_HOVER : C_CHROME);
    draw_icon(IC_PIN, pbx + (pb - (float)S(14)) / 2, pby + (pb - (float)S(14)) / 2, S(14), E.pinned ? C_EDITOR : C_UI_DIM);
    hit(0, top, rail, pby - top + pb + (float)S(4), H_PIN, 0);
    float ty = pby + pb + (float)S(14);
    const char *names[3] = {"FILES", "SEARCH", "GIT"};
    for (int i = 0; i < 3; i++) {
        const AtlasGlyph *g = vlabel(names[i], E.ui, FS_BOLD);
        if (!g) continue;
        float th = (float)g->h + (float)S(28);
        bool on = (int)E.view == i;
        if (on) { r_rect(E.r, 0, ty, rail, th, C_SELROW); r_rect(E.r, 0, ty, (float)S(2), th, C_ACCENT); }
        else if (hovered(H_ACTIVITY, i)) r_rect(E.r, 0, ty, rail, th, C_HOVER);
        RInst inst;
        r_make_glyph(E.r, &inst, g, (rail - (float)g->w) / 2 + 1, ty + (th - (float)g->h) / 2, on ? C_ACCENT : C_UI_DIM);
        r_push(E.r, &inst, 1);
        hit(0, ty, rail, th, H_ACTIVITY, i);
        ty += th + (float)S(2);
    }

    /* content: filter box with a "+" and the grouped list */
    float cx = rail + (float)S(10), cw = sw - cx - (float)S(10);
    float y = top + (float)S(8);
    bool files_view = E.view == V_EXPLORER, search_view = E.view == V_SEARCH;
    if (files_view || search_view) {
        float plus_w = files_view ? (float)S(32) : 0.f, bh = (float)S(28);
        float bw = cw - (files_view ? plus_w + (float)S(6) : 0.f);
        bool foc = E.mode == M_SIDE;
        r_rect(E.r, cx, y, bw, bh, foc ? C_ACCENT : C_FIELD);
        r_rect(E.r, cx + 1, y + 1, bw - 2, bh - 2, C_FIELD);
        float tw_ = search_view ? (float)S(26) * 3 + (float)S(6) : 0.f;   /* room for the three toggles inside the box */
        const std::string &qs = search_view ? E.sq : E.side_q;
        float pen = draw_text(E.ui, cx + (float)S(10), ui_base(y, bh), qs.c_str(), C_UI, bw - (float)S(20) - tw_);
        if (qs.empty()) draw_text(E.ui, cx + (float)S(10), ui_base(y, bh), search_view ? "Search in files" : "Search files", C_UI_DIM, bw - (float)S(20) - tw_);
        if (foc && E.blink_on) r_rect(E.r, pen, y + (float)S(6), std::max(1.f, E.scale), bh - (float)S(12), C_CURSOR);
        hit(cx, y, bw - tw_, bh, H_SIDEBOX, 0);
        if (search_view) {
            const char *lab[3] = {"Aa", ".*", "ab"};
            int ids[3] = {A_TOG_CASE, A_TOG_REGEX, A_TOG_WORD};
            bool on[3] = {E.sq_case, E.sq_regex, E.sq_word};
            float tx = cx + bw - tw_ + (float)S(3), th = bh - (float)S(8);
            for (int k = 0; k < 3; k++) {
                float w = (float)S(24);
                if (on[k]) r_rect(E.r, tx, y + (float)S(4), w, th, C_ACCENT);
                else if (hovered(H_ACT, ids[k])) r_rect(E.r, tx, y + (float)S(4), w, th, C_HOVER);
                float lw2 = text_width(E.ui, lab[k]);
                draw_text(E.ui, tx + (w - lw2) / 2, ui_base(y + (float)S(4), th), lab[k], on[k] ? C_ON_ACCENT : C_UI_DIM, w);
                hit(tx, y + (float)S(4), w, th, H_ACT, ids[k]);
                tx += w + (float)S(2);
            }
        }
        if (files_view) {
            float px = cx + bw + (float)S(6);
            r_rect(E.r, px, y, plus_w, bh, hovered(H_ACT, A_NEW) ? C_HOVER : C_CHROME);
            float pl = (float)S(12), pcx = px + plus_w / 2, pcy = y + bh / 2;
            r_rect(E.r, pcx - pl / 2, pcy - 0.75f * E.scale, pl, std::max(1.5f, 1.5f * E.scale), C_UI);
            r_rect(E.r, pcx - 0.75f * E.scale, pcy - pl / 2, std::max(1.5f, 1.5f * E.scale), pl, C_UI);
            hit(px, y, plus_w, bh, H_ACT, A_NEW);
        }
        y += bh + (float)S(8);
        if (search_view) {
            std::string st;
            uint32_t sc = C_UI_FAINT;
            if (!E.s_active) st = "Type to search every file in the workspace";
            else if (!E.sstat.error.empty()) { st = "pattern: " + E.sstat.error; sc = C_RED; }
            else st = sfmt("%llu results in %llu files   %.1f ms%s%s", (unsigned long long)E.sstat.hits, (unsigned long long)E.sstat.files_with_hits, E.sstat.ms,
                           E.sstat.truncated ? "   (capped)" : "", E.sstat.done ? "" : "   searching...");
            draw_text(E.ui, cx, ui_base(y, (float)S(20)), st.c_str(), sc, cw);
            if (E.s_active && E.sstat.done && E.sstat.error.empty()) {
                std::string st2 = sfmt("%llu files, %.0f MB scanned", (unsigned long long)E.sstat.files_scanned, (double)E.sstat.bytes / 1e6);
                draw_text(E.ui, cx, ui_base(y + (float)S(18), (float)S(20)), st2.c_str(), C_UI_FAINT, cw);
                y += (float)S(18);
            }
            y += (float)S(26);
        } else y += (float)S(4);
    } else {
        y += (float)S(4);
    }
    float list_top = y;
    r_clip(E.r, (int)(rail + 1), (int)list_top, (int)(sw - rail - 2), (int)(bot - list_top));
    float rowh = (float)S(28), ry = y - E.side_scroll;
    auto group = [&](const std::string &name, bool chev, bool open, int hit_kind) {
        draw_text(E.ui, cx + (chev ? (float)S(18) : 0.f), ui_base(ry, (float)S(24)), name.c_str(), C_GROUP, cw);
        if (chev) draw_icon(open ? IC_CHEV_D : IC_CHEV_R, cx - (float)S(4), ry + ((float)S(24) - (float)S(16)) / 2, S(16), C_GROUP);
        if (hit_kind) hit(rail + 1, ry, sw - rail - 2, (float)S(24), hit_kind, 0);
        ry += (float)S(26);
    };
    if (search_view) {
        const std::vector<std::string> &fl = E.search.files();
        size_t rl = E.tree.root().size() + 1;
        float rh = (float)S(24);
        for (size_t i = 0; i < E.srows.size(); i++, ry += rh) {
            if (ry + rh < list_top || ry > bot) continue;
            const Ed::SRow &row = E.srows[i];
            if (row.hit < 0) {
                const std::string &path = fl[(size_t)row.file];
                std::string rel = path.substr(std::min(rl, path.size()));
                size_t sl = rel.rfind('/');
                std::string base = sl == std::string::npos ? rel : rel.substr(sl + 1), dir = sl == std::string::npos ? "" : rel.substr(0, sl);
                r_rect(E.r, rail + 1, ry, sw - rail - 2, rh, C_SELROW);
                float pen = draw_text(E.ui, cx, ui_base(ry, rh), base.c_str(), C_UI, cw - (float)S(40), FS_BOLD);
                if (!dir.empty()) draw_text(E.ui, pen + (float)S(8), ui_base(ry, rh), dir.c_str(), C_UI_FAINT, sw - pen - (float)S(48));
                std::string cnt = std::to_string(row.count);
                draw_text(E.ui, sw - (float)S(14) - text_width(E.ui, cnt.c_str()), ui_base(ry, rh), cnt.c_str(), C_UI_DIM, (float)S(40));
                hit(rail + 1, ry, sw - rail - 2, rh, H_SRES, (int)i);
                continue;
            }
            const SearchHit &h = E.sres[(size_t)row.hit];
            if ((int)i == E.sel_hit) r_rect(E.r, rail + 1, ry, sw - rail - 2, rh, C_MENU_SEL);
            else if (hovered(H_SRES, (int)i)) r_rect(E.r, rail + 1, ry, sw - rail - 2, rh, C_HOVER);
            std::string num = std::to_string(h.line);
            draw_text(E.ui, cx + (float)S(30) - text_width(E.ui, num.c_str()), ui_base(ry, rh), num.c_str(), C_UI_FAINT, (float)S(40));
            size_t t0 = 0;
            while (t0 < h.ms && (h.text[t0] == ' ' || h.text[t0] == '\t')) t0++;     /* indentation is noise in a narrow list */
            std::string prefix;
            size_t sh0 = t0;
            if (h.ms > t0 + 18) { sh0 = h.ms - 14; prefix = "..."; }                  /* keep the match inside the visible width */
            std::string shown = prefix + h.text.substr(sh0);
            size_t ms2 = prefix.size() + (h.ms - sh0), me2 = prefix.size() + (std::min<size_t>(h.me, h.text.size()) - sh0);
            float x0 = cx + (float)S(40);
            float xa = x0 + text_width(E.ui, shown.substr(0, ms2).c_str()), xb = xa + text_width(E.ui, shown.substr(ms2, me2 > ms2 ? me2 - ms2 : 0).c_str());
            if (xb > xa) r_rect(E.r, xa, ry + (float)S(3), std::min(xb, sw - (float)S(6)) - xa, rh - (float)S(6), ui_mix(C_CHROME, C_ACCENT, 0.45f));
            draw_text(E.ui, x0, ui_base(ry, rh), shown.c_str(), C_UI, sw - x0 - (float)S(6));
            hit(rail + 1, ry, sw - rail - 2, rh, H_SRES, (int)i);
        }
        float content = (ry + E.side_scroll) - list_top;
        float maxs = std::max(0.f, content - (bot - list_top) + rh);
        if (E.side_scroll > maxs) E.side_scroll = maxs;
    } else if (files_view && !E.side_q.empty()) {
        group(sfmt("RESULTS  %zu", E.side_hits.size()), false, true, 0);
        size_t rl = E.tree.root().size() + 1;
        for (size_t i = 0; i < E.side_hits.size(); i++, ry += rowh) {
            if (ry + rowh < list_top || ry > bot) continue;
            if (hovered(H_SIDEHIT, (int)i)) r_rect(E.r, rail + 1, ry, sw - rail - 2, rowh, C_HOVER);
            std::string rel = E.side_hits[i].substr(std::min(rl, E.side_hits[i].size()));
            size_t sl = rel.rfind('/');
            std::string base = sl == std::string::npos ? rel : rel.substr(sl + 1), dir = sl == std::string::npos ? "" : rel.substr(0, sl);
            const std::vector<int> &ps = i < E.side_pos.size() ? E.side_pos[i] : std::vector<int>();
            float pen = draw_text_hl(E.ui, cx + (float)S(14), ui_base(ry, rowh), base, ps, (int)(sl == std::string::npos ? 0 : sl + 1), C_UI, C_ACCENT, cw - (float)S(14));
            if (!dir.empty()) draw_text_hl(E.ui, pen + (float)S(8), ui_base(ry, rowh), dir, ps, 0, C_UI_FAINT, C_ACCENT, sw - pen - (float)S(18));
            hit(rail + 1, ry, sw - rail - 2, rowh, H_SIDEHIT, (int)i);
        }
    } else if (files_view) {
        std::string name = E.tree.root_name();
        for (char &c : name) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        group(name, true, E.root_open, H_ROOT);
        if (E.root_open) {
            const std::vector<Tree::Row> &rows = E.tree.rows();
            for (size_t i = 0; i < rows.size(); i++, ry += rowh) {
                if (ry + rowh < list_top || ry > bot) continue;
                const Tree::Row &r = rows[i];
                int open_idx = -1;
                for (size_t k = 0; k < E.docs.size(); k++) if (!r.dir && E.docs[k]->path == r.path) open_idx = (int)k;
                bool cur = open_idx == E.active;
                if (cur) { r_rect(E.r, rail + 1, ry, sw - rail - 2, rowh, C_SELROW); r_rect(E.r, rail + 1, ry, (float)S(2), rowh, C_ACCENT); }
                else if (hovered(H_TREE, (int)i)) r_rect(E.r, rail + 1, ry, sw - rail - 2, rowh, C_HOVER);
                float x = cx + (float)S(14 * r.depth);
                float tx = x + (float)S(r.dir ? 18 : 14);
                if (r.dir) draw_icon(r.expanded ? IC_CHEV_D : IC_CHEV_R, x - (float)S(2), ry + (rowh - (float)S(16)) / 2, S(16), C_UI_DIM);
                else if (open_idx >= 0) draw_dot(x + (float)S(3), ry + rowh / 2, S(10), E.docs[(size_t)open_idx]->modified() ? C_ORANGE : C_GREEN);   /* open in a tab; orange = unsaved edits */
                draw_text(E.ui, tx + (float)S(open_idx >= 0 && !r.dir ? 4 : 0), ui_base(ry, rowh), r.name.c_str(), cur ? C_ACCENT : C_UI, sw - tx - (float)S(8));
                hit(rail + 1, ry, sw - rail - 2, rowh, H_TREE, (int)i);
            }
        }
        float content = (ry + E.side_scroll) - list_top;
        float maxs = std::max(0.f, content - (bot - list_top) + rowh);
        if (E.side_scroll > maxs) E.side_scroll = maxs;
    } else if (!E.git.repo) {
        draw_text(E.ui, cx, ui_base(ry, rowh), E.git.available ? "No git repository here" : "git is not installed", C_UI, cw, FS_BOLD);
        if (E.git.available) {
            draw_text(E.ui, cx, ui_base(ry + rowh, rowh), "Open a folder, or a file,", C_UI_DIM, cw);
            draw_text(E.ui, cx, ui_base(ry + rowh * 2 - (float)S(4), rowh), "that is inside a repository.", C_UI_DIM, cw);
        }
    } else {
        std::string br = E.git.branch + (E.git.ahead || E.git.behind ? sfmt("   %d ahead  %d behind", E.git.ahead, E.git.behind) : std::string());
        draw_icon(IC_GIT, cx, ry + (rowh - (float)S(14)) / 2, S(14), C_ACCENT);
        draw_text(E.ui, cx + (float)S(22), ui_base(ry, rowh), br.c_str(), C_UI, cw - (float)S(22), FS_BOLD);
        ry += rowh + (float)S(6);
        if (E.git.changes.empty()) draw_text(E.ui, cx, ui_base(ry, rowh), "Working tree clean", C_UI_DIM, cw);
        const char *order[3] = {"STAGED", "CHANGES", "UNTRACKED"};
        for (const char *grp : order) {
            size_t n = 0;
            for (const GitChange &c : E.git.changes) if (!strcmp(git_group(c), grp)) n++;
            if (!n) continue;
            group(sfmt("%s  %zu", grp, n), false, true, 0);
            for (size_t i = 0; i < E.git.changes.size(); i++) {
                const GitChange &c = E.git.changes[i];
                if (strcmp(git_group(c), grp)) continue;
                if (ry + rowh >= list_top && ry <= bot) {
                    if (hovered(H_GITROW, (int)i)) r_rect(E.r, rail + 1, ry, sw - rail - 2, rowh, C_HOVER);
                    size_t slash = c.path.rfind('/');
                    std::string base = slash == std::string::npos ? c.path : c.path.substr(slash + 1), dir = slash == std::string::npos ? "" : c.path.substr(0, slash);
                    draw_dot(cx + (float)S(5), ry + rowh / 2, S(10), git_dot(c));
                    float pen = draw_text(E.ui, cx + (float)S(18), ui_base(ry, rowh), base.c_str(), C_UI, cw - (float)S(18));
                    if (!dir.empty()) draw_text(E.ui, pen + (float)S(8), ui_base(ry, rowh), dir.c_str(), C_UI_FAINT, sw - pen - (float)S(18));
                    hit(rail + 1, ry, sw - rail - 2, rowh, H_GITROW, (int)i);
                }
                ry += rowh;
            }
            ry += (float)S(8);
        }
    }
    r_clip_off(E.r);
}

/* ---- the frame ---- */

void draw_menu_bar(const Layout &L, float *menu_end) {
    r_rect(E.r, 0, 0, (float)E.W, (float)L.menu_h, C_MENUBAR);
    float x = (float)S(8);
    for (size_t i = 0; i < MENUS.size(); i++) {
        float w = text_width(E.ui, MENUS[i].name) + (float)S(14);
        bool open = E.menu_open == (int)i;
        if (open || hovered(H_MENU, (int)i)) r_rect(E.r, x, (float)S(3), w, (float)(L.menu_h - S(6)), open ? C_MENU_SEL : C_HOVER);
        draw_text(E.ui, x + (float)S(7), ui_base(0, (float)L.menu_h), MENUS[i].name, C_UI, w);
        hit(x, 0, w, (float)L.menu_h, H_MENU, (int)i);
        x += w;
    }
    *menu_end = x;
}

void draw_panel(const Layout &L) {
    float px = (float)L.ed_x, py = (float)L.panel_y, pw = (float)(E.W - L.ed_x), ph = (float)L.panel_h, hh = (float)S(30);
    r_rect(E.r, px, py, pw, ph, C_EDITOR);
    r_rect(E.r, px, py, pw, 1, C_BORDER);
    hit(px, py - (float)S(3), pw, (float)S(7), H_SPLIT, 0);
    /* header: label, one chip per terminal, buttons on the right */
    float x = px + (float)S(14);
    draw_text(E.ui, x, ui_base(py, hh), "TERMINAL", E.term_focus ? C_UI : C_UI_DIM, pw, FS_BOLD);
    if (E.term_focus) r_rect(E.r, x, py + hh - (float)S(2), text_width(E.ui, "TERMINAL", FS_BOLD), (float)S(2), C_ACCENT);
    x += text_width(E.ui, "TERMINAL", FS_BOLD) + (float)S(20);
    for (size_t i = 0; i < E.terms.size(); i++) {
        std::string lab = std::to_string(i + 1) + ": " + E.terms[i]->title;
        float w = text_width(E.ui, lab.c_str()) + (float)S(20);
        bool act = (int)i == E.term_active;
        if (act) r_rect(E.r, x, py + (float)S(4), w, hh - (float)S(8), C_SELROW);
        else if (hovered(H_TERMTAB, (int)i)) r_rect(E.r, x, py + (float)S(4), w, hh - (float)S(8), C_HOVER);
        draw_text(E.ui, x + (float)S(10), ui_base(py, hh), lab.c_str(), act ? C_UI : C_UI_DIM, w);
        hit(x, py, w, hh, H_TERMTAB, (int)i);
        x += w + (float)S(4);
    }
    float bx2 = px + pw - (float)S(30);
    const struct { Icon ic; int act; } btn[3] = {{IC_CHEV_D, A_TERM_TOGGLE}, {IC_CLOSE, A_TERM_KILL}, {IC_DOT, A_TERM_NEW}};
    for (int k = 0; k < 3; k++) {
        float bx3 = bx2 - (float)(k * S(28));
        if (hovered(H_ACT, btn[k].act)) r_rect(E.r, bx3 - (float)S(2), py + (float)S(4), (float)S(24), hh - (float)S(8), C_HOVER);
        if (btn[k].act == A_TERM_NEW) {   /* a plus sign */
            float pl = (float)S(11), cx2 = bx3 + (float)S(10), cy2 = py + hh / 2;
            r_rect(E.r, cx2 - pl / 2, cy2 - 0.75f * E.scale, pl, std::max(1.5f, 1.5f * E.scale), C_UI_DIM);
            r_rect(E.r, cx2 - 0.75f * E.scale, cy2 - pl / 2, std::max(1.5f, 1.5f * E.scale), pl, C_UI_DIM);
        } else draw_icon(btn[k].ic, bx3 + (float)S(2), py + (hh - (float)S(16)) / 2, S(16), C_UI_DIM);
        hit(bx3 - (float)S(2), py, (float)S(24), hh, H_ACT, btn[k].act);
    }
    /* body */
    float cx = px, cy = py + hh, cw = pw, ch = ph - hh;
    E.term_rx = cx; E.term_ry = cy; E.term_rw = cw; E.term_rh = ch;
    Ed::Term *t = cur_term();
    if (!t || !t->core) { draw_text(E.ui, cx + (float)S(16), ui_base(cy, (float)S(26)), "No terminal. Press Ctrl+Shift+` to start one.", C_UI_FAINT, cw); return; }
    int cols, rows;
    tv_fit(E.font, cw, ch, &cols, &rows);
    if (cols > 0 && rows > 0 && (cols != tcore_cols(t->core) || rows != tcore_rows(t->core))) tcore_resize(t->core, cols, rows);
    tcore_set_focus(t->core, E.term_focus && E.focused);
    r_clip(E.r, (int)cx, (int)cy, (int)cw, (int)ch);
    tv_draw(t->core, E.r, E.font, &E.pal, cx, cy, cw, ch, E.blink_on && E.term_focus);
    r_clip_off(E.r);
}

/* ---- editor polish: indent guides, bracket pairs, scrollbar ---- */

int indent_cols(const std::string &t, bool *blank) {
    int c = 0;
    size_t i = 0;
    for (; i < t.size() && (t[i] == ' ' || t[i] == '\t'); i++) c = t[i] == '\t' ? (c / TAB_W + 1) * TAB_W : c + 1;
    *blank = i == t.size();
    return c;
}

int indent_step(const Doc &d) {   /* the width the status bar reports ("Spaces: 4", "Tab Size: 4") */
    size_t colon = d.indent.find(':');
    int w = colon == std::string::npos ? 4 : atoi(d.indent.c_str() + colon + 1);
    return w >= 2 && w <= 8 ? w : 4;
}

/* the bracket next to the cursor and its partner, as byte offsets (-1 when there is none); plain counting, strings and comments are not skipped */
void bracket_pair(Doc &d, long *a, long *b) {
    if (E.br_doc == &d && E.br_cur == d.cur && E.br_rev == d.buf.revision()) { *a = E.br_a; *b = E.br_b; return; }
    E.br_doc = &d; E.br_cur = d.cur; E.br_rev = d.buf.revision(); E.br_a = E.br_b = -1;
    static const char *OPEN = "([{", *CLOSE = ")]}";
    std::string c;
    for (int k = 0; k < 2 && E.br_a < 0; k++) {
        if (k == 0 && d.cur == 0) continue;
        size_t p = k == 0 ? d.cur - 1 : d.cur;
        if (p >= d.buf.size()) continue;
        d.buf.copy(p, 1, c);
        if (c.empty()) continue;
        const char *o = strchr(OPEN, c[0]), *cl = strchr(CLOSE, c[0]);
        if (c[0] == 0 || (!o && !cl)) continue;
        char me = c[0], want = o ? CLOSE[o - OPEN] : OPEN[cl - CLOSE];
        int depth = 1;
        std::string chunk;
        size_t budget = 200000;
        if (o) {
            for (size_t q = p + 1; q < d.buf.size() && budget > 0;) {
                d.buf.copy(q, 4096, chunk);
                for (size_t i = 0; i < chunk.size(); i++) { if (chunk[i] == me) depth++; else if (chunk[i] == want && --depth == 0) { E.br_a = (long)p; E.br_b = (long)(q + i); break; } }
                if (E.br_a >= 0 || chunk.empty()) break;
                q += chunk.size(); budget = budget > chunk.size() ? budget - chunk.size() : 0;
            }
        } else {
            for (size_t q = p; q > 0 && budget > 0;) {
                size_t from = q > 4096 ? q - 4096 : 0;
                d.buf.copy(from, q - from, chunk);
                for (size_t i = chunk.size(); i-- > 0;) { if (chunk[i] == me) depth++; else if (chunk[i] == want && --depth == 0) { E.br_a = (long)p; E.br_b = (long)(from + i); break; } }
                if (E.br_a >= 0) break;
                budget = budget > q - from ? budget - (q - from) : 0;
                q = from;
            }
        }
    }
    *a = E.br_a; *b = E.br_b;
}

/* thumb geometry of the vertical scrollbar; false when the whole file fits */
bool sb_geom(const Layout &L, const Doc &d, float *ty, float *th, float *thumb_y, float *thumb_h) {
    size_t lines = d.buf.lines();
    if (lines <= (size_t)L.rows || L.rows <= 0) return false;
    *ty = (float)L.text_y;
    *th = (float)(L.ed_bottom - L.text_y);
    float frac = (float)L.rows / (float)lines;
    *thumb_h = std::max((float)S(28), *th * frac);
    float pos = (float)d.top / (float)std::max<size_t>(1, lines - 1);
    *thumb_y = *ty + pos * (*th - *thumb_h);
    return true;
}

void sb_to_pointer() {
    Layout L = layout();
    Doc &d = D();
    float ty, th, y0, h;
    if (!sb_geom(L, d, &ty, &th, &y0, &h)) return;
    float frac = (E.my - ty - h / 2) / std::max(1.f, th - h);
    frac = std::max(0.f, std::min(1.f, frac));
    d.top = (size_t)(frac * (float)(d.buf.lines() - 1));
    redraw();
}

void build_frame() {
    Layout L = layout();
    Doc &d = D();
    if (E.follow) { ensure_visible(L); E.follow = false; }
    E.hits.clear();
    E.hit_floor = (float)L.bar_y;
    size_t cl = d.buf.line_of(d.cur), cb = d.cur - d.buf.line_start(cl);
    std::string t;
    double tm0 = E.timing ? now_s() : 0;

    r_rect(E.r, 0, 0, (float)E.W, (float)E.H, C_EDITOR);

    /* ---- editor column: tab row ---- */
    float ex = (float)L.ed_x, ew = (float)(E.W - L.ed_x);
    r_rect(E.r, ex, (float)L.tab_y, ew, (float)L.tab_h, C_CHROME);
    r_rect(E.r, ex, (float)(L.tab_y + L.tab_h - 1), ew, 1, C_BORDER);
    {
        float tx = ex, ty = (float)L.tab_y, th = (float)L.tab_h;
        if (splash()) hit(tx, ty, ew, th, H_TABSPACE, 0);   /* no tabs yet: a double click anywhere on the strip opens an empty one */
        const size_t nt = splash() ? 0 : E.docs.size();
        std::vector<float> tw(nt);
        float total = 0;
        for (size_t i = 0; i < nt; i++) {
            tw[i] = text_width(E.ui, E.docs[i]->name().c_str()) + (float)S(48);
            total += tw[i];
        }
        if (total > ew) for (float &w : tw) w = std::max((float)S(90), w * ew / total);
        for (size_t i = 0; i < nt; i++) {
            bool act = (int)i == E.active;
            r_rect(E.r, tx, ty, tw[i], th, act ? C_EDITOR : C_CHROME);
            if (act) r_rect(E.r, tx, ty + th - (float)S(2), tw[i], (float)S(2), C_ACCENT);
            r_rect(E.r, tx + tw[i] - 1, ty, 1, th, C_BORDER);
            float ix = tx + (float)S(14);
            const Doc &dd = *E.docs[i];
            std::string nm = dd.name();
            draw_text(E.ui, ix, ui_base(ty, th), nm.c_str(), act ? C_UI : C_UI_DIM, tw[i] - (float)S(46));
            bool mod = dd.modified(), hv = hovered(H_TABCLOSE, (int)i) || hovered(H_TAB, (int)i);
            float cx = tx + tw[i] - (float)S(26), cy = ty + (th - (float)S(16)) / 2;
            if (mod && !hovered(H_TABCLOSE, (int)i)) draw_icon(IC_DOT, cx, cy, S(16), C_UI);
            else if (act || hv) draw_icon(IC_CLOSE, cx, cy, S(16), hovered(H_TABCLOSE, (int)i) ? C_UI : C_UI_DIM);
            hit(tx, ty, tw[i] - (float)S(30), th, H_TAB, (int)i);
            hit(tx + tw[i] - (float)S(30), ty, (float)S(30), th, H_TABCLOSE, (int)i);
            tx += tw[i];
        }
        if (nt) hit(tx, ty, ex + ew - tx, th, H_TABSPACE, 0);   /* the empty part of the strip: a double click opens a new empty tab */
    }
    double tm1 = E.timing ? now_s() : 0;

    /* ---- editor area ---- */
    if (splash()) {
        const char *name = "Hootedit";
        float cxm = ex + ew / 2;
        float w = text_width(E.big, name);
        draw_text(E.big, cxm - w / 2, (float)E.H / 2, name, C_BORDER, (float)E.W);
        const char *hint = "Double click or Ctrl+N: new tab   Ctrl+P: open   Ctrl+B: sidebar";
        draw_text(E.ui, cxm - text_width(E.ui, hint) / 2, (float)E.H / 2 + (float)S(34), hint, C_UI_FAINT, ew);
    } else {
        /* the faint band behind the cursor line also covers the gutter */
        if (cl >= d.top && cl < d.top + (size_t)L.rows) r_rect(E.r, (float)L.ed_x, (float)(L.text_y + (long)(cl - d.top) * E.rowh), (float)(L.text_x - L.ed_x), (float)E.rowh, C_CURLINE);
        r_clip(E.r, L.text_x, L.text_y, E.W - L.text_x, L.ed_bottom - L.text_y);
        /* pass 1: the visible lines, their indentation (blank lines borrow from their neighbours) and the active indent guide */
        int nrows = (int)std::min<size_t>((size_t)L.rows, d.buf.lines() > d.top ? d.buf.lines() - d.top : 0);
        std::vector<std::string> texts((size_t)nrows);
        std::vector<int> ind((size_t)nrows);
        std::vector<char> blank((size_t)nrows);
        for (int row = 0; row < nrows; row++) { d.buf.line_text(d.top + (size_t)row, texts[(size_t)row]); bool bl; ind[(size_t)row] = indent_cols(texts[(size_t)row], &bl); blank[(size_t)row] = bl; }
        const int step = indent_step(d);
        {
            std::vector<int> fixed(ind);
            for (int row = 0; row < nrows; row++) {
                if (!blank[(size_t)row]) continue;
                int up = 0, down = 0;
                for (int k = row - 1; k >= 0; k--) if (!blank[(size_t)k]) { up = ind[(size_t)k]; break; }
                for (int k = row + 1; k < nrows; k++) if (!blank[(size_t)k]) { down = ind[(size_t)k]; break; }
                fixed[(size_t)row] = std::min(up, down);
            }
            ind.swap(fixed);
        }
        int act_col = -1, act_lo = 0, act_hi = -1;   /* the guide just left of the cursor line's text, and the rows it runs along */
        if (cl >= d.top && cl < d.top + (size_t)nrows) {
            int crow = (int)(cl - d.top), ci = ind[(size_t)crow];
            if (ci > 0) {
                act_col = ((ci - 1) / step) * step;
                act_lo = act_hi = crow;
                while (act_lo > 0 && ind[(size_t)act_lo - 1] > act_col) act_lo--;
                while (act_hi + 1 < nrows && ind[(size_t)act_hi + 1] > act_col) act_hi++;
            }
        }
        /* what to mark besides the selection: the other occurrences of a selected word, or every find match */
        std::string occ;
        if (d.has_sel() && d.sel_hi() - d.sel_lo() >= 2 && d.sel_hi() - d.sel_lo() <= 64) { occ = d.selection(); if (occ.find('\n') != std::string::npos) occ.clear(); }
        Regex find_re;
        bool use_find = false;
        if (E.mode == M_FIND && !E.find_q.empty()) {
            bool ic = !E.find_case;
            if (ic) for (unsigned char c : E.find_q) if (c >= 'A' && c <= 'Z') ic = false;
            std::string err;
            use_find = find_re.compile(E.find_regex ? E.find_q : Searcher::escape_literal(E.find_q), ic, &err);
        }
        std::unique_ptr<Matcher> find_mp;
        if (use_find) find_mp.reset(new Matcher(find_re));
        long bra = -1, brb = -1;
        bracket_pair(d, &bra, &brb);
        long bla = bra >= 0 ? (long)d.buf.line_of((size_t)bra) : -1, blb = brb >= 0 ? (long)d.buf.line_of((size_t)brb) : -1;
        /* pass 2: per row, bottom layer first (cursor line, matches, selection, indent guides, bracket pair), then the text */
        for (int row = 0; row < nrows; row++) {
            size_t line = d.top + (size_t)row;
            const std::string &tt = texts[(size_t)row];
            float ry = (float)(L.text_y + row * E.rowh);
            if (line == cl) r_rect(E.r, (float)L.text_x, ry, (float)(E.W - L.text_x), (float)E.rowh, C_CURLINE);
            auto mark = [&](size_t b0, size_t b1, uint32_t col) {
                int c0 = col_of(tt, b0) - d.left, c1 = col_of(tt, b1) - d.left;
                c0 = std::max(c0, 0); c1 = std::min(c1, L.cols);
                if (c1 > c0) r_rect(E.r, (float)(L.text_x + c0 * E.cw), ry, (float)((c1 - c0) * E.cw), (float)E.rowh, col);
            };
            if (use_find) { size_t from = 0, ms, me; int guard = 0; while (from <= tt.size() && guard++ < 200 && find_mp->find(tt.data(), tt.size(), from, &ms, &me)) { if (me > ms) mark(ms, me, C_OCC); from = me > ms ? me : ms + 1; } }
            else if (!occ.empty()) { size_t at = 0; while ((at = tt.find(occ, at)) != std::string::npos) { mark(at, at + occ.size(), C_OCC); at += occ.size(); } }
            if (d.has_sel()) {
                size_t ls = d.buf.line_start(line), le = ls + tt.size();
                size_t lo = d.sel_lo(), hi = d.sel_hi();
                if (hi > ls && lo <= le && !(lo == le && hi == le)) {
                    int c0 = col_of(tt, lo > ls ? lo - ls : 0), c1 = col_of(tt, std::min(hi, le) - ls);
                    if (hi > le && line + 1 < d.buf.lines()) c1++;
                    c0 = std::max(c0 - d.left, 0);
                    c1 = std::min(c1 - d.left, L.cols);
                    if (c1 > c0) r_rect(E.r, (float)(L.text_x + c0 * E.cw), ry, (float)((c1 - c0) * E.cw), (float)E.rowh, C_SEL);
                }
            }
            for (int g = 0; g < ind[(size_t)row]; g += step) {   /* one faint line per indent level that the text is inside of */
                if (g < d.left) continue;
                bool act = g == act_col && row >= act_lo && row <= act_hi;
                r_rect(E.r, (float)(L.text_x + (g - d.left) * E.cw), ry, std::max(1.f, E.scale), (float)E.rowh, act ? C_GUIDE_ACT : C_GUIDE);
            }
            if ((long)line == bla) { size_t b0 = (size_t)bra - d.buf.line_start(line); mark(b0, b0 + 1, C_BRACKET); }
            if ((long)line == blb) { size_t b0 = (size_t)brb - d.buf.line_start(line); mark(b0, b0 + 1, C_BRACKET); }
            double tr0 = E.timing ? now_s() : 0;
            const std::vector<RInst> &v = row_instances(tt, L.cols, d.lang, d.line_state(line));
            if (!v.empty()) r_push_at(E.r, v.data(), (int)v.size(), (float)L.text_x, ry);
            if (E.timing) E.t_rows += now_s() - tr0;
        }
        double tf0 = E.timing ? now_s() : 0;
        r_clip_off(E.r);
        if (E.timing) E.t_flush += now_s() - tf0;
        {   /* a thin scrollbar on the right edge of the text; the thumb is the visible part of the file */
            float sty, sth, sy0, sh;
            if (sb_geom(L, d, &sty, &sth, &sy0, &sh)) {
                float sx = (float)(E.W - S(10));
                bool hot = E.sb_drag || (E.mx >= sx - (float)S(2) && E.my >= sty && E.my < sty + sth);
                r_rect(E.r, sx + (float)S(2), sy0, (float)S(6), sh, hot ? C_SB_HOT : C_SB_THUMB);
                if (cl >= 0) { float cy = sty + ((float)cl / (float)std::max<size_t>(1, d.buf.lines() - 1)) * sth; r_rect(E.r, sx + (float)S(2), cy, (float)S(6), std::max(1.f, E.scale), C_ACCENT); }
                hit((float)(E.W - S(14)), sty, (float)S(14), sth, H_VSCROLL, 0);
            }
        }
        for (int row = 0; row < L.rows; row++) {
            size_t line = d.top + (size_t)row;
            if (line >= d.buf.lines()) break;
            char num[24];
            snprintf(num, sizeof num, "%zu", line + 1);
            float x = (float)(L.ed_x + S(20));   /* left aligned */
            draw_text(E.font, x, (float)(L.text_y + row * E.rowh + E.rowasc), num, line == cl ? C_GUTTER_CUR : C_GUTTER, (float)E.W);
        }
        if (E.blink_on && E.mode == M_EDIT && cl >= d.top && cl < d.top + (size_t)L.rows) {
            d.buf.line_text(cl, t);
            int c = col_of(t, cb) - d.left;
            if (c >= 0 && c <= L.cols) r_rect(E.r, (float)(L.text_x + c * E.cw), (float)(L.text_y + (long)(cl - d.top) * E.rowh), std::max(1.f, 1.5f * E.scale), (float)E.rowh, C_CURSOR);
        }
    }
    double tm3 = E.timing ? now_s() : 0;

    if (L.panel_h > 0) draw_panel(L);

    E.hit_floor = 1e9f;   /* the status bar owns the area below the editor */
    /* ---- status bar ---- */
    float by0 = (float)L.bar_y, bh = (float)L.status_h;
    r_rect(E.r, 0, by0, (float)E.W, bh, C_CHROME);
    r_rect(E.r, 0, by0, (float)E.W, 1, C_BORDER);
    float sy = ui_base(by0, bh);
    float rx = (float)E.W - (float)S(10);
    {   /* right side, right to left: language, line ending, encoding, indent, position */
        d.buf.line_text(cl, t);
        std::string items[5] = {lang_name(d.lang), d.eol, "UTF-8", d.indent, sfmt("Ln %zu, Col %d", cl + 1, col_of(t, cb) + 1)};
        if (d.has_sel()) items[4] += sfmt(" (%zu selected)", d.sel_hi() - d.sel_lo());
        for (int i = 0; i < 5; i++) {
            float w = text_width(E.ui, items[(size_t)i].c_str());
            float x = rx - w;
            if (i == 0) { if (hovered(H_ACT, A_LANG)) r_rect(E.r, x - (float)S(6), by0 + 1, w + (float)S(12), bh - 1, C_HOVER); hit(x - (float)S(6), by0, w + (float)S(12), bh, H_ACT, A_LANG); }
            draw_text(E.ui, x, sy, items[(size_t)i].c_str(), C_UI_DIM, w + 2);
            rx = x - (float)S(18);
        }
    }
    {   /* left side: branch, then the Lestrix-style performance segments */
        float lx = (float)S(10);
        if (E.git.repo) {
            draw_icon(IC_GIT, lx, by0 + (bh - (float)S(13)) / 2, S(13), C_UI_DIM);
            float pen = draw_text(E.ui, lx + (float)S(18), sy, E.git.branch.c_str(), C_UI_DIM, (float)S(160));
            hit(lx, by0, pen - lx, bh, H_ACT, A_VIEW_GIT);
            lx = pen + (float)S(18);
        }
        std::string last = E.ops.empty() ? std::string() : E.ops.back().name + "  " + fmt_ms(E.ops.back().ms);
        std::string left = E.status_msg.empty() ? (last.empty() ? std::string(E.hud) : last + "   |   " + E.hud) : E.status_msg;
        draw_text(E.ui, lx, sy, left.c_str(), E.status_msg.empty() ? C_UI_FAINT : C_UI_DIM, std::max(0.f, rx - lx));
    }
    if (E.timing) { E.t_gutter += tm3 - tm1; E.t_status += now_s() - tm3; E.t_top += tm1 - tm0; }

    /* ---- sidebar ---- */
    E.hit_floor = (float)L.bar_y;
    if (E.side_open) draw_sidebar(L);
    E.hit_floor = 1e9f;   /* menus, the command box and dialogs may go anywhere */

    /* ---- menu bar and command box (drawn last so menus and the box sit above the sidebar) ---- */
    float menu_end;
    draw_menu_bar(L, &menu_end);
    float mh = (float)L.menu_h;
    std::string lname = lang_name(d.lang);
    float lw = text_width(E.ui, lname.c_str()) + (float)S(34);
    float bw = std::min((float)E.W * 0.36f, (float)S(560));
    float bx = std::max(((float)E.W - bw) / 2, menu_end + (float)S(40));
    bw = std::min(bw, (float)E.W - bx - lw - (float)S(24));
    float by = (float)S(3), bhh = mh - (float)S(6);
    if (bw > (float)S(120)) {
        bool active_box = E.mode == M_FIND || E.mode == M_QUICK;
        r_rect(E.r, bx, by, bw, bhh, active_box ? C_BOX_LINE : C_BOX_LINE);
        r_rect(E.r, bx + 1, by + 1, bw - 2, bhh - 2, C_BOX);
        if (active_box) r_rect(E.r, bx, by + bhh - 2, bw, 2, C_ACCENT);
        hit(bx, by, bw, bhh, H_BOX, 0);
        float tx0 = bx + (float)S(10);
        if (E.mode == M_FIND || E.mode == M_QUICK) {
            const std::string &q = E.mode == M_FIND ? E.find_q : E.quick_q;
            float msgw = E.mode == M_FIND && !E.find_msg.empty() ? text_width(E.ui, E.find_msg.c_str()) + (float)S(10) : 0;
            float togw = E.mode == M_FIND ? (float)S(54) : 0.f;
            if (E.mode == M_FIND) {   /* case and regex toggles inside the box, like the sidebar search */
                const char *lab[2] = {"Aa", ".*"};
                int ids[2] = {A_TOG_CASE, A_TOG_REGEX};
                bool on[2] = {E.find_case, E.find_regex};
                float tx = bx + bw - togw - (float)S(4);
                for (int k2 = 0; k2 < 2; k2++) {
                    float w = (float)S(24), th = bhh - (float)S(8);
                    if (on[k2]) r_rect(E.r, tx, by + (float)S(4), w, th, C_ACCENT);
                    else if (hovered(H_ACT, ids[k2])) r_rect(E.r, tx, by + (float)S(4), w, th, C_HOVER);
                    draw_text(E.ui, tx + (w - text_width(E.ui, lab[k2])) / 2, ui_base(by + (float)S(4), th), lab[k2], on[k2] ? C_ON_ACCENT : C_UI_DIM, w);
                    hit(tx, by + (float)S(4), w, th, H_ACT, ids[k2]);
                    tx += w + (float)S(2);
                }
                msgw += togw;
            }
            float pen = draw_text(E.ui, tx0, ui_base(by, bhh), q.c_str(), C_UI, bw - (float)S(20) - msgw);
            if (q.empty()) draw_text(E.ui, tx0, ui_base(by, bhh), E.mode == M_FIND ? "Find in file" : "Type a file name to open", C_UI_FAINT, bw);
            if (E.blink_on) r_rect(E.r, pen, by + (float)S(5), std::max(1.f, E.scale), bhh - (float)S(10), C_CURSOR);
            if (!E.find_msg.empty() && E.mode == M_FIND) draw_text(E.ui, bx + bw - msgw, ui_base(by, bhh), E.find_msg.c_str(), C_RED, msgw);
        } else {
            std::string wn = E.tree.root_name();
            draw_text(E.ui, tx0, ui_base(by, bhh), wn.empty() ? "Hootedit" : wn.c_str(), C_UI_DIM, bw - (float)S(20));
        }
        float lx2 = bx + bw + (float)S(12);
        bool lopen = E.mode == M_PICK && E.pick_kind == PICK_LANG;
        if (lopen || hovered(H_ACT, A_LANG)) r_rect(E.r, lx2, by, lw, bhh, lopen ? C_MENU_SEL : C_HOVER);
        draw_text(E.ui, lx2 + (float)S(8), ui_base(by, bhh), lname.c_str(), d.lang_forced ? C_UI : C_UI_DIM, lw - (float)S(24));
        draw_icon(IC_CHEV_D, lx2 + lw - (float)S(20), by + (bhh - (float)S(14)) / 2, S(14), C_UI_DIM);
        hit(lx2, by, lw, bhh, H_ACT, A_LANG);
    }
    r_rect(E.r, 0, mh - 1, (float)E.W, 1, C_BORDER);

    /* ---- overlays ---- */
    if (E.mode == M_MENU && E.menu_open >= 0) {
        const Menu &m = MENUS[(size_t)E.menu_open];
        float x = (float)S(8);
        for (int i = 0; i < E.menu_open; i++) x += text_width(E.ui, MENUS[(size_t)i].name) + (float)S(14);
        float iw = 0;
        for (const MenuItem &it : m.items) iw = std::max(iw, text_width(E.ui, it.label) + text_width(E.ui, it.keys) + (float)S(60));
        iw = std::max(iw, (float)S(200));
        float ih = (float)S(24), sep = (float)S(9);
        float ph = (float)S(8);
        for (const MenuItem &it : m.items) ph += it.label[0] ? ih : sep;
        r_rect(E.r, x, mh, iw, ph, C_MENU_BORDER);
        r_rect(E.r, x + 1, mh, iw - 2, ph - 1, C_MENU);
        float y = mh + (float)S(4);
        for (size_t i = 0; i < m.items.size(); i++) {
            const MenuItem &it = m.items[i];
            if (!it.label[0]) { r_rect(E.r, x + (float)S(8), y + sep / 2, iw - (float)S(16), 1, C_MENU_BORDER); y += sep; continue; }
            bool enabled = it.act != A_NONE;
            if (enabled && (int)i == E.menu_sel) r_rect(E.r, x + (float)S(3), y, iw - (float)S(6), ih, C_MENU_SEL);
            uint32_t col = enabled ? C_UI : C_UI_FAINT;
            draw_text(E.ui, x + (float)S(26), ui_base(y, ih), it.label, col, iw);
            float kw = text_width(E.ui, it.keys);
            draw_text(E.ui, x + iw - kw - (float)S(14), ui_base(y, ih), it.keys, C_UI_FAINT, kw + 2);
            if (enabled) hit(x, y, iw, ih, H_MENUITEM, (int)i);
            y += ih;
        }
    }
    if (E.mode == M_QUICK && !E.quick_hits.empty()) {
        float pw = bw, px = bx, py = mh - 1;
        float ih = (float)S(24), ph = (float)E.quick_hits.size() * ih + (float)S(8);
        r_rect(E.r, px, py, pw, ph, C_MENU_BORDER);
        r_rect(E.r, px + 1, py, pw - 2, ph - 1, C_MENU);
        size_t rl = E.tree.root().size() + 1;
        for (size_t i = 0; i < E.quick_hits.size(); i++) {
            float y = py + (float)S(4) + (float)i * ih;
            if ((int)i == E.quick_sel) r_rect(E.r, px + (float)S(3), y, pw - (float)S(6), ih, C_MENU_SEL);
            std::string rel = E.quick_hits[i].substr(std::min(rl, E.quick_hits[i].size()));
            size_t sl = rel.rfind('/');
            std::string base = sl == std::string::npos ? rel : rel.substr(sl + 1), dir = sl == std::string::npos ? "" : rel.substr(0, sl);
            draw_icon(IC_FILE, px + (float)S(10), y + (ih - (float)S(14)) / 2, S(14), file_color(base));
            const std::vector<int> &ps = i < E.quick_pos.size() ? E.quick_pos[i] : std::vector<int>();
            float pen = draw_text_hl(E.ui, px + (float)S(32), ui_base(y, ih), base, ps, (int)(sl == std::string::npos ? 0 : sl + 1), C_UI, C_ACCENT, pw - (float)S(40));
            if (!dir.empty()) draw_text_hl(E.ui, pen + (float)S(8), ui_base(y, ih), dir, ps, 0, C_UI_FAINT, C_ACCENT, px + pw - pen - (float)S(16));
            hit(px, y, pw, ih, H_QUICKITEM, (int)i);
        }
    }
    if (E.mode == M_PICK) {
        std::vector<int> view = pick_view();
        float ih = (float)S(24);
        int maxrows = std::max(3, std::min(16, (int)(((float)E.H - mh - (float)S(60)) / ih)));
        int n = (int)view.size();
        E.pick_sel = std::max(0, std::min(E.pick_sel, n - 1));
        if (E.pick_sel < E.pick_scroll) E.pick_scroll = E.pick_sel;
        if (E.pick_sel >= E.pick_scroll + maxrows) E.pick_scroll = E.pick_sel - maxrows + 1;
        float pw = std::max(bw, (float)S(340)), px = std::min(std::max(0.f, bx), (float)E.W - pw - (float)S(4));
        float py = mh - 1, ph = (float)(std::min(n, maxrows) + 1) * ih + (float)S(8);
        r_rect(E.r, px, py, pw, ph, C_MENU_BORDER);
        r_rect(E.r, px + 1, py, pw - 2, ph - 1, C_MENU);
        const char *ph_txt = E.pick_kind == PICK_LANG ? "Select Language Mode" : E.pick_kind == PICK_THEME ? "Select Color Theme" : "Select the Terminal";
        std::string fl = E.pick_filter.empty() ? ph_txt : E.pick_filter;
        float pen = draw_text(E.ui, px + (float)S(12), ui_base(py + (float)S(4), ih), fl.c_str(), E.pick_filter.empty() ? C_UI_FAINT : C_UI, pw - (float)S(24));
        if (E.blink_on && !E.pick_filter.empty()) r_rect(E.r, pen, py + (float)S(8), std::max(1.f, E.scale), ih - (float)S(8), C_CURSOR);
        for (int k = 0; k < maxrows && E.pick_scroll + k < n; k++) {
            const Ed::PItem &it = E.pick_items[(size_t)view[(size_t)(E.pick_scroll + k)]];
            float iy = py + (float)S(4) + (float)(k + 1) * ih;
            if (E.pick_scroll + k == E.pick_sel) r_rect(E.r, px + (float)S(3), iy, pw - (float)S(6), ih, C_MENU_SEL);
            draw_text(E.ui, px + (float)S(16), ui_base(iy, ih), it.label.c_str(), it.current ? C_ACCENT : C_UI, pw - (float)S(32));
            hit(px, iy, pw, ih, H_PICKITEM, E.pick_scroll + k);
        }
    }
    if (E.mode == M_CONFIRM || E.mode == M_PROMPT) {
        r_rect(E.r, 0, 0, (float)E.W, (float)E.H, C_SHADE);
        float pw = std::min((float)E.W - (float)S(24), (float)S(560)), ph = (float)S(118);
        float px = ((float)E.W - pw) / 2, py = ((float)E.H - ph) / 2;
        r_rect(E.r, px, py, pw, ph, C_MENU_BORDER);
        r_rect(E.r, px + 1, py + 1, pw - 2, ph - 2, C_MENU);
        if (E.mode == M_CONFIRM) {
            std::string msg = "Save changes to " + E.docs[(size_t)E.confirm_idx]->name() + " before closing?";
            draw_text(E.ui, px + (float)S(18), ui_base(py + (float)S(14), (float)S(24)), msg.c_str(), C_UI, pw - (float)S(36));
            const char *lab[3] = {"Save (S)", "Don't save (D)", "Cancel (Esc)"};
            int kind[3] = {H_BTN_SAVE, H_BTN_DISCARD, H_BTN_CANCEL};
            float bx2 = px + (float)S(18), byy = py + (float)S(60), bth = (float)S(30);
            for (int k = 0; k < 3; k++) {
                float w = text_width(E.ui, lab[k]) + (float)S(28);
                r_rect(E.r, bx2, byy, w, bth, k == 0 ? C_ACCENT : C_HOVER);
                draw_text(E.ui, bx2 + (float)S(14), ui_base(byy, bth), lab[k], k == 0 ? C_ON_ACCENT : C_UI, w);
                hit(bx2, byy, w, bth, kind[k], 0);
                bx2 += w + (float)S(10);
            }
        } else {
            const char *what = E.prompt_kind == PK_SAVEAS ? "Save as" : E.prompt_kind == PK_OPEN_FILE ? "Open file" : "Open folder";
            std::string msg = std::string(what) + " (Enter to confirm, Esc to cancel):";
            draw_text(E.ui, px + (float)S(18), ui_base(py + (float)S(12), (float)S(24)), msg.c_str(), C_UI, pw - (float)S(36));
            r_rect(E.r, px + (float)S(18), py + (float)S(46), pw - (float)S(36), (float)S(30), C_BOX_LINE);
            r_rect(E.r, px + (float)S(19), py + (float)S(47), pw - (float)S(38), (float)S(28), C_BOX);
            float pen = draw_text(E.ui, px + (float)S(28), ui_base(py + (float)S(46), (float)S(30)), E.prompt_q.c_str(), C_UI, pw - (float)S(56));
            if (E.blink_on) r_rect(E.r, pen, py + (float)S(52), std::max(1.f, E.scale), (float)S(18), C_CURSOR);
            if (!E.status_msg.empty()) draw_text(E.ui, px + (float)S(18), ui_base(py + (float)S(84), (float)S(22)), E.status_msg.c_str(), C_RED, pw - (float)S(36));
        }
    }
    if (E.mode == M_FILEDLG) {
        r_rect(E.r, 0, 0, (float)E.W, (float)E.H, C_SHADE);
        float pw = std::min((float)E.W - (float)S(24), (float)S(700)), ph = std::min((float)E.H - (float)S(24), (float)S(520));
        float px = ((float)E.W - pw) / 2, py = ((float)E.H - ph) / 2;
        r_rect(E.r, px, py, pw, ph, C_MENU_BORDER);
        r_rect(E.r, px + 1, py + 1, pw - 2, ph - 2, C_MENU);
        r_rect(E.r, px, py, pw, (float)S(2), C_ACCENT);
        float ix = px + (float)S(18), iw = pw - (float)S(36);
        const char *title = E.fd.kind == FD_OPEN_FOLDER ? "Open Folder" : E.fd.kind == FD_OPEN_FILE ? "Open File" : "Save As";
        draw_text(E.ui, ix, ui_base(py + (float)S(10), (float)S(28)), title, C_UI, iw, FS_BOLD);
        std::string htxt = std::string("hidden files: ") + (E.fd.hidden ? "shown" : "hidden") + "   (Ctrl+H)";
        float hw = text_width(E.ui, htxt.c_str());
        if (hovered(H_FD_HIDDEN, 0)) r_rect(E.r, px + pw - hw - (float)S(24), py + (float)S(10), hw + (float)S(12), (float)S(28), C_HOVER);
        draw_text(E.ui, px + pw - hw - (float)S(18), ui_base(py + (float)S(10), (float)S(28)), htxt.c_str(), C_UI_DIM, hw + 2);
        hit(px + pw - hw - (float)S(24), py + (float)S(10), hw + (float)S(12), (float)S(28), H_FD_HIDDEN, 0);
        /* places and the current folder */
        float y = py + (float)S(46);
        const char *places[3] = {"Home", "Workspace", "/"};
        float bx3 = ix;
        for (int k2 = 0; k2 < 3; k2++) {
            float w = text_width(E.ui, places[k2]) + (float)S(20), h2 = (float)S(26);
            r_rect(E.r, bx3, y, w, h2, hovered(H_FD_PLACE, k2) ? C_HOVER : C_FIELD);
            draw_text(E.ui, bx3 + (float)S(10), ui_base(y, h2), places[k2], C_UI, w);
            hit(bx3, y, w, h2, H_FD_PLACE, k2);
            bx3 += w + (float)S(6);
        }
        std::string dirshow = E.fd.dir;
        float maxw = ix + iw - bx3 - (float)S(8);
        while (dirshow.size() > 8 && text_width(E.ui, dirshow.c_str()) > maxw) dirshow = dirshow.substr(dirshow.size() > 40 ? 6 : 2);   /* cut the front of a long path */
        if (dirshow != E.fd.dir) dirshow = "..." + dirshow;
        draw_text(E.ui, bx3 + (float)S(8), ui_base(y, (float)S(26)), dirshow.c_str(), C_UI_DIM, maxw);
        /* filter / path box */
        y += (float)S(36);
        float fh = (float)S(30);
        bool list_focus = !(E.fd.kind == FD_SAVE && E.fd.name_focus);
        r_rect(E.r, ix, y, iw, fh, list_focus ? C_ACCENT : C_BOX_LINE);
        r_rect(E.r, ix + 1, y + 1, iw - 2, fh - 2, C_FIELD);
        float pen = draw_text(E.ui, ix + (float)S(10), ui_base(y, fh), E.fd.filter.c_str(), C_UI, iw - (float)S(20));
        if (E.fd.filter.empty()) draw_text(E.ui, ix + (float)S(10), ui_base(y, fh), "Type to filter, or a path starting with / or ~", C_UI_FAINT, iw - (float)S(20));
        if (list_focus && E.blink_on) r_rect(E.r, pen, y + (float)S(6), std::max(1.f, E.scale), fh - (float)S(12), C_CURSOR);
        /* the list */
        y += fh + (float)S(8);
        float footer = E.fd.kind == FD_SAVE ? (float)S(104) : (float)S(64);
        float lh = py + ph - footer - y, rowh2 = (float)S(26);
        std::vector<int> view = fd_view();
        int n = (int)view.size(), maxrows = std::max(1, (int)(lh / rowh2));
        E.fd.sel = std::max(0, std::min(E.fd.sel, std::max(0, n - 1)));
        if (E.fd.sel < E.fd.scroll) E.fd.scroll = E.fd.sel;
        if (E.fd.sel >= E.fd.scroll + maxrows) E.fd.scroll = E.fd.sel - maxrows + 1;
        E.fd.scroll = std::max(0, std::min(E.fd.scroll, std::max(0, n - maxrows)));
        r_rect(E.r, ix, y, iw, lh, C_EDITOR);
        r_clip(E.r, (int)ix, (int)y, (int)iw, (int)lh);
        for (int k2 = 0; k2 < maxrows && E.fd.scroll + k2 < n; k2++) {
            int vi = E.fd.scroll + k2;
            const Ed::FdEntry &en = E.fd.all[(size_t)view[(size_t)vi]];
            float ry2 = y + (float)k2 * rowh2;
            bool selr = vi == E.fd.sel && (E.fd.picked || E.fd.kind != FD_OPEN_FOLDER);
            if (selr) r_rect(E.r, ix, ry2, iw, rowh2, E.fd.picked ? C_MENU_SEL : C_HOVER);
            else if (hovered(H_FD_ITEM, vi)) r_rect(E.r, ix, ry2, iw, rowh2, C_HOVER);
            draw_icon(en.name == ".." ? IC_ARROW_L : en.dir ? IC_FOLDER : IC_FILE, ix + (float)S(10), ry2 + (rowh2 - (float)S(16)) / 2, S(16), en.dir ? C_ACCENT : file_color(en.name));
            std::string label = en.name == ".." ? "..  (up one folder)" : en.name + (en.dir ? "/" : "");
            draw_text(E.ui, ix + (float)S(36), ui_base(ry2, rowh2), label.c_str(), en.dir ? C_UI : C_UI_DIM, iw - (float)S(46));
            hit(ix, ry2, iw, rowh2, H_FD_ITEM, vi);
        }
        r_clip_off(E.r);
        if (n == 0) draw_text(E.ui, ix + (float)S(12), ui_base(y, rowh2), "nothing matches", C_UI_FAINT, iw);
        /* footer: file name when saving, a message, the buttons */
        float fy = y + lh + (float)S(8);
        if (E.fd.kind == FD_SAVE) {
            draw_text(E.ui, ix, ui_base(fy, (float)S(30)), "Name:", C_UI_DIM, (float)S(60));
            float nx = ix + (float)S(56), nw = iw - (float)S(56);
            r_rect(E.r, nx, fy, nw, (float)S(30), E.fd.name_focus ? C_ACCENT : C_BOX_LINE);
            r_rect(E.r, nx + 1, fy + 1, nw - 2, (float)S(28), C_FIELD);
            float np = draw_text(E.ui, nx + (float)S(10), ui_base(fy, (float)S(30)), E.fd.name.c_str(), C_UI, nw - (float)S(20));
            if (E.fd.name_focus && E.blink_on) r_rect(E.r, np, fy + (float)S(6), std::max(1.f, E.scale), (float)S(18), C_CURSOR);
            hit(nx, fy, nw, (float)S(30), H_FD_PLACE, 99);
            fy += (float)S(40);
        }
        if (!E.fd.msg.empty()) draw_text(E.ui, ix, ui_base(fy, (float)S(24)), E.fd.msg.c_str(), C_RED, iw - (float)S(280));
        std::string oklabel = E.fd.kind == FD_OPEN_FOLDER ? "Open Folder" : E.fd.kind == FD_OPEN_FILE ? "Open" : "Save";
        const char *lab2[2] = {"Cancel", oklabel.c_str()};
        float bx4 = ix + iw;
        for (int k2 = 1; k2 >= 0; k2--) {
            float w = text_width(E.ui, lab2[k2]) + (float)S(36), h2 = (float)S(30);
            bx4 -= w;
            r_rect(E.r, bx4, fy, w, h2, k2 == 1 ? C_ACCENT : (hovered(H_FD_CANCEL, 0) ? C_HOVER : C_FIELD));
            draw_text(E.ui, bx4 + (float)S(18), ui_base(fy, h2), lab2[k2], k2 == 1 ? C_ON_ACCENT : C_UI, w);
            hit(bx4, fy, w, h2, k2 == 1 ? H_FD_OK : H_FD_CANCEL, 0);
            bx4 -= (float)S(8);
        }
    }
    if (E.show_stats) {
        std::string s = cache_stats_text();
        std::vector<std::string> ls;
        size_t p = 0;
        while (p <= s.size()) { size_t q = s.find('\n', p); if (q == std::string::npos) q = s.size(); ls.push_back(s.substr(p, q - p)); p = q + 1; }
        float pw = 0;
        for (auto &l : ls) pw = std::max(pw, text_width(E.ui, l.c_str()));
        pw = std::min(pw, (float)E.W - (float)S(60));
        float lh = (float)S(22), ph = (float)ls.size() * lh + (float)S(28);
        float px = ((float)E.W - pw) / 2 - (float)S(14), py = ((float)E.H - ph) / 2;
        r_rect(E.r, px, py, pw + (float)S(28), ph, C_MENU_BORDER);
        r_rect(E.r, px + 1, py + 1, pw + (float)S(26), ph - 2, C_MENU);
        for (size_t i = 0; i < ls.size(); i++) draw_text(E.ui, px + (float)S(14), ui_base(py + (float)S(14) + (float)i * lh, lh), ls[i].c_str(), C_UI, pw);
    }
}

/* ---- status segments (same numbers as the Lestrix bar) ---- */

void hud_tick() {
    double dt = E.prev_time ? E.now - E.prev_time : 0;
    long ticks = proc_cpu_ticks();
    double cpu = dt > 0 ? (double)(ticks - E.prev_ticks) / (double)sysconf(_SC_CLK_TCK) / dt * 100 : 0;
    double fps = dt > 0 ? (double)(E.frames - E.prev_frames) / dt : 0;
    CacheStats d = {sd_cache.glyph_hit - E.prev_cache.glyph_hit, sd_cache.glyph_miss - E.prev_cache.glyph_miss, sd_cache.glyph_recycle - E.prev_cache.glyph_recycle,
                    sd_cache.row_hit - E.prev_cache.row_hit, sd_cache.row_miss - E.prev_cache.row_miss, sd_cache.row_recycle - E.prev_cache.row_recycle};
    E.prev_cache = sd_cache; E.prev_time = E.now; E.prev_ticks = ticks; E.prev_frames = E.frames;
    char rp[16], gp[16], rc[24];
    uint64_t rt = d.row_hit + d.row_miss, gt = d.glyph_hit + d.glyph_miss;
    if (rt) snprintf(rp, sizeof rp, "%.0f%%", 100.0 * (double)d.row_hit / (double)rt); else snprintf(rp, sizeof rp, "--");
    if (gt) snprintf(gp, sizeof gp, "%.0f%%", 100.0 * (double)d.glyph_hit / (double)gt); else snprintf(gp, sizeof gp, "--");
    fmt_count((double)(d.row_recycle + d.glyph_recycle), rc, sizeof rc);
    snprintf(E.hud, sizeof E.hud, "fps %.0f  cpu %.0f%% (%ld)  mem %s  cache rows %s glyphs %s recyc %s", fps, cpu,
             sysconf(_SC_NPROCESSORS_ONLN), fmt_mb((double)proc_rss_kb() * 1024).c_str(), rp, gp, rc);
    if (strcmp(E.hud, E.hud_last) != 0) { memcpy(E.hud_last, E.hud, sizeof E.hud); redraw(); }
}

/* ---- window ---- */

bool gl_init(GlKind kind, int w, int h) {
    if (E.gl) { SDL_GL_DeleteContext(E.gl); E.gl = nullptr; }
    if (E.win) { SDL_DestroyWindow(E.win); E.win = nullptr; }
    if (kind == GLK_CORE) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kind == GLK_ES3 ? 3 : 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    }
    E.win = SDL_CreateWindow("Hootedit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (E.win) {   /* the owl as the window icon (64x64, embedded) */
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)APP_ICON_RGBA, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(E.win, ic); SDL_FreeSurface(ic); }
    }
    E.gl = E.win ? SDL_GL_CreateContext(E.win) : nullptr;
    if (!E.gl || sd_gl_load(kind)) return false;
    SDL_GL_SetSwapInterval(1);
    int ww, wh;
    SDL_GetWindowSize(E.win, &ww, &wh);
    SDL_GL_GetDrawableSize(E.win, &E.W, &E.H);
    E.scale = ww ? (float)E.W / (float)ww : 1.f;
    E.atlas = atlas_new(E.scale > 1.5f ? 4096 : 2048);
    E.r = E.atlas ? r_new(E.atlas) : nullptr;
    if (!E.r) return false;
    const char *ver = (const char *)glGetString(GL_VERSION), *rend = (const char *)glGetString(GL_RENDERER);
    snprintf(E.gl_desc, sizeof E.gl_desc, "%s on %s", ver ? ver : "OpenGL", rend ? rend : "unknown GPU");
    return true;
}

void write_ppm(const char *file) {
    std::vector<uint8_t> px((size_t)E.W * (size_t)E.H * 4);
    glReadPixels(0, 0, E.W, E.H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE *f = fopen(file, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", E.W, E.H);
    for (int y = E.H - 1; y >= 0; y--)
        for (int x = 0; x < E.W; x++) fwrite(&px[((size_t)y * (size_t)E.W + (size_t)x) * 4], 1, 3, f);
    fclose(f);
}

/* ---- input ---- */

size_t pos_from_xy(int mx, int my) {
    Layout L = layout();
    Doc &d = D();
    long row = (my - L.text_y) / E.rowh;
    if (my < L.text_y) row = -1;
    long line = (long)d.top + row;
    line = std::max(0L, std::min(line, (long)d.buf.lines() - 1));
    std::string t;
    d.buf.line_text((size_t)line, t);
    int col = d.left + (mx - L.text_x) / E.cw;
    if (mx < L.text_x) col = 0;
    return d.buf.line_start((size_t)line) + byte_of(t, col);
}

void move_line(long delta, bool extend) {
    Doc &d = D();
    size_t l = d.buf.line_of(d.cur);
    long nl = (long)l + delta;
    if (nl < 0) { d.move_to(0, extend, false); return; }
    if (nl >= (long)d.buf.lines()) { d.move_to(d.buf.size(), extend, false); return; }
    std::string t;
    d.buf.line_text((size_t)nl, t);
    d.move_to(d.buf.line_start((size_t)nl) + byte_of(t, d.want_col), extend, true);
}

void pop_utf8(std::string &s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    s.resize(i);
}

/* ---- terminal panel: the Lestrix terminal engine drawn into a rectangle at the bottom of the editor ---- */

Ed::Term *cur_term() { return E.term_active >= 0 && E.term_active < (int)E.terms.size() ? E.terms[(size_t)E.term_active].get() : nullptr; }

void thook_wake(void *) { app_wake(); }
void thook_exited(void *u, int) { static_cast<Ed::Term *>(u)->dead = true; app_wake(); }
void thook_title(void *u, const char *title) { if (title && *title) static_cast<Ed::Term *>(u)->title = title; redraw(); }
void thook_clip_set(void *, const char *text, bool primary) { if (primary) SDL_SetPrimarySelectionText(text); else SDL_SetClipboardText(text); }
void thook_clip_request(void *u, bool primary) {
    Ed::Term *t = static_cast<Ed::Term *>(u);
    char *c = primary ? SDL_GetPrimarySelectionText() : SDL_GetClipboardText();
    if (c && *c && t->core) tcore_paste(t->core, c);
    SDL_free(c);
}

bool new_terminal() {
    const char *sh = getenv("SHELL");
    std::string shell = sh && *sh ? sh : "/bin/sh";
    std::string cwd = E.tree.root().empty() ? "." : E.tree.root();
    TermHooks h{};
    h.wake = thook_wake; h.exited = thook_exited; h.title = thook_title; h.clip_set = thook_clip_set; h.clip_request = thook_clip_request;
    auto t = std::make_unique<Ed::Term>();
    int cols = 80, rows = 24;
    Layout L = layout();
    tv_fit(E.font, (float)(E.W - L.ed_x), (float)(S(E.panel_logical) - S(30)), &cols, &rows);
    char *argv[] = {const_cast<char *>(shell.c_str()), nullptr};
    t->core = tcore_new(argv, cwd.c_str(), std::max(cols, 20), std::max(rows, 4), &h, t.get(), true);
    if (!t->core) { E.status_msg = "cannot start " + shell; return false; }
    size_t sl = shell.rfind('/');
    t->title = sl == std::string::npos ? shell : shell.substr(sl + 1);
    E.terms.push_back(std::move(t));
    E.term_active = (int)E.terms.size() - 1;
    E.term_open = true;
    E.term_focus = true;
    return true;
}

void kill_terminal(int i) {
    if (i < 0 || i >= (int)E.terms.size()) return;
    Ed::Term *t = E.terms[(size_t)i].get();
    if (t->core) { tcore_close(t->core); tcore_free(t->core); t->core = nullptr; }
    E.terms.erase(E.terms.begin() + i);
    if (E.terms.empty()) { E.term_active = -1; E.term_open = false; E.term_focus = false; }
    else E.term_active = std::min(E.term_active > i ? E.term_active - 1 : E.term_active, (int)E.terms.size() - 1);
    redraw();
}

void terms_pump() {
    bool changed = false;
    for (auto &t : E.terms) if (t->core && tcore_pump(t->core)) changed = true;
    if (changed) { E.last_active = E.now; redraw(); }
    for (size_t i = E.terms.size(); i-- > 0;) if (E.terms[i]->dead) kill_terminal((int)i);   /* a shell that exited closes its tab */
}

void mark_terms_dirty() {
    for (auto &t : E.terms) if (t->core) { tcore_lock(t->core); vt_mark_all_dirty(tcore_vt(t->core)); tcore_unlock(t->core); }
}

/* external terminals: what is installed, and how to ask each one to start in a directory */
struct ExtKnown { const char *name, *dirflag; };
const ExtKnown EXT_KNOWN[] = {
    {"x-terminal-emulator", ""}, {"gnome-terminal", "--working-directory="}, {"konsole", "--workdir "}, {"xfce4-terminal", "--working-directory="},
    {"mate-terminal", "--working-directory="}, {"tilix", "--working-directory="}, {"terminator", "--working-directory="}, {"kitty", "--directory "},
    {"alacritty", "--working-directory "}, {"foot", "--working-directory "}, {"wezterm", "start --cwd "}, {"lestrix", "--working-directory "}, {"xterm", ""},
};

bool on_path(const std::string &name) {
    const char *path = getenv("PATH");
    if (!path) return false;
    std::string p = path;
    for (size_t b = 0; b <= p.size();) {
        size_t e = p.find(':', b);
        if (e == std::string::npos) e = p.size();
        std::string f = p.substr(b, e - b) + "/" + name;
        if (access(f.c_str(), X_OK) == 0) return true;
        b = e + 1;
    }
    return false;
}

void detect_externals() {
    E.term_ext.clear();
    for (const ExtKnown &k : EXT_KNOWN) if (on_path(k.name)) E.term_ext.push_back({k.name, k.dirflag});
    const char *t = getenv("TERMINAL");
    if (t && *t && on_path(t)) { bool have = false; for (auto &x : E.term_ext) if (x.name == t) have = true; if (!have) E.term_ext.push_back({t, ""}); }
}

void launch_external(std::string name) {
    if (name.empty()) { detect_externals(); if (E.term_ext.empty()) { E.status_msg = "no terminal emulator found"; redraw(); return; } name = E.term_ext[0].name; }
    std::string flag;
    for (const ExtKnown &k : EXT_KNOWN) if (name == k.name) flag = k.dirflag;
    std::string cwd = E.tree.root().empty() ? "." : E.tree.root();
    std::vector<std::string> args = {name};
    if (!flag.empty()) {
        if (flag.back() == '=') args.push_back(flag + cwd);
        else { size_t b = 0; while (b < flag.size()) { size_t e = flag.find(' ', b); if (e == std::string::npos) e = flag.size(); if (e > b) args.push_back(flag.substr(b, e - b)); b = e + 1; } args.push_back(cwd); }
    }
    std::vector<char *> argv;
    for (std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {   /* double fork: the terminal is not our child, so there is nothing to reap and no SIGCHLD handling to disturb */
        if (fork() != 0) _exit(0);
        setsid();
        if (chdir(cwd.c_str()) != 0) {}
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
    E.status_msg = "opened " + name;
    redraw();
}

void apply_theme(const UiTheme *t) {
    if (E.theme && E.frames) begin_op("theme switch");
    E.theme = t;
    const char *acc = E.follow_fleet && E.fleet.found && !E.fleet.accent.empty() ? E.fleet.accent.c_str() : t->accent;
    uint32_t bg0 = ui_rgba(t->bg0, 255), bg1 = ui_rgba(t->bg1, 255), bg2 = ui_rgba(t->bg2, 255), ink = ui_rgba(t->ink, 255);
    uint32_t accent = ui_rgba(acc, 255), tbg = ui_rgba(t->term_bg, 255), tfg = ui_rgba(t->term_fg, 255);
    C_CHROME = bg1; C_MENUBAR = bg0; C_EDITOR = tbg; C_FIELD = bg2; C_SELROW = ui_mix(bg1, ink, 0.10);
    C_HOVER = ui_mix(bg1, ink, 0.06); C_BORDER = ui_mix(bg1, ink, 0.16); C_GROUP = ui_mix(bg1, ink, 0.40);
    C_UI = ink; C_UI_DIM = ui_mix(bg1, ink, 0.70); C_UI_FAINT = ui_mix(bg1, ink, 0.48);
    C_ACCENT = accent; C_FG = tfg; C_CURSOR = tfg;
    C_GUTTER = ui_mix(tbg, tfg, 0.40); C_GUTTER_CUR = ui_mix(tbg, tfg, 0.85);
    C_MENU = (ui_mix(bg1, bg0, 0.25) & 0x00ffffffu) | (250u << 24); C_MENU_BORDER = ui_mix(bg1, ink, 0.28); C_MENU_SEL = ui_mix(bg1, accent, 0.40);
    C_CURLINE = ui_mix(tbg, tfg, 0.055);   /* the faint band behind the cursor line */
    C_GUIDE = ui_mix(tbg, tfg, 0.13); C_GUIDE_ACT = ui_mix(tbg, tfg, 0.38);   /* indent guides, and the one of the block the cursor is in */
    C_OCC = ui_mix(tbg, accent, 0.24); C_BRACKET = ui_mix(tbg, accent, 0.50);
    C_SB_THUMB = ui_mix(tbg, tfg, 0.20); C_SB_HOT = ui_mix(tbg, tfg, 0.38);
    C_SEL = ui_mix(tbg, accent, 0.38); C_BOX = ui_mix(bg1, bg0, 0.55); C_BOX_LINE = ui_mix(bg1, ink, 0.22);
    C_RED = ui_rgba(t->red, 255); C_GREEN = ui_rgba(t->up, 255); C_ORANGE = ui_rgba(t->ansi[3], 255);
    unsigned r = accent & 255, g = (accent >> 8) & 255, b = (accent >> 16) & 255;
    C_ON_ACCENT = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0 > 0.47 ? RGBA(16, 18, 20, 255) : RGBA(255, 255, 255, 255);
    /* syntax colours from the theme's ANSI palette: bright blue keywords, cyan types, yellow functions, orange strings, dim green comments */
    auto an = [&](int i) { return ui_rgba(t->ansi[i], 255); };
    TOKCOL[T_PLAIN] = tfg;
    TOKCOL[T_COMMENT] = ui_mix(tbg, an(2), 0.70);
    TOKCOL[T_STRING] = ui_mix(an(3), an(1), 0.45);
    TOKCOL[T_NUMBER] = an(10);
    TOKCOL[T_KEYWORD] = an(12);
    TOKCOL[T_TYPE] = an(14);
    TOKCOL[T_PREPROC] = an(13);
    TOKCOL[T_PUNCT] = tfg;
    TOKCOL[T_FUNC] = an(11);
    TOKCOL[T_KEY] = an(12);
    TOKCOL[T_HEADING] = an(12);
    TOKCOL[T_INS] = an(2);
    TOKCOL[T_DEL] = an(1);
    ui_palette(&E.pal, t, acc);
    mark_terms_dirty();
    E.rows.clear();
    redraw();
}

void follow_fleetwm() {
    fleetwm_read(&E.fleet);
    if (!E.follow_fleet || !E.fleet.found) return;
    const char *name = fleetwm_theme_name(E.fleet.theme);
    apply_theme(ui_theme_find(name ? name : "fleetwm Dark"));
}

void open_picker(PickKind k) {
    begin_op(k == PICK_LANG ? "language picker" : k == PICK_THEME ? "theme picker" : "terminal picker");
    E.pick_kind = k;
    E.pick_filter.clear();
    E.pick_items.clear();
    E.pick_sel = E.pick_scroll = 0;
    Doc &d = D();
    if (k == PICK_LANG) {
        E.pick_items.push_back({"Auto Detect (" + std::string(lang_name(lang_detect(d.path.c_str(), nullptr, 0))) + ")", -1, !d.lang_forced});
        for (int i = 0; i < lang_count(); i++) E.pick_items.push_back({lang_name(lang_at(i)), i, d.lang_forced && d.lang == lang_at(i)});
    } else if (k == PICK_THEME) {
        E.pick_items.push_back({"Follow fleetwm theme" + std::string(E.fleet.found ? "" : " (fleetwm not found)"), -2, E.follow_fleet});
        for (int i = 0; i < UI_THEME_COUNT; i++) E.pick_items.push_back({UI_THEMES[i].name, i, !E.follow_fleet && E.theme == &UI_THEMES[i]});
    }
    else if (k == PICK_TERMINAL) {
        detect_externals();
        E.pick_items.push_back({"Built-in terminal (Lestrix engine, in the panel)", 0, E.term_choice == "builtin"});
        for (size_t i = 0; i < E.term_ext.size(); i++) {
            std::string label = E.term_ext[i].name == "x-terminal-emulator" ? "System default (x-terminal-emulator)" : E.term_ext[i].name;
            E.pick_items.push_back({"External: " + label, (int)i + 1, E.term_choice == "external:" + E.term_ext[i].name});
        }
    }
    for (size_t i = 0; i < E.pick_items.size(); i++) if (E.pick_items[i].current) E.pick_sel = (int)i;
    E.mode = M_PICK;
    redraw();
}

void settings_save();

void pick_choose(int idx) {
    std::vector<int> view = pick_view();
    if (idx < 0 || idx >= (int)view.size()) return;
    Ed::PItem it = E.pick_items[(size_t)view[(size_t)idx]];
    E.mode = M_EDIT;
    if (E.pick_kind == PICK_LANG) {
        Doc &d = D();
        if (it.id < 0) d.set_language(nullptr, false); else d.set_language(lang_at(it.id), true);
        E.rows.clear();
    } else if (E.pick_kind == PICK_THEME) {
        if (it.id == -2) {
            if (E.fleet.found || (fleetwm_read(&E.fleet), E.fleet.found)) { E.follow_fleet = true; follow_fleetwm(); }
            else E.status_msg = "fleetwm theme file not found";
        } else { E.follow_fleet = false; apply_theme(&UI_THEMES[it.id]); }
        settings_save();
    } else if (E.pick_kind == PICK_TERMINAL) {
        E.term_choice = it.id == 0 ? "builtin" : "external:" + E.term_ext[(size_t)it.id - 1].name;
        E.status_msg = it.id == 0 ? "terminal: built-in panel" : "terminal: " + E.term_ext[(size_t)it.id - 1].name + " (opens in its own window)";
        settings_save();
    }
    redraw();
}

void open_side(bool open) {
    E.side_open = open;
    E.side_close_at = 0;
    redraw();
}

void show_view(View v) {
    E.view = v;
    if (v == V_GIT) git_refresh();
    open_side(true);
    settings_save();
}

/* ---- file browser dialog ---- */

void fd_load(const std::string &dir0) {
    char real[4096];
    std::string dir = realpath(dir0.c_str(), real) ? real : dir0;
    E.fd.dir = dir;
    E.fd.all.clear();
    E.fd.sel = E.fd.scroll = 0;
    E.fd.picked = false;
    if (dir != "/") E.fd.all.push_back({"..", true});
    std::vector<Ed::FdEntry> v;
    if (DIR *dp = opendir(dir.c_str())) {
        while (dirent *e = readdir(dp)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (!E.fd.hidden && e->d_name[0] == '.') continue;
            std::string full = dir + (dir == "/" ? "" : "/") + e->d_name;
            struct stat st;
            bool isdir = e->d_type == DT_DIR;
            if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) isdir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            if (E.fd.kind == FD_OPEN_FOLDER && !isdir) continue;
            v.push_back({e->d_name, isdir});
        }
        closedir(dp);
    }
    std::sort(v.begin(), v.end(), [](const Ed::FdEntry &a, const Ed::FdEntry &b) { return a.dir != b.dir ? a.dir : lower_ascii(a.name) < lower_ascii(b.name); });
    E.fd.all.insert(E.fd.all.end(), v.begin(), v.end());
}

std::vector<int> fd_view() {   /* indices into E.fd.all that match the filter, best first */
    std::vector<int> out;
    bool path_mode = !E.fd.filter.empty() && (E.fd.filter[0] == '/' || E.fd.filter[0] == '~');
    if (E.fd.filter.empty() || path_mode) { for (size_t i = 0; i < E.fd.all.size(); i++) out.push_back((int)i); return out; }
    FuzzyQuery fq;
    fq.parse(E.fd.filter);
    std::vector<std::pair<int, int>> sc;
    for (size_t i = 0; i < E.fd.all.size(); i++) {
        if (E.fd.all[i].name == "..") continue;
        int s2;
        if (fq.match(E.fd.all[i].name.data(), E.fd.all[i].name.size(), &s2, nullptr)) sc.push_back({-s2, (int)i});
    }
    std::sort(sc.begin(), sc.end());
    for (auto &p : sc) out.push_back(p.second);
    return out;
}

void open_filedlg(FdKind k) {
    E.fd.kind = k;
    E.fd.filter.clear();
    E.fd.msg.clear();
    E.fd.overwrite_armed = false;
    Doc &d = D();
    std::string start = E.tree.root();
    if (!d.path.empty()) start = d.path.substr(0, d.path.rfind('/') == std::string::npos ? 0 : d.path.rfind('/'));
    if (start.empty()) { const char *h = getenv("HOME"); start = h ? h : "/"; }
    E.fd.name = k == FD_SAVE ? (d.path.empty() ? "untitled.txt" : d.name()) : "";
    E.fd.name_focus = k == FD_SAVE;
    E.mode = M_FILEDLG;
    std::string pick_name;
    if (k == FD_OPEN_FOLDER && E.tree.root().size() > 1) {   /* start beside the current workspace, with it highlighted: the usual reason is to switch to a sibling */
        size_t sl = E.tree.root().rfind('/');
        start = sl == 0 ? "/" : E.tree.root().substr(0, sl);
        pick_name = E.tree.root().substr(sl + 1);
    }
    fd_load(start);
    if (!pick_name.empty()) for (size_t i = 0; i < E.fd.all.size(); i++) if (E.fd.all[i].name == pick_name) { E.fd.sel = (int)i; E.fd.picked = true; }
    redraw();
}

void fd_cancel() { E.mode = M_EDIT; E.pending = P_NONE; E.status_msg.clear(); redraw(); }

void fd_go(const std::string &dir) { E.fd.filter.clear(); E.fd.msg.clear(); fd_load(dir); redraw(); }

void fd_accept();

/* Enter / double click on a row: folders open in the dialog, files are opened (or, when saving, their name is taken) */
void fd_activate(int idx) {
    std::vector<int> view = fd_view();
    if (idx < 0 || idx >= (int)view.size()) return;
    const Ed::FdEntry &en = E.fd.all[(size_t)view[(size_t)idx]];
    std::string full = en.name == ".." ? E.fd.dir.substr(0, std::max<size_t>(1, E.fd.dir.rfind('/'))) : E.fd.dir + (E.fd.dir == "/" ? "" : "/") + en.name;
    if (en.dir) { fd_go(full); return; }
    if (E.fd.kind == FD_SAVE) { E.fd.name = en.name; E.fd.name_focus = true; E.fd.overwrite_armed = false; redraw(); return; }
    E.mode = M_EDIT;
    if (!E.pinned) open_side(false);
    open_path(full);
}

/* the dialog's main button: open the folder, open the selected file, or save under the typed name */
void fd_accept() {
    bool path_mode = !E.fd.filter.empty() && (E.fd.filter[0] == '/' || E.fd.filter[0] == '~');
    if (path_mode) {   /* a typed path: go there, or take the file */
        std::string p = expand_path(E.fd.filter);
        struct stat st;
        if (stat(p.c_str(), &st) != 0) { E.fd.msg = "not found: " + p; redraw(); return; }
        if (S_ISDIR(st.st_mode)) {
            if (E.fd.kind == FD_OPEN_FOLDER) { E.mode = M_EDIT; set_workspace(p); redraw(); return; }
            fd_go(p);
            return;
        }
        if (E.fd.kind == FD_OPEN_FILE) { E.mode = M_EDIT; if (!E.pinned) open_side(false); open_path(p); return; }
        if (E.fd.kind == FD_SAVE) { E.fd.name = p.substr(p.rfind('/') + 1); fd_go(p.substr(0, p.rfind('/'))); return; }
    }
    std::vector<int> view = fd_view();
    if (E.fd.kind == FD_OPEN_FOLDER) {
        std::string target = E.fd.dir;
        if (E.fd.picked && E.fd.sel >= 0 && E.fd.sel < (int)view.size()) { const Ed::FdEntry &en = E.fd.all[(size_t)view[(size_t)E.fd.sel]]; if (en.name != ".." && en.dir) target = E.fd.dir + (E.fd.dir == "/" ? "" : "/") + en.name; }
        E.mode = M_EDIT;
        set_workspace(target);
        redraw();
        return;
    }
    if (E.fd.kind == FD_OPEN_FILE) { if (E.fd.sel >= 0 && E.fd.sel < (int)view.size()) fd_activate(E.fd.sel); return; }
    /* save */
    if (E.fd.name.empty() || E.fd.name.find('/') != std::string::npos) { E.fd.msg = "type a file name (no slash)"; redraw(); return; }
    std::string target = E.fd.dir + (E.fd.dir == "/" ? "" : "/") + E.fd.name;
    struct stat st;
    if (stat(target.c_str(), &st) == 0 && target != D().path && !E.fd.overwrite_armed) { E.fd.overwrite_armed = true; E.fd.msg = E.fd.name + " already exists: press Save again to replace it"; redraw(); return; }
    E.prompt_kind = PK_SAVEAS;
    E.prompt_q = target;
    finish_prompt();
    if (E.mode == M_FILEDLG && E.status_msg.compare(0, 11, "SAVE FAILED") == 0) { E.fd.msg = E.status_msg; }
    else if (E.mode != M_FILEDLG) {}
}

void fd_key(const SDL_KeyboardEvent &k) {
    SDL_Keycode sym = k.keysym.sym;
    bool ctrl = k.keysym.mod & KMOD_CTRL;
    std::vector<int> view = fd_view();
    int n = (int)view.size();
    bool save_name = E.fd.kind == FD_SAVE && E.fd.name_focus;
    bool path_mode = !E.fd.filter.empty() && (E.fd.filter[0] == '/' || E.fd.filter[0] == '~');
    if (sym != SDLK_RETURN && sym != SDLK_KP_ENTER) E.fd.overwrite_armed = false;
    E.fd.msg.clear();
    if (sym == SDLK_ESCAPE) { fd_cancel(); return; }
    if (sym == SDLK_h && ctrl) { E.fd.hidden = !E.fd.hidden; fd_load(E.fd.dir); return; }
    if (sym == SDLK_UP) { E.fd.sel = std::max(0, E.fd.sel - 1); E.fd.picked = true; return; }
    if (sym == SDLK_DOWN) { E.fd.sel = std::min(std::max(0, n - 1), E.fd.sel + 1); E.fd.picked = true; return; }
    if (sym == SDLK_PAGEUP) { E.fd.sel = std::max(0, E.fd.sel - 10); E.fd.picked = true; return; }
    if (sym == SDLK_PAGEDOWN) { E.fd.sel = std::min(std::max(0, n - 1), E.fd.sel + 10); E.fd.picked = true; return; }
    if (sym == SDLK_TAB && E.fd.kind == FD_SAVE) { E.fd.name_focus = !E.fd.name_focus; return; }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
        if (ctrl || save_name || path_mode) { fd_accept(); return; }
        if (E.fd.kind == FD_OPEN_FOLDER && !E.fd.picked) { fd_accept(); return; }   /* nothing chosen: take the folder we are in */
        if (!E.fd.picked && n > 1 && E.fd.all[0].name == "..") E.fd.sel = 1;          /* files: Enter without choosing takes the first entry */
        fd_activate(E.fd.sel);
        return;
    }
    if (sym == SDLK_BACKSPACE) {
        E.fd.overwrite_armed = false;
        if (save_name) pop_utf8(E.fd.name);
        else if (!E.fd.filter.empty()) { pop_utf8(E.fd.filter); E.fd.sel = 0; E.fd.picked = false; }
        else if (E.fd.dir != "/") fd_go(E.fd.dir.substr(0, std::max<size_t>(1, E.fd.dir.rfind('/'))));   /* an empty filter: Backspace goes up a level */
        return;
    }
    if (sym == SDLK_v && ctrl) {
        char *c = SDL_GetClipboardText();
        if (c) { (save_name ? E.fd.name : E.fd.filter) += c; SDL_free(c); }
        return;
    }
}

void open_hit(int row_idx) {
    if (row_idx < 0 || (size_t)row_idx >= E.srows.size()) return;
    Ed::SRow row = E.srows[(size_t)row_idx];
    std::string path = E.search.files()[(size_t)row.file];
    SearchHit h;
    bool have = row.hit >= 0;
    if (have) h = E.sres[(size_t)row.hit];
    E.mode = M_EDIT;
    if (!E.pinned) open_side(false);
    open_path(path);
    if (have) {
        Doc &d = D();
        size_t line = h.line > 0 ? h.line - 1 : 0;
        if (line < d.buf.lines()) {
            size_t base = d.buf.line_start(line) + h.off;
            d.break_group();
            d.anchor = base + h.ms;
            d.move_to(base + h.me, true, false);
        }
        E.follow = true;
    }
    touch();
}

void toggle_search_option(int act) {
    if (E.mode == M_FIND) {
        if (act == A_TOG_CASE) E.find_case = !E.find_case; else if (act == A_TOG_REGEX) E.find_regex = !E.find_regex;
        if (!E.find_q.empty()) find_from(D().sel_lo());
        return;
    }
    if (act == A_TOG_CASE) E.sq_case = !E.sq_case; else if (act == A_TOG_REGEX) E.sq_regex = !E.sq_regex; else if (act == A_TOG_WORD) E.sq_word = !E.sq_word;
    if (E.view == V_SEARCH) start_search();
}

void search_step(int dir) {   /* Up/Down in the results list: next or previous match row */
    if (E.srows.empty()) return;
    int i = E.sel_hit;
    for (int n = 0; n < (int)E.srows.size(); n++) {
        i += dir;
        if (i < 0 || i >= (int)E.srows.size()) { i = i < 0 ? (int)E.srows.size() : -1; continue; }
        if (E.srows[(size_t)i].hit >= 0) break;
    }
    if (i < 0 || i >= (int)E.srows.size() || E.srows[(size_t)i].hit < 0) return;
    E.sel_hit = i;
    float rh = (float)S(24), topy = (float)i * rh, vh = (float)(E.H - S(190));
    if (topy < E.side_scroll) E.side_scroll = topy;
    else if (topy + rh > E.side_scroll + vh) E.side_scroll = topy + rh - vh;
}

void do_action(int act) {
    Doc &d = D();
    if (act > A_NONE && act < A_COUNT) begin_op(ACT_LABEL[act]);
    switch (act) {
    case A_NEW:
        if (splash()) { E.blank_tab = true; set_active(0); }   /* the start screen turns into an empty tab */
        else { new_doc(); set_active((int)E.docs.size() - 1); }
        break;
    case A_OPEN_FILE: begin_prompt(PK_OPEN_FILE); break;
    case A_OPEN_FOLDER: begin_prompt(PK_OPEN_FOLDER); break;
    case A_SAVE: save_active(); break;
    case A_SAVEAS: begin_prompt(PK_SAVEAS); break;
    case A_CLOSE: close_tab(E.active); break;
    case A_QUIT: request_quit(); break;
    case A_UNDO: d.undo(); break;
    case A_REDO: d.redo(); break;
    case A_CUT: { std::string s = d.selection(); if (!s.empty()) { SDL_SetClipboardText(s.c_str()); d.erase_back(); } break; }
    case A_COPY: { std::string s = d.selection(); if (!s.empty()) SDL_SetClipboardText(s.c_str()); break; }
    case A_PASTE: { char *c = SDL_GetClipboardText(); if (c && *c) { d.break_group(); d.insert_text(c, strlen(c)); d.break_group(); } if (c) SDL_free(c); break; }
    case A_SELALL: d.break_group(); d.select_all(); break;
    case A_FIND: E.mode = M_FIND; E.find_msg.clear(); break;
    case A_QUICK: E.mode = M_QUICK; E.quick_q.clear(); E.quick_sel = 0; quick_update(); break;
    case A_TOGGLE_SIDEBAR: open_side(!E.side_open); break;
    case A_PIN:
        E.pinned = !E.pinned;
        E.side_open = true;
        E.side_close_at = E.pinned ? 0 : E.now + 0.6;   /* unpinned: it floats, and leaves when the pointer is elsewhere */
        settings_save();
        break;
    case A_VIEW_EXPLORER: show_view(V_EXPLORER); break;
    case A_VIEW_GIT: show_view(V_GIT); break;
    case A_LANG: open_picker(PICK_LANG); break;
    case A_THEME: open_picker(PICK_THEME); break;
    case A_SEARCH: show_view(V_SEARCH); E.mode = M_SIDE; if (E.s_active && E.srows.empty()) start_search(); break;
    case A_TOG_REGEX: case A_TOG_CASE: case A_TOG_WORD: toggle_search_option(act); break;
    case A_TERM_TOGGLE:
        if (E.term_choice != "builtin") { launch_external(E.term_choice.substr(E.term_choice.find(':') + 1)); break; }
        if (E.term_open && E.term_focus) { E.term_open = false; E.term_focus = false; }
        else if (E.term_open) E.term_focus = true;
        else { E.term_open = true; if (E.terms.empty() && !new_terminal()) E.term_open = false; else E.term_focus = true; }
        break;
    case A_TERM_NEW:
        if (E.term_choice != "builtin") launch_external(E.term_choice.substr(E.term_choice.find(':') + 1)); else new_terminal();
        break;
    case A_TERM_KILL: kill_terminal(E.term_active); break;
    case A_TERM_PICK: open_picker(PICK_TERMINAL); break;
    case A_TERM_EXTERNAL: launch_external(E.term_choice.compare(0, 9, "external:") == 0 ? E.term_choice.substr(9) : std::string()); break;
    case A_STATS: E.show_stats = !E.show_stats; break;
    case A_NEXT_TAB: set_active((E.active + 1) % (int)E.docs.size()); break;
    case A_PREV_TAB: set_active((E.active + (int)E.docs.size() - 1) % (int)E.docs.size()); break;
    case A_ABOUT: E.status_msg = "Hootedit 0.4.0 - BeanGreen247 - fast native editor"; break;
    case A_REFRESH: E.tree.refresh(); git_refresh(); break;
    default: return;
    }
    if ((act == A_TERM_TOGGLE || act == A_TERM_NEW) && E.term_focus && E.mode != M_EDIT && E.mode != M_CONFIRM && E.mode != M_PROMPT) E.mode = M_EDIT;   /* a focused terminal owns the keyboard */
    E.follow = true;
    touch();
}

int menu_next(int menu, int sel, int dir) {
    const std::vector<MenuItem> &it = MENUS[(size_t)menu].items;
    int n = (int)it.size();
    for (int k = 1; k <= n; k++) {
        int j = ((sel + dir * k) % n + n) % n;
        if (it[(size_t)j].label[0] && it[(size_t)j].act != A_NONE) return j;
    }
    return sel;
}

void open_menu(int m) {
    begin_op(std::string("menu ") + MENUS[(size_t)m].name);
    E.menu_open = m;
    E.mode = M_MENU;
    E.menu_sel = -1;
    E.menu_sel = menu_next(m, -1, 1);
    redraw();
}

void key_down(const SDL_KeyboardEvent &k) {
    bool ctrl = k.keysym.mod & KMOD_CTRL, shift = k.keysym.mod & KMOD_SHIFT;
    SDL_Keycode sym = k.keysym.sym;
    Doc &d = D();
    Layout L = layout();

    if ((E.mode == M_SIDE || E.mode == M_FIND || E.mode == M_QUICK || E.mode == M_PICK) && ctrl && sym == SDLK_BACKQUOTE) {   /* the terminal shortcut works from any input box */
        E.mode = M_EDIT;
        do_action(shift ? A_TERM_NEW : A_TERM_TOGGLE);
        touch();
        return;
    }
    if (E.mode == M_CONFIRM) {
        if (sym == SDLK_s || sym == SDLK_y || sym == SDLK_RETURN) {
            Doc &c = *E.docs[(size_t)E.confirm_idx];
            if (c.path.empty()) { E.active = E.confirm_idx; begin_prompt(PK_SAVEAS); }
            else if (c.save(nullptr)) after_save();
            else { E.status_msg = "SAVE FAILED, buffer kept"; E.mode = M_EDIT; E.pending = P_NONE; }
        } else if (sym == SDLK_d || sym == SDLK_n) {
            Pending p = E.pending;
            close_confirmed();
            if (p == P_QUIT) request_quit();
        } else if (sym == SDLK_ESCAPE || sym == SDLK_c) { E.mode = M_EDIT; E.pending = P_NONE; }
        else return;
        touch();
        return;
    }
    if (E.mode == M_FILEDLG) { fd_key(k); touch(); return; }
    if (E.mode == M_PROMPT) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) finish_prompt();
        else if (sym == SDLK_ESCAPE) { E.mode = M_EDIT; E.pending = P_NONE; E.status_msg.clear(); }
        else if (sym == SDLK_BACKSPACE) pop_utf8(E.prompt_q);
        else if (sym == SDLK_v && ctrl) { char *c = SDL_GetClipboardText(); if (c) { E.prompt_q += c; SDL_free(c); } }
        else return;
        touch();
        return;
    }
    if (E.mode == M_MENU) {
        int n = (int)MENUS.size();
        if (sym == SDLK_ESCAPE) { E.mode = M_EDIT; E.menu_open = -1; }
        else if (sym == SDLK_LEFT) open_menu((E.menu_open + n - 1) % n);
        else if (sym == SDLK_RIGHT) open_menu((E.menu_open + 1) % n);
        else if (sym == SDLK_UP) E.menu_sel = menu_next(E.menu_open, E.menu_sel, -1);
        else if (sym == SDLK_DOWN) E.menu_sel = menu_next(E.menu_open, E.menu_sel, 1);
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            int act = E.menu_sel >= 0 ? MENUS[(size_t)E.menu_open].items[(size_t)E.menu_sel].act : A_NONE;
            E.mode = M_EDIT; E.menu_open = -1;
            do_action(act);
        } else return;
        touch();
        return;
    }
    if (E.mode == M_PICK) {
        int n = (int)pick_view().size();
        if (sym == SDLK_ESCAPE) E.mode = M_EDIT;
        else if (sym == SDLK_UP) E.pick_sel = std::max(0, E.pick_sel - 1);
        else if (sym == SDLK_DOWN) E.pick_sel = std::min(n - 1, E.pick_sel + 1);
        else if (sym == SDLK_PAGEUP) E.pick_sel = std::max(0, E.pick_sel - 10);
        else if (sym == SDLK_PAGEDOWN) E.pick_sel = std::min(n - 1, E.pick_sel + 10);
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) pick_choose(E.pick_sel);
        else if (sym == SDLK_BACKSPACE) { pop_utf8(E.pick_filter); E.pick_sel = 0; }
        else return;
        touch();
        return;
    }
    if (E.mode == M_QUICK) {
        int n = (int)E.quick_hits.size();
        if (sym == SDLK_ESCAPE) E.mode = M_EDIT;
        else if (sym == SDLK_UP) E.quick_sel = std::max(0, E.quick_sel - 1);
        else if (sym == SDLK_DOWN) E.quick_sel = std::min(n - 1, E.quick_sel + 1);
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { E.mode = M_EDIT; if (n) open_path(E.quick_hits[(size_t)E.quick_sel]); }
        else if (sym == SDLK_BACKSPACE) { pop_utf8(E.quick_q); E.quick_sel = 0; quick_update(); }
        else return;
        touch();
        return;
    }
    if (E.mode == M_SIDE) {
        bool alt = k.keysym.mod & KMOD_ALT;
        bool searching = E.view == V_SEARCH;
        std::string &qs = searching ? E.sq : E.side_q;
        auto changed = [&]() { if (searching) start_search(); else { E.side_scroll = 0; side_filter_update(); } };
        if (sym == SDLK_ESCAPE) { E.mode = M_EDIT; if (!searching) E.side_q.clear(); }
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (searching) { int r = E.sel_hit; if (r < 0) for (size_t i = 0; i < E.srows.size(); i++) if (E.srows[i].hit >= 0) { r = (int)i; break; } open_hit(r); }
            else if (!E.side_hits.empty()) { std::string p = E.side_hits[0]; E.mode = M_EDIT; E.side_q.clear(); if (!E.pinned) open_side(false); open_path(p); }
        }
        else if (sym == SDLK_DOWN && searching) search_step(1);
        else if (sym == SDLK_UP && searching) search_step(-1);
        else if (sym == SDLK_BACKSPACE) { pop_utf8(qs); changed(); }
        else if (sym == SDLK_v && ctrl) { char *c = SDL_GetClipboardText(); if (c) { qs += c; SDL_free(c); changed(); } }
        else if (alt && searching && sym == SDLK_r) toggle_search_option(A_TOG_REGEX);
        else if (alt && searching && sym == SDLK_c) toggle_search_option(A_TOG_CASE);
        else if (alt && searching && sym == SDLK_w) toggle_search_option(A_TOG_WORD);
        else return;
        touch();
        return;
    }
    if (E.mode == M_FIND) {
        if (sym == SDLK_ESCAPE) E.mode = M_EDIT;
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) find_from(d.sel_hi());
        else if (sym == SDLK_BACKSPACE) { pop_utf8(E.find_q); find_from(d.sel_lo()); }
        else if (sym == SDLK_f && ctrl) { E.find_q.clear(); E.find_msg.clear(); }
        else if (sym == SDLK_v && ctrl) { char *c = SDL_GetClipboardText(); if (c) { E.find_q += c; SDL_free(c); find_from(d.sel_lo()); } }
        else if (sym == SDLK_w && ctrl) { E.mode = M_EDIT; close_tab(E.active); }
        else if (sym == SDLK_n && ctrl) { E.mode = M_EDIT; do_action(A_NEW); }
        else if (sym == SDLK_p && ctrl) do_action(A_QUICK);
        else if ((k.keysym.mod & KMOD_ALT) && sym == SDLK_r) toggle_search_option(A_TOG_REGEX);
        else if ((k.keysym.mod & KMOD_ALT) && sym == SDLK_c) toggle_search_option(A_TOG_CASE);
        else return;
        touch();
        return;
    }

    if (E.term_open && E.term_focus && cur_term() && cur_term()->core) {
        bool alt = k.keysym.mod & KMOD_ALT;
        TermCore *core = cur_term()->core;
        if (ctrl && sym == SDLK_BACKQUOTE) { do_action(shift ? A_TERM_NEW : A_TERM_TOGGLE); touch(); return; }
        if (ctrl && shift && sym == SDLK_c) { tcore_copy(core); touch(); return; }
        if (ctrl && shift && sym == SDLK_v) { tcore_paste_request(core); touch(); return; }
        if (ctrl && !shift && !alt && (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN) && E.terms.size() > 1) {
            int n = (int)E.terms.size();
            E.term_active = (E.term_active + (sym == SDLK_PAGEDOWN ? 1 : n - 1)) % n;
            touch();
            return;
        }
        TKey tk = TK_NONE;
        switch (sym) {
        case SDLK_UP: case SDLK_KP_8: tk = TK_UP; break; case SDLK_DOWN: case SDLK_KP_2: tk = TK_DOWN; break;
        case SDLK_LEFT: case SDLK_KP_4: tk = TK_LEFT; break; case SDLK_RIGHT: case SDLK_KP_6: tk = TK_RIGHT; break;
        case SDLK_HOME: tk = TK_HOME; break; case SDLK_END: tk = TK_END; break; case SDLK_INSERT: tk = TK_INSERT; break;
        case SDLK_DELETE: tk = TK_DELETE; break; case SDLK_PAGEUP: tk = TK_PGUP; break; case SDLK_PAGEDOWN: tk = TK_PGDN; break;
        case SDLK_F1: tk = TK_F1; break; case SDLK_F2: tk = TK_F2; break; case SDLK_F3: tk = TK_F3; break; case SDLK_F4: tk = TK_F4; break;
        case SDLK_F5: tk = TK_F5; break; case SDLK_F6: tk = TK_F6; break; case SDLK_F7: tk = TK_F7; break; case SDLK_F8: tk = TK_F8; break;
        case SDLK_F9: tk = TK_F9; break; case SDLK_F10: tk = TK_F10; break; case SDLK_F11: tk = TK_F11; break; case SDLK_F12: tk = TK_F12; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: tk = TK_ENTER; break; case SDLK_BACKSPACE: tk = TK_BACKSPACE; break;
        case SDLK_TAB: tk = TK_TAB; break; case SDLK_ESCAPE: tk = TK_ESCAPE; break;
        default: break;
        }
        int mods = (shift ? TM_SHIFT : 0) | (alt ? TM_ALT : 0) | (ctrl ? TM_CTRL : 0) | ((k.keysym.mod & KMOD_GUI) ? TM_META : 0);
        uint32_t cp = (sym >= 32 && sym < 127) ? (uint32_t)sym : 0;
        if (tk == TK_ENTER) tcore_cmd_begin(core, now_s());
        begin_op("terminal key");
        if (tcore_key(core, tk, cp, mods)) touch();
        return;
    }
    E.status_msg.clear();
    if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE || sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_UP || sym == SDLK_DOWN) begin_op(sym == SDLK_BACKSPACE || sym == SDLK_DELETE || sym == SDLK_RETURN || sym == SDLK_KP_ENTER ? "keystroke" : "cursor move");
    if (E.show_stats && sym == SDLK_ESCAPE) { E.show_stats = false; touch(); return; }
    if (E.side_open && !E.pinned && sym == SDLK_ESCAPE) { open_side(false); touch(); return; }
    switch (sym) {
    case SDLK_LEFT: { d.break_group(); size_t p = d.has_sel() && !shift ? d.sel_lo() : d.prev_char(d.cur); d.move_to(p, shift, false); break; }
    case SDLK_RIGHT: { d.break_group(); size_t p = d.has_sel() && !shift ? d.sel_hi() : d.next_char(d.cur); d.move_to(p, shift, false); break; }
    case SDLK_UP: d.break_group(); move_line(-1, shift); break;
    case SDLK_DOWN: d.break_group(); move_line(1, shift); break;
    case SDLK_PAGEUP:
        if (ctrl) { do_action(A_PREV_TAB); return; }
        d.break_group(); move_line(-std::max(1, L.rows - 1), shift); break;
    case SDLK_PAGEDOWN:
        if (ctrl) { do_action(A_NEXT_TAB); return; }
        d.break_group(); move_line(std::max(1, L.rows - 1), shift); break;
    case SDLK_HOME: { d.break_group(); size_t l = d.buf.line_of(d.cur); d.move_to(ctrl ? 0 : d.buf.line_start(l), shift, false); break; }
    case SDLK_END: { d.break_group(); size_t l = d.buf.line_of(d.cur); d.move_to(ctrl ? d.buf.size() : d.buf.line_start(l) + d.buf.line_len(l), shift, false); break; }
    case SDLK_BACKSPACE: d.erase_back(); break;
    case SDLK_DELETE: d.erase_fwd(); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: d.insert_text("\n", 1); break;
    case SDLK_TAB:
        if (ctrl) { do_action(shift ? A_PREV_TAB : A_NEXT_TAB); return; }
        d.insert_text("\t", 1); break;
    case SDLK_a: if (ctrl) do_action(A_SELALL); else return; return;
    case SDLK_c: if (ctrl) do_action(A_COPY); else return; return;
    case SDLK_x: if (ctrl) do_action(A_CUT); else return; return;
    case SDLK_v: if (ctrl) do_action(A_PASTE); else return; return;
    case SDLK_z: if (ctrl) do_action(shift ? A_REDO : A_UNDO); else return; return;
    case SDLK_y: if (ctrl) do_action(A_REDO); else return; return;
    case SDLK_s: if (ctrl) do_action(shift ? A_SAVEAS : A_SAVE); else return; return;
    case SDLK_n: if (ctrl) do_action(A_NEW); else return; return;
    case SDLK_o: if (ctrl) do_action(A_OPEN_FILE); else return; return;
    case SDLK_w: if (ctrl) do_action(A_CLOSE); else return; return;
    case SDLK_q: if (ctrl) do_action(A_QUIT); else return; return;
    case SDLK_f: if (ctrl) do_action(shift ? A_SEARCH : A_FIND); else return; return;
    case SDLK_p: if (ctrl) do_action(A_QUICK); else return; return;
    case SDLK_b: if (ctrl) do_action(A_TOGGLE_SIDEBAR); else return; return;
    case SDLK_e: if (ctrl && shift) do_action(A_VIEW_EXPLORER); else return; return;
    case SDLK_g: if (ctrl && shift) do_action(A_VIEW_GIT); else return; return;
    case SDLK_l: if (ctrl && shift) do_action(A_LANG); else return; return;
    case SDLK_t: if (ctrl && shift) do_action(A_THEME); else return; return;
    case SDLK_BACKQUOTE: if (ctrl) do_action(shift ? A_TERM_NEW : A_TERM_TOGGLE); else return; return;
    case SDLK_i: if (ctrl && shift) do_action(A_STATS); else return; return;
    default: return;
    }
    E.follow = true;
    touch();
}

void click_hit(const Hit &h) {
    bool term_hit = h.kind == H_TERMTAB || h.kind == H_SPLIT || (h.kind == H_ACT && (h.id == A_TERM_NEW || h.id == A_TERM_KILL || h.id == A_TERM_TOGGLE));
    E.term_focus = term_hit && E.term_open;
    if (term_hit && h.kind == H_ACT) E.term_focus = true;
    switch (h.kind) {
    case H_TAB: set_active(h.id); E.mode = M_EDIT; break;
    case H_TABCLOSE: E.mode = M_EDIT; close_tab(h.id); break;
    case H_BOX: do_action(E.mode == M_FIND ? A_NONE : A_FIND); break;
    case H_ACT: if (h.id != A_TOG_CASE && h.id != A_TOG_REGEX && h.id != A_TOG_WORD) E.mode = M_EDIT; do_action(h.id); break;
    case H_MENU: if (E.mode == M_MENU && E.menu_open == h.id) { E.mode = M_EDIT; E.menu_open = -1; } else open_menu(h.id); break;
    case H_MENUITEM: { int act = MENUS[(size_t)E.menu_open].items[(size_t)h.id].act; E.mode = M_EDIT; E.menu_open = -1; do_action(act); break; }
    case H_PICKITEM: pick_choose(h.id); break;
    case H_QUICKITEM: E.mode = M_EDIT; if (h.id < (int)E.quick_hits.size()) open_path(E.quick_hits[(size_t)h.id]); break;
    case H_BTN_SAVE: { SDL_KeyboardEvent k = {}; k.keysym.sym = SDLK_s; key_down(k); break; }
    case H_BTN_DISCARD: { SDL_KeyboardEvent k = {}; k.keysym.sym = SDLK_d; key_down(k); break; }
    case H_BTN_CANCEL: { SDL_KeyboardEvent k = {}; k.keysym.sym = SDLK_ESCAPE; key_down(k); break; }
    case H_ROOT: E.root_open = !E.root_open; break;
    case H_TREE: {
        const std::vector<Tree::Row> &rows = E.tree.rows();
        if ((size_t)h.id >= rows.size()) break;
        if (rows[(size_t)h.id].dir) E.tree.toggle((size_t)h.id);
        else { std::string p = rows[(size_t)h.id].path; if (!E.pinned) open_side(false); open_path(p); }
        break;
    }
    case H_ACTIVITY: show_view((View)h.id); break;
    case H_PIN: do_action(A_PIN); break;
    case H_GITROW:
        if ((size_t)h.id < E.git.changes.size()) {
            GitChange c = E.git.changes[(size_t)h.id];
            if (!E.pinned) open_side(false);
            if (SDL_GetModState() & KMOD_CTRL) open_path(E.git.root + "/" + c.path); else open_diff(c);
        }
        break;
    case H_SIDEBOX: E.mode = M_SIDE; if (E.view == V_GIT) E.view = V_EXPLORER; if (E.view == V_EXPLORER && !E.side_q.empty()) side_filter_update(); break;
    case H_SRES: open_hit(h.id); break;
    case H_SIDEHIT:
        if ((size_t)h.id < E.side_hits.size()) { std::string p = E.side_hits[(size_t)h.id]; E.mode = M_EDIT; E.side_q.clear(); if (!E.pinned) open_side(false); open_path(p); }
        break;
    case H_TERMTAB: if (h.id >= 0 && h.id < (int)E.terms.size()) { E.term_active = h.id; E.term_focus = true; } break;
    case H_SPLIT: E.split_drag = true; break;
    case H_TABSPACE: if (g_clicks >= 2) do_action(A_NEW); break;
    case H_VSCROLL: E.sb_drag = true; sb_to_pointer(); break;
    case H_FD_ITEM: {
        std::vector<int> view = fd_view();
        E.fd.sel = h.id;
        E.fd.picked = true;
        if (E.fd.kind == FD_SAVE && h.id < (int)view.size() && !E.fd.all[(size_t)view[(size_t)h.id]].dir) { E.fd.name = E.fd.all[(size_t)view[(size_t)h.id]].name; E.fd.overwrite_armed = false; }
        if (g_clicks >= 2) fd_activate(h.id);
        break;
    }
    case H_FD_OK: fd_accept(); break;
    case H_FD_CANCEL: fd_cancel(); break;
    case H_FD_HIDDEN: E.fd.hidden = !E.fd.hidden; fd_load(E.fd.dir); break;
    case H_FD_PLACE: {
        const char *home = getenv("HOME");
        if (h.id == 0) fd_go(home ? home : "/");
        else if (h.id == 1) fd_go(E.tree.root().empty() ? "/" : E.tree.root());
        else if (h.id == 2) fd_go("/");
        else if (h.id == 99) E.fd.name_focus = true;
        break;
    }
    case H_SIDEBAR: break;
    default: break;
    }
    touch();
}

void text_input(const char *s) {
    if (E.mode == M_EDIT && E.term_open && E.term_focus && cur_term() && cur_term()->core) {
        if (!(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT))) { begin_op("terminal key"); tcore_text(cur_term()->core, s); touch(); }
        return;
    }
    if (E.mode == M_EDIT) begin_op("keystroke");
    else if (E.mode == M_QUICK) begin_op("quick-open filter", std::to_string(E.quick_files.size()) + " files");
    else if (E.mode == M_SIDE && E.view == V_EXPLORER) begin_op("file filter", std::to_string(E.quick_files.size()) + " files");
    if (E.mode == M_FIND) { E.find_q += s; find_from(D().sel_lo()); touch(); return; }
    if (E.mode == M_FILEDLG) {
        if (E.fd.kind == FD_SAVE && E.fd.name_focus) { E.fd.name += s; E.fd.overwrite_armed = false; }
        else {
            E.fd.filter += s;
            E.fd.sel = 0;
            E.fd.picked = false;
            if ((E.fd.filter[0] == '/' || E.fd.filter[0] == '~') && E.fd.filter.back() == '/') {   /* a typed path ending in "/" opens that folder */
                std::string p = expand_path(E.fd.filter);
                struct stat st;
                if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) fd_go(p);
            }
        }
        E.fd.msg.clear();
        touch();
        return;
    }
    if (E.mode == M_PROMPT) { E.prompt_q += s; touch(); return; }
    if (E.mode == M_PICK) { E.pick_filter += s; E.pick_sel = 0; touch(); return; }
    if (E.mode == M_QUICK) { E.quick_q += s; E.quick_sel = 0; quick_update(); touch(); return; }
    if (E.mode == M_SIDE) {
        if (E.view == V_SEARCH) { E.sq += s; start_search(); }
        else { E.side_q += s; E.side_scroll = 0; side_filter_update(); }
        touch();
        return;
    }
    if (E.mode == M_CONFIRM || E.mode == M_MENU) return;
    if (SDL_GetModState() & KMOD_CTRL) return;
    D().insert_text(s, strlen(s));
    E.status_msg.clear();
    E.follow = true;
    touch();
}

bool in_term(float x, float y) {
    return E.term_open && cur_term() && cur_term()->core && x >= E.term_rx && x < E.term_rx + E.term_rw && y >= E.term_ry && y < E.term_ry + E.term_rh;
}
void term_cell(float x, float y, int *col, int *row) {
    *col = (int)((x - E.term_rx - (float)TV_PAD) / (float)font_cell_w(E.font));
    *row = (int)((y - E.term_ry - (float)TV_PAD) / (float)font_cell_h(E.font));
}
int term_mods() {
    Uint16 m = (Uint16)SDL_GetModState();
    return ((m & KMOD_SHIFT) ? TM_SHIFT : 0) | ((m & KMOD_ALT) ? TM_ALT : 0) | ((m & KMOD_CTRL) ? TM_CTRL : 0) | ((m & KMOD_GUI) ? TM_META : 0);
}

void update_hover() {
    int kind = 0, id = -1;
    for (size_t i = E.hits.size(); i-- > 0;) {
        const Hit &h = E.hits[i];
        if (E.mx >= h.x && E.mx < h.x + h.w && E.my >= h.y && E.my < h.y + h.h) { if (h.kind == H_SIDEBAR) break; kind = h.kind; id = h.id; break; }
    }
    if (kind != E.hover_kind || id != E.hover_id) { E.hover_kind = kind; E.hover_id = id; redraw(); }
}

void handle_motion() {
    Layout L = layout();
    if (!E.pinned) {
        bool in_side = E.side_open && E.mx < (float)L.side_x1 && E.my >= (float)L.menu_h;
        if (!E.side_open && E.mx >= 0 && E.mx <= (float)S(3) && E.my >= (float)L.menu_h && E.my < (float)L.bar_y) open_side(true);
        else if (E.side_open) E.side_close_at = in_side ? 0 : (E.side_close_at ? E.side_close_at : E.now + 0.35);
    }
    update_hover();
}

void handle_event(const SDL_Event &e) {
    if (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) E.ev_t0 = now_s();
    switch (e.type) {
    case SDL_QUIT: request_quit(); redraw(); break;
    case SDL_KEYDOWN: key_down(e.key); break;
    case SDL_TEXTINPUT: text_input(e.text.text); break;
    case SDL_MOUSEWHEEL: {
        Layout L = layout();
        if (E.mode == M_EDIT && in_term(E.mx, E.my)) {
            int col, row;
            term_cell(E.mx, E.my, &col, &row);
            tcore_wheel(cur_term()->core, e.wheel.preciseY, col, row, term_mods());
            redraw();
            break;
        }
        if (E.mode == M_FILEDLG) { E.fd.scroll = std::max(0, E.fd.scroll - e.wheel.y * 3); redraw(); break; }
        if (E.mode == M_PICK) { E.pick_scroll = std::max(0, E.pick_scroll - e.wheel.y * 3); redraw(); break; }
        if (E.side_open && E.mx < (float)L.side_x1 && E.my >= (float)L.menu_h && E.my < (float)L.bar_y) {
            E.side_scroll = std::max(0.f, E.side_scroll - (float)e.wheel.y * (float)S(66));
            redraw();
            break;
        }
        Doc &d = D();
        long nt = (long)d.top - (long)e.wheel.y * 3;
        d.top = nt < 0 ? 0 : std::min((size_t)nt, d.buf.lines() - 1);
        if (e.wheel.x) d.left = std::max(0, d.left + e.wheel.x * 4);
        redraw();
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        g_clicks = e.button.clicks;
        if (E.mode == M_EDIT && in_term((float)e.button.x * E.scale, (float)e.button.y * E.scale)) {
            float mx = (float)e.button.x * E.scale, my = (float)e.button.y * E.scale;
            E.mx = mx; E.my = my;
            int col, row;
            term_cell(mx, my, &col, &row);
            int b = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
            E.term_focus = true;
            tcore_mouse_button(cur_term()->core, b, true, col, row, e.button.clicks, term_mods());
            E.term_drag = true;
            touch();
            break;
        }
        if (e.button.button == SDL_BUTTON_LEFT) {
            float mx = (float)e.button.x * E.scale, my = (float)e.button.y * E.scale;
            E.mx = mx; E.my = my;
            for (size_t i = E.hits.size(); i-- > 0;) {
                const Hit &h = E.hits[i];
                if (mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h) {
                    if (h.kind == H_SIDEBAR) return;   /* empty sidebar area: swallow the click */
                    Hit c = h; click_hit(c); return;
                }
            }
            bool was_overlay = E.mode == M_PICK || E.mode == M_FIND || E.mode == M_MENU || E.mode == M_QUICK || E.mode == M_SIDE;
            if (was_overlay) { E.mode = M_EDIT; E.menu_open = -1; }
            if (E.mode != M_EDIT) return;
            if (E.side_open && !E.pinned) open_side(false);
            E.term_focus = false;
            Layout L = layout();
            if (splash() && g_clicks >= 2 && my >= (float)L.tab_y && my < (float)L.ed_bottom && mx >= (float)L.ed_x) { do_action(A_NEW); return; }   /* double click on the start screen */
            if (my >= (float)L.text_y && my < (float)L.ed_bottom && mx >= (float)L.ed_x) {
                Doc &d = D();
                d.break_group();
                d.move_to(pos_from_xy((int)mx, (int)my), (SDL_GetModState() & KMOD_SHIFT) != 0, false);
                E.dragging = true;
                touch();
            } else redraw();
        }
        break;
    case SDL_MOUSEMOTION:
        if (g_scripted && e.motion.which != SCRIPT_MOUSE) break;   /* --script drives the pointer; the real one (xvfb parks it mid-window) is ignored */
        E.mx = (float)e.motion.x * E.scale; E.my = (float)e.motion.y * E.scale;
        if (E.sb_drag) sb_to_pointer();
        else if (E.split_drag) {
            Layout L = layout();
            int maxl = (int)((float)(L.bar_y - L.text_y - S(100)) / E.scale);
            E.panel_logical = std::max(80, std::min(maxl, (int)((float)L.bar_y - E.my) / (int)std::max(1.f, E.scale)));
            redraw();
        } else if (E.term_drag || (E.mode == M_EDIT && in_term(E.mx, E.my))) {
            int col, row;
            term_cell(E.mx, E.my, &col, &row);
            tcore_mouse_move(cur_term()->core, col, row, term_mods());
            if (E.term_drag) touch();
        } else if (E.dragging && (e.motion.state & SDL_BUTTON_LMASK)) {
            D().move_to(pos_from_xy((int)E.mx, (int)E.my), true, false);
            E.follow = true;
            touch();
        } else handle_motion();
        break;
    case SDL_MOUSEBUTTONUP:
        if (E.term_drag && cur_term() && cur_term()->core) {
            int col, row;
            term_cell((float)e.button.x * E.scale, (float)e.button.y * E.scale, &col, &row);
            int b = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
            tcore_mouse_button(cur_term()->core, b, false, col, row, 1, term_mods());
            if (e.button.button == SDL_BUTTON_LEFT) E.term_drag = false;
            redraw();
        }
        if (E.split_drag) { E.split_drag = false; settings_save(); }
        E.sb_drag = false;
        E.dragging = false;
        break;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) { SDL_GL_GetDrawableSize(E.win, &E.W, &E.H); redraw(); }
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) { E.focused = true; touch(); }
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { E.focused = false; E.blink_on = true; redraw(); }
        else if (e.window.event == SDL_WINDOWEVENT_LEAVE) { E.mx = E.my = -1; if (!E.pinned && E.side_open) E.side_close_at = E.now + 0.35; update_hover(); }
        else if (e.window.event == SDL_WINDOWEVENT_EXPOSED) redraw();
        break;
    default: break;
    }
}

/* --script "ctrl+n;text:hello;act:pin;menu:3;mouse:20,300;click:20,300": drives the editor without a keyboard, for screenshots and tests */
void run_script(const char *script) {
    std::string s = script;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(';', p);
        if (q == std::string::npos) q = s.size();
        std::string step = s.substr(p, q - p);
        p = q + 1;
        if (step.empty()) continue;
        auto frame_if_timed = [&]() {
            if (!g_timed) return;
            for (int attempt = 0; attempt < 2; attempt++) {
                uint32_t gen0 = atlas_generation(E.atlas);
                r_begin(E.r, E.W, E.H, C_EDITOR);
                double b0 = now_s();
                build_frame();
                E.last_build_ms = (now_s() - b0) * 1000.0;
                r_end(E.r);
                if (atlas_generation(E.atlas) == gen0) break;
            }
            end_op_if_pending();
            E.frames++;
        };
        if (step.compare(0, 4, "wait") == 0) {   /* wait:MS lets background work (a search) finish, then polls and draws */
            double until = now_s() + atof(step.c_str() + 5) / 1000.0;
            while (now_s() < until && (E.s_active && !E.sstat.done)) { search_poll(); SDL_Delay(1); }
            search_poll(); E.ev_t0 = 0; frame_if_timed();
            continue;
        }
        E.ev_t0 = now_s();
        if (step.compare(0, 9, "hitcheck:") == 0) {   /* prints what a click at x,y would hit (test hook) */
            int x = 0, y = 0;
            sscanf(step.c_str() + 9, "%d,%d", &x, &y);
            build_frame();
            const Hit *found = nullptr;
            for (size_t i = E.hits.size(); i-- > 0;) { const Hit &h = E.hits[i]; if ((float)x >= h.x && (float)x < h.x + h.w && (float)y >= h.y && (float)y < h.y + h.h) { found = &h; break; } }
            printf("hitcheck %d,%d: %s kind=%d id=%d\n", x, y, found ? "hit" : "none", found ? found->kind : 0, found ? found->id : -1);
            continue;
        }
        if (step == "frame") { begin_op("empty frame (nothing changed, full redraw)"); frame_if_timed(); continue; }
        if (step.compare(0, 5, "open:") == 0) { open_path(step.substr(5)); frame_if_timed(); continue; }
        if (step.compare(0, 5, "text:") == 0) { text_input(step.c_str() + 5); frame_if_timed(); continue; }
        if (step.compare(0, 5, "menu:") == 0) { open_menu(atoi(step.c_str() + 5)); frame_if_timed(); continue; }
        if (step.compare(0, 6, "mouse:") == 0 || step.compare(0, 6, "click:") == 0 || step.compare(0, 7, "dclick:") == 0) {
            int x = 0, y = 0;
            bool dbl = step[0] == 'd';
            sscanf(step.c_str() + (dbl ? 7 : 6), "%d,%d", &x, &y);
            SDL_Event ev = {};
            if (step[0] == 'm') { ev.type = SDL_MOUSEMOTION; ev.motion.which = SCRIPT_MOUSE; ev.motion.x = (int)((float)x / E.scale); ev.motion.y = (int)((float)y / E.scale); }
            else { ev.type = SDL_MOUSEBUTTONDOWN; ev.button.button = SDL_BUTTON_LEFT; ev.button.clicks = dbl ? 2 : 1; ev.button.x = (int)((float)x / E.scale); ev.button.y = (int)((float)y / E.scale); }
            handle_event(ev);
            if (step[0] != 'm') { SDL_Event up = {}; up.type = SDL_MOUSEBUTTONUP; handle_event(up); }
            build_frame();   /* refresh the hit boxes the way the real loop would before the next event */
            frame_if_timed();
            continue;
        }
        if (step.compare(0, 4, "act:") == 0) {
            bool found = false;
            for (int i = 1; i < A_COUNT; i++) if (step.compare(4, std::string::npos, ACT_NAMES[i]) == 0) { do_action(i); found = true; }
            if (!found) fprintf(stderr, "hootedit: unknown action in --script: %s\n", step.c_str() + 4);
            frame_if_timed();
            if (!g_timed) build_frame();
            continue;
        }
        SDL_KeyboardEvent k = {};
        std::string rest = step;
        for (;;) {
            size_t plus = rest.find('+');
            if (plus == std::string::npos || plus + 1 >= rest.size()) break;
            std::string m = rest.substr(0, plus);
            if (m == "ctrl") k.keysym.mod |= KMOD_LCTRL; else if (m == "shift") k.keysym.mod |= KMOD_LSHIFT; else if (m == "alt") k.keysym.mod |= KMOD_LALT;
            rest = rest.substr(plus + 1);
        }
        k.keysym.sym = SDL_GetKeyFromName(rest.c_str());
        if (k.keysym.sym == SDLK_UNKNOWN) { fprintf(stderr, "hootedit: unknown key in --script: %s\n", rest.c_str()); continue; }
        key_down(k);
        frame_if_timed();
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::vector<const char *> args;
    const char *shot = nullptr, *script = nullptr, *cli_theme = nullptr;
    int bench = 0;
    bool stats_at_exit = false;
    double exit_after = 0, shot_after = 0.4;
    int win_w = 1280, win_h = 720;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cache-stats")) stats_at_exit = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc) exit_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shot-after") && i + 1 < argc) shot_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timed")) g_timed = true;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) cli_theme = argv[++i];
        else if (!strcmp(argv[i], "--version")) { puts("hootedit 0.4.0"); return 0; }
        else if (argv[i][0] != '-') args.push_back(argv[i]);
        else { fprintf(stderr, "usage: hootedit [--cache-stats] [--size WxH] [--shot FILE] [--exit-after SEC] [--script STEPS] [--bench N] [--theme NAME] [FILE|FOLDER...]\n"); return 2; }
    }
    settings_load();
    std::string workspace;
    for (const char *a : args) {
        struct stat st;
        if (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) { if (workspace.empty()) workspace = a; continue; }
        Doc &d = new_doc(false);
        if (!d.open(a)) { fprintf(stderr, "hootedit: cannot open %s\n", a); return 1; }
        if (workspace.empty()) {
            std::string full = expand_path(a);
            GitInfo g = git_query(full.substr(0, full.rfind('/') == std::string::npos ? 0 : full.rfind('/')));
            workspace = g.repo ? g.root : full.substr(0, full.rfind('/'));
        }
    }
    if (E.docs.empty()) new_doc();
    if (workspace.empty()) { char cwd[1024]; workspace = getcwd(cwd, sizeof cwd) ? cwd : "."; }
    signal(SIGUSR1, on_usr1);
    /* the window class is what a panel uses to match a window to its menu entry and icon; a terminal that exports its own SDL class (Lestrix does) would
     * otherwise make every program started from it look like that terminal. Set ours explicitly before SDL_Init, which reads it once, then take it out of the environment again so the shells
     * and external terminals we start do not inherit it. */
    setenv("SDL_VIDEO_X11_WMCLASS", "hootedit", 1);
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "hootedit", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "hootedit: SDL: %s\n", SDL_GetError()); return 1; }
    bool ok = false;
    for (GlKind k : {GLK_CORE, GLK_ES3, GLK_ES2}) if ((ok = gl_init(k, win_w, win_h))) break;
    unsetenv("SDL_VIDEO_X11_WMCLASS");
    unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
    if (!ok) { fprintf(stderr, "hootedit: no usable OpenGL context\n"); return 1; }
    E.font = font_open("monospace", 14.0 * E.scale, true);
    E.ui = font_open("sans", 13.0 * E.scale, false);
    E.big = font_open("sans", 34.0 * E.scale, false);
    if (!E.font || !E.ui || !E.big) { fprintf(stderr, "hootedit: no font\n"); return 1; }
    E.cw = font_cell_w(E.font); E.ch = font_cell_h(E.font); E.asc = font_ascent(E.font);
    E.rowh = std::max(E.ch, (int)lround(19.0 * E.scale));
    E.rowasc = E.asc + (E.rowh - E.ch) / 2;
    E.uh = font_cell_h(E.ui); E.uasc = font_ascent(E.ui);
    g_ev_wake = SDL_RegisterEvents(1);
    tcore_set_history_defaults(10000, 64, 0, false);   /* scrollback of the terminal panel: 10,000 lines, up to 64 MB in memory, no disk spill */
    wp_configure(1);
    tcore_set_native_cat(true);                         /* the built-in fast cat of the engine: plain "cat file" and "producer | cat" at memory speed */
    if (cli_theme) { E.theme = ui_theme_find(cli_theme); E.follow_fleet = false; }
    if (!E.theme) E.theme = ui_theme_find("Xylonic Dark");
    apply_theme(E.theme);
    if (E.follow_fleet) follow_fleetwm();
    SDL_StartTextInput();
    set_workspace(workspace);
    E.side_open = true;
    set_active(0);

    E.now = now_s();
    double t0 = E.now;
    E.last_active = E.last_blink = E.now;
    build_frame();   /* one layout pass first so clicks and menus have their hit boxes */
    if (script) { g_scripted = true; run_script(script); }
    hud_tick();
    if (bench > 0) {   /* scroll one line per frame through the file: CPU time to build a frame, and the same with every row already cached */
        Doc &d = D();
        size_t nl = d.buf.lines();
        E.timing = true;
        for (int pass = 0; pass < 2; pass++) {
            E.t_top = E.t_rows = E.t_gutter = E.t_status = E.t_flush = 0;
            double sum_build = 0, sum_full = 0, worst = 0;
            for (int i = 0; i < bench; i++) {
                d.top = (size_t)i % (nl > 60 ? nl - 60 : 1);
                double c0 = now_s();
                r_begin(E.r, E.W, E.H, C_EDITOR);
                build_frame();
                double c1 = now_s();
                r_end(E.r);
                SDL_GL_SwapWindow(E.win);
                double c2 = now_s();
                sum_build += c1 - c0; sum_full += c2 - c0;
                if (c1 - c0 > worst) worst = c1 - c0;
            }
            printf("bench pass %d (%s): build_frame %.1f us avg, worst %.1f us; with GL submit+swap %.1f us (software GL)\n", pass + 1,
                   pass == 0 ? "cold caches" : "warm row cache", sum_build / bench * 1e6, worst * 1e6, sum_full / bench * 1e6);
            printf("  per frame: tab row %.1f us, text rows (hash+lookup+push, no GL) %.1f us, clip flush (GL) %.1f us, editor area incl. rows %.1f us, status bar %.1f us\n",
                   E.t_top / bench * 1e6, E.t_rows / bench * 1e6, E.t_flush / bench * 1e6, E.t_gutter / bench * 1e6, E.t_status / bench * 1e6);
        }
        printf("%s\n", cache_stats_text().c_str());
        return 0;
    }
    SDL_Event ev;
    while (E.running) {
        double due = E.next_hud ? E.next_hud : E.now;
        if (E.focused && E.now - E.last_active < BLINK_FOR) due = std::min(due, E.last_blink + 0.53);
        if (E.side_close_at > 0) due = std::min(due, E.side_close_at);
        if (E.follow_fleet && !E.fleet_watch) due = std::min(due, E.next_fleet ? E.next_fleet : E.now + 1.0);
        double left = due - E.now;
        int wait = left < 0.001 ? 1 : left > 5.0 ? 5000 : (int)(left * 1000.0) + 1;
        if (E.dirty) wait = 0;
        if (SDL_WaitEventTimeout(&ev, wait)) { do handle_event(ev); while (SDL_PollEvent(&ev)); }
        E.now = now_s();
        if (g_print_stats) { g_print_stats = 0; fprintf(stderr, "%s\n", cache_stats_text().c_str()); }
        g_wake_pending = 0;
        search_poll();
        terms_pump();
        if (E.follow_fleet && !E.fleet_watch) E.fleet_watch = fleetwm_watch_start();
        if (fleetwm_watch_take() || (E.follow_fleet && E.now >= E.next_fleet)) {
            E.next_fleet = E.now + (E.fleet_watch ? 30.0 : 1.0);   /* with the watcher the timer only catches a missed event */
            if (E.follow_fleet && fleetwm_stamp() != E.fleet.stamp) follow_fleetwm();
        }
        if (E.side_close_at > 0 && E.now >= E.side_close_at) { E.side_close_at = 0; if (!E.pinned && E.side_open) open_side(false); }
        if (E.now >= E.next_hud) { E.next_hud = E.now + (!E.focused || E.now - E.last_active > BLINK_FOR ? 5 : E.now - E.last_active > 3.0 ? 3 : 1); hud_tick(); }
        if (E.focused && E.now - E.last_active < BLINK_FOR && E.now - E.last_blink > 0.53) { E.last_blink = E.now; E.blink_on = !E.blink_on; redraw(); }
        else if ((!E.focused || E.now - E.last_active >= BLINK_FOR) && !E.blink_on) { E.blink_on = true; redraw(); }
        if (E.dirty) {
            E.dirty = false;
            double c0 = now_s();
            for (int attempt = 0; attempt < 2; attempt++) {
                uint32_t gen0 = atlas_generation(E.atlas);
                r_begin(E.r, E.W, E.H, C_EDITOR);
                double b0 = now_s();
                build_frame();
                E.last_build_ms = (now_s() - b0) * 1000.0;
                r_end(E.r);
                if (atlas_generation(E.atlas) == gen0) break;   /* atlas was cleared mid-frame: UVs built before are stale, draw again */
            }
            end_op_if_pending();
            E.frames++;
            if (shot && E.now - t0 > shot_after) { write_ppm(shot); E.running = false; }
            SDL_GL_SwapWindow(E.win);
            double cost = now_s() - c0;
            E.frame_cost = E.frame_cost == 0 ? cost : E.frame_cost * 0.9 + cost * 0.1;
        }
        if (shot && !E.dirty && E.now - t0 > shot_after) redraw();
        if (exit_after > 0 && E.now - t0 > exit_after) E.running = false;
    }
    if (stats_at_exit) printf("%s\n", cache_stats_text(true).c_str());
    while (!E.terms.empty()) kill_terminal(0);
    return 0;
}
