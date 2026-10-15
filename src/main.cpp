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
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "appicon.h"
#include "askpass.h"
#include "remote.h"
#include "doc.h"
#include "engine/atlas.h"
#include "engine/font.h"
#include "engine/gl.h"
#include "engine/render.h"
#include "fuzzy.h"
#include "git.h"
#include "merge.h"
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
#include "vim.h"

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
         C_CURLINE, C_GUIDE, C_GUIDE_ACT, C_OCC, C_BRACKET, C_SB_THUMB, C_SB_HOT,
         C_GIT_ADD, C_GIT_MOD, C_GIT_DEL, C_GIT_UNT, C_GIT_CONF,                 /* file status colours (VS Code's gitDecoration.*) */
         C_GUT_ADD, C_GUT_MOD, C_GUT_DEL,                                         /* the change bars in the editor gutter (editorGutter.*) */
         C_DIFF_ADD, C_DIFF_ADD_TXT, C_DIFF_DEL, C_DIFF_DEL_TXT, C_DIFF_FILL, C_DIFF_FOLD;   /* diff editor line and word backgrounds */
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
    A_TOGGLE_SIDEBAR, A_PIN, A_VIEW_EXPLORER, A_VIEW_GIT, A_LANG, A_STATS, A_NEXT_TAB, A_PREV_TAB, A_ABOUT, A_REFRESH, A_THEME, A_SEARCH, A_TOG_REGEX, A_TOG_CASE, A_TOG_WORD, A_TERM_TOGGLE, A_TERM_NEW, A_TERM_KILL, A_TERM_PICK, A_TERM_EXTERNAL, A_GOTO, A_REOPEN, A_CLOSE_ALL, A_PALETTE, A_REPLACE, A_OUTLINE, A_VIM, A_SHORTCUTS, A_COUNT
};
const char *ACT_NAMES[A_COUNT] = {"none", "new", "open_file", "open_folder", "save", "saveas", "close", "quit", "undo", "redo", "cut", "copy", "paste", "selall", "find", "quick",
                                  "toggle_sidebar", "pin", "view_explorer", "view_git", "lang", "stats", "next_tab", "prev_tab", "about", "refresh", "theme", "search", "tog_regex", "tog_case", "tog_word", "term_toggle", "term_new", "term_kill", "term_pick", "term_external", "goto", "reopen", "close_all", "palette", "replace", "outline", "vim", "shortcuts"};

const char *ACT_LABEL[A_COUNT] = {"", "new tab", "open-file dialog", "open-folder dialog", "save", "save-as dialog", "close tab", "quit", "undo", "redo", "cut", "copy", "paste", "select all",
                                  "find box", "quick open", "toggle sidebar", "pin sidebar", "files tab", "git tab", "language picker", "stats panel", "next tab", "previous tab",
                                  "about", "refresh", "theme picker", "search tab", "toggle regex", "toggle case", "toggle word", "terminal panel", "new terminal", "kill terminal", "terminal picker", "external terminal", "go to line", "reopen closed tab", "close all tabs", "command palette", "replace", "toggle outline", "toggle vim keys", "keyboard shortcuts"};

struct MenuItem { const char *label, *keys; int act; };   /* act A_NONE with a label = disabled, empty label = separator */
struct Menu { const char *name; std::vector<MenuItem> items; };
const std::vector<Menu> MENUS = {
    {"File", {{"New File", "Ctrl+N", A_NEW}, {"Open File...", "Ctrl+O", A_OPEN_FILE}, {"Open Folder...", "Ctrl+K Ctrl+O", A_OPEN_FOLDER}, {"Quick Open...", "Ctrl+P", A_QUICK}, {"", "", A_NONE},
              {"Save", "Ctrl+S", A_SAVE}, {"Save As...", "Ctrl+Shift+S", A_SAVEAS}, {"", "", A_NONE}, {"Close Tab", "Ctrl+W", A_CLOSE}, {"Exit", "Ctrl+Q", A_QUIT}}},
    {"Edit", {{"Undo", "Ctrl+Z", A_UNDO}, {"Redo", "Ctrl+Y", A_REDO}, {"", "", A_NONE}, {"Cut", "Ctrl+X", A_CUT}, {"Copy", "Ctrl+C", A_COPY}, {"Paste", "Ctrl+V", A_PASTE},
              {"Select All", "Ctrl+A", A_SELALL}, {"", "", A_NONE}, {"Go to Line...", "Ctrl+G", A_GOTO}, {"Find", "Ctrl+F", A_FIND}, {"Replace", "Ctrl+H", A_REPLACE}, {"Find in Files", "Ctrl+Shift+F", A_SEARCH}}},
    {"View", {{"Files", "Ctrl+Shift+E", A_VIEW_EXPLORER}, {"Search", "Ctrl+Shift+F", A_SEARCH}, {"Git", "Ctrl+Shift+G", A_VIEW_GIT}, {"", "", A_NONE}, {"Toggle Sidebar", "Ctrl+B", A_TOGGLE_SIDEBAR},
              {"Pin / Unpin Sidebar", "", A_PIN}, {"", "", A_NONE}, {"Next Tab", "Ctrl+PgDn", A_NEXT_TAB}, {"Previous Tab", "Ctrl+PgUp", A_PREV_TAB}, {"", "", A_NONE},
              {"Command Palette...", "Ctrl+Shift+P", A_PALETTE}, {"", "", A_NONE}, {"Language Mode...", "Ctrl+K M", A_LANG}, {"Color Theme...", "Ctrl+K Ctrl+T", A_THEME}, {"", "", A_NONE}, {"Outline (Files tab)", "", A_OUTLINE}, {"Vim Keys", "", A_VIM}, {"", "", A_NONE}, {"Cache Statistics", "Ctrl+Shift+I", A_STATS}}},
    {"Terminal", {{"New Terminal", "Ctrl+Shift+`", A_TERM_NEW}, {"Toggle Terminal", "Ctrl+`", A_TERM_TOGGLE}, {"Kill Terminal", "", A_TERM_KILL}, {"", "", A_NONE},
                  {"Choose Terminal...", "", A_TERM_PICK}, {"Open External Terminal Here", "", A_TERM_EXTERNAL}}},
    {"Help", {{"Keyboard Shortcuts", "Ctrl+K Ctrl+H", A_SHORTCUTS}, {"", "", A_NONE}, {"About Hootedit", "", A_ABOUT}}},
};

struct Hit { float x, y, w, h; int kind, id; };
enum HitKind { H_TAB = 1, H_TABCLOSE, H_BOX, H_ACT, H_MENU, H_MENUITEM, H_PICKITEM, H_QUICKITEM, H_BTN_SAVE, H_BTN_DISCARD, H_BTN_CANCEL, H_TREE, H_ROOT, H_ACTIVITY, H_PIN, H_GITROW, H_SIDEBAR, H_SIDEBOX, H_SIDEHIT, H_SRES, H_TERMTAB, H_SPLIT, H_VSCROLL, H_FD_ITEM, H_FD_OK, H_FD_CANCEL, H_FD_PLACE, H_FD_HIDDEN, H_TABSPACE, H_GITSEC, H_GITSTG, H_GITOPEN, H_GITMSG, H_GITCOMMIT, H_GITLOG, H_GITCFILE, H_GITTL, H_GITREFRESH, H_GITALL, H_DIFFBTN, H_DIFFFOLD, H_GUTTER, H_BLAME, H_DLGBTN, H_DLGBG, H_DLGINPUT, H_CTXITEM, H_CTXBG, H_GITMORE, H_GITBRANCH, H_GITDD, H_GITDISC, H_GITDIR, H_DIFFHUNK, H_RROW, H_RNEW, H_PEEK, H_GITOP, H_CONFLICT, H_MERGE, H_REPL, H_SECT, H_OPENED, H_OUTLINE };
enum Mode { M_EDIT, M_FIND, M_PICK, M_CONFIRM, M_PROMPT, M_MENU, M_QUICK, M_SIDE, M_FILEDLG, M_GITMSG, M_DIALOG, M_CTX };
enum FdKind { FD_OPEN_FILE, FD_OPEN_FOLDER, FD_SAVE };
enum PickKind { PICK_LANG, PICK_THEME, PICK_TERMINAL, PICK_CUSTOM };
enum Pending { P_NONE, P_CLOSE, P_QUIT };
enum PromptKind { PK_SAVEAS, PK_OPEN_FILE, PK_OPEN_FOLDER };
enum View { V_EXPLORER, V_SEARCH, V_GIT, V_REMOTE };

struct GitUi {
    std::string msg;                                   /* the commit message being typed */
    bool open_staged = true, open_changes = true, open_graph = true, open_tl = true, open_merge = true;
    std::vector<GitCommit> log;                        /* the branch history, newest first */
    struct Lane { int col = 0; std::vector<uint8_t> top, bot; std::vector<int> conv, out; };   /* the graph drawing of one commit row, see graph_lanes */
    std::vector<Lane> lanes;
    int lane_cols = 1;
    int log_sel = -1;                                  /* the expanded commit, or -1 */
    std::vector<GitFile> sel_files;                    /* its files, or the files that differ between two commits */
    std::string cmp_a, cmp_b;                          /* set while sel_files is a comparison of two commits */
    std::vector<GitCommit> tl;                         /* history of the active file */
    std::string tl_path;
    std::string mark_hash, mark_label;                 /* "Select for compare": Ctrl+click marks, Shift+click compares with the mark */
    std::string err;                                   /* the last commit failure */
    bool blame_inline = false;                          /* the faint "who changed this line" text after the cursor line */
    double blame_t0 = 0;
    bool tree_view = false;                            /* the file lists as a folder tree instead of a flat list */
    std::set<std::string> collapsed;                   /* tree folders the user closed, as "<section>:<dir>" */
    int smart_commit = 0;                              /* nothing staged: 0 ask, 1 always stage all, 2 never */
    bool sync_confirm = true;                          /* ask before Sync Changes */
};

/* A VS Code style modal: a message, optional detail, an optional input, and buttons left to right. done gets the button index, or -1 for Esc. */
struct Dialog {
    std::string title, detail, text, placeholder;
    bool input = false, mask = false;
    bool fresh = false;                  /* the prefilled text is selected: the next key replaces it */
    std::vector<std::string> btns;
    int focus = 0;
    std::function<void(int, const std::string &)> done;
};

/* A context menu. Submenus open to the right while the pointer is over their item. */
struct CtxItem {
    std::string label, hint;
    bool sep = false, enabled = true, checked = false;
    std::function<void()> act;
    std::vector<CtxItem> sub;
};
struct CtxMenu { float x = 0, y = 0; std::vector<CtxItem> items; std::vector<int> open; };   /* open: the item index opened at each level */

/* One git command on a worker thread, so a slow push or pull does not freeze the window. */
struct GitJob {
    std::thread th;
    std::atomic<bool> done{false};
    bool running = false;
    std::string label, out;
    int code = 0;
    std::function<void(int, const std::string &)> cb;
};

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
    std::string repl_q;                                  /* the replacement typed in the Replace box */
    bool repl_on = false, repl_focus = false;            /* the Replace box is open; the keyboard goes to it */
    struct PItem { std::string label; int id; bool current; };
    PickKind pick_kind = PICK_LANG;
    std::string pick_filter;
    int pick_sel = 0, pick_scroll = 0;
    std::vector<PItem> pick_items;
    std::string pick_title;
    std::function<void(const PItem &)> pick_cb;
    Dialog dlg;
    CtxMenu ctx;
    Mode mode_before = M_EDIT;                       /* where to return when a dialog or menu closes */
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
    unsigned long use_clock = 0;                         /* stamps Doc::used */
    std::vector<Doc *> mru_walk;                         /* the tab order while Ctrl is held down during Ctrl+Tab */
    int mru_pos = -1;
    float mx = -1, my = -1;
    double tip_at = 0;                                   /* when the pointer has rested long enough for a tooltip, 0 = none pending */
    bool tip_on = false;
    std::string title_last;
    bool blank_tab = false;                          /* the placeholder document has been turned into a real empty tab (double click or Ctrl+N) */
    float hit_floor = 1e9f;                          /* hit boxes are cut off at this y: nothing behind the status bar can be clicked */
    std::string git_for;                             /* the folder the Git tab was last computed for */

    /* workspace and sidebar */
    Tree tree;
    GitInfo git;
    View view = V_EXPLORER;
    bool pinned = true, side_open = true, root_open = true, opened_open = true, outline_open = true;
    std::unordered_map<std::string, char> deco;        /* absolute path -> git status letter, for file and folder rows in the Explorer */
    bool outline_on = false;                           /* Explorer shows the symbols of the active file (View > Outline, off by default) */
    bool vim_on = false;                               /* modal editing */
    double side_close_at = 0;
    float side_scroll = 0;
    GitUi g;
    float tab_scroll = 0;                            /* how far the tab strip is scrolled, in pixels */
    bool tab_reveal = false;                         /* bring the active tab into view on the next frame */
    int tab_press = -1;                              /* tab under the mouse button, -1 none */
    float tab_press_x = 0;
    bool tab_dragging = false;
    std::vector<float> tab_x, tab_w;                 /* the last frame's tab geometry (strip coordinates, unscrolled) */
    std::vector<std::string> closed_tabs;            /* paths of closed tabs, newest last, for Ctrl+Shift+T */
    bool chord_k = false;                            /* Ctrl+K was pressed: the next key finishes the chord */

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
    struct OpRec { std::string name; double ms; int n; double build_ms; double work_ms; std::string brief; };   /* brief: the name without the (details), for the status bar */
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

/* what is known about one open file's relation to git: the HEAD text and the changed line ranges (gutter bars), and blame */
struct GutterCache {
    uint64_t rev = ~0ull;                          /* buffer revision the hunks were computed for */
    bool base_stale = true, computed = false, tracked_ok = false;
    std::vector<std::string> base_lines;
    std::unordered_map<std::string, int> ids;      /* line text -> id, over the HEAD text */
    std::vector<DiffHunk> hunks;                   /* HEAD lines against the buffer's lines */
    std::vector<GitBlame> blame;
    bool blame_tried = false, blame_ok = false;
};
std::unordered_map<const Doc *, GutterCache> g_gutter;
/* merge conflict blocks of the open files (see conflicts_for) */
struct Conflict { long a = -1, base = -1, mid = -1, b = -1; };   /* line numbers of the four marker lines (base: the ||||||| line of diff3 style, else -1) */
struct ConflictCache { uint64_t rev = ~0ull; bool listed = false; std::vector<Conflict> v; };
std::unordered_map<const Doc *, ConflictCache> g_conf;
struct Peek { const Doc *doc = nullptr; int b0 = -1; } g_peek;   /* the inline diff of one change under its lines, opened from a change bar */

void gutter_invalidate() { g_conf.clear(); for (auto &kv : g_gutter) { kv.second.base_stale = true; kv.second.computed = false; kv.second.blame_tried = false; kv.second.blame_ok = false; } }
/* no file and nothing typed: the start screen shows, with no tab strip */
bool splash() { return E.docs.size() == 1 && D().pristine() && !E.blank_tab; }
Ed::Term *cur_term();
std::vector<int> fd_view();
void git_refresh();
void remote_push_changed();
void git_ui_reload();
void git_ui_timeline();
void diff_reload_all();
void gutter_invalidate();
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

void record_op(const std::string &name, double ms, double build_ms = -1, double work_ms = -1, const std::string &brief = std::string()) {
    if (!E.ops.empty() && E.ops.back().name == name) { E.ops.back().ms = ms; E.ops.back().n++; E.ops.back().build_ms = build_ms; E.ops.back().work_ms = work_ms; E.ops.back().brief = brief; }   /* repeated keystrokes keep one line */
    else { E.ops.push_back({name, ms, 1, build_ms, work_ms, brief}); if (E.ops.size() > 64) E.ops.erase(E.ops.begin()); }
}

void end_op_if_pending() {
    if (!E.op_pending) return;
    double ms = (now_s() - E.op_t0) * 1000.0;
    record_op(E.op_name + (E.op_detail.empty() ? "" : " (" + E.op_detail + ")"), ms, E.last_build_ms, E.op_work > 0 ? E.op_work : -1, E.op_name);
    E.op_pending = false;
    E.op_t0 = 0;
}

/* ---- settings (the part that must survive a restart: pin state and sidebar view) ---- */

std::string settings_path() {
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    std::string dir = x && *x ? x : std::string(h ? h : ".") + "/.config";
    return dir + "/hootedit/settings.ini";
}

bool have_vim() {   /* the Vim Keys setting needs a vim on PATH (the toggle is greyed out without one) */
    static int found = -1;
    if (found < 0) {
        found = 0;
        const char *path = getenv("PATH");
        std::string p = path ? path : "/usr/bin:/bin";
        for (size_t a = 0; a <= p.size() && !found; ) {
            size_t b = p.find(':', a);
            if (b == std::string::npos) b = p.size();
            std::string dir = p.substr(a, b - a);
            for (const char *nm : {"vim", "nvim", "vi"}) if (!dir.empty() && access((dir + "/" + nm).c_str(), X_OK) == 0) { found = 1; break; }
            a = b + 1;
        }
    }
    return found == 1;
}

Vim g_vim;
bool vim_active() { return E.vim_on && have_vim(); }
void vim_reset() { g_vim.reset(vim_active()); }

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
        else if (!strcmp(line, "sidebar_view")) E.view = !strcmp(v, "git") ? V_GIT : !strcmp(v, "search") ? V_SEARCH : !strcmp(v, "remote") ? V_REMOTE : V_EXPLORER;
        else if (!strcmp(line, "theme")) E.theme = ui_theme_find(v);
        else if (!strcmp(line, "follow_fleetwm")) E.follow_fleet = atoi(v) != 0;
        else if (!strcmp(line, "terminal")) E.term_choice = v;
        else if (!strcmp(line, "git_smart_commit")) E.g.smart_commit = atoi(v);
        else if (!strcmp(line, "git_sync_confirm")) E.g.sync_confirm = atoi(v) != 0;
        else if (!strcmp(line, "outline")) E.outline_on = atoi(v) != 0;
        else if (!strcmp(line, "vim_keys")) E.vim_on = atoi(v) != 0;
        else if (!strcmp(line, "git_tree")) E.g.tree_view = atoi(v) != 0;
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
    fprintf(f, "sidebar_pinned=%d\nsidebar_view=%s\ntheme=%s\nfollow_fleetwm=%d\nterminal=%s\npanel_height=%d\n", E.pinned ? 1 : 0, E.view == V_GIT ? "git" : E.view == V_SEARCH ? "search" : E.view == V_REMOTE ? "remote" : "explorer", E.theme ? E.theme->name : "Xylonic Dark", E.follow_fleet ? 1 : 0, E.term_choice.c_str(), E.panel_logical);
    fprintf(f, "outline=%d\nvim_keys=%d\n", E.outline_on ? 1 : 0, E.vim_on ? 1 : 0);
    fprintf(f, "git_smart_commit=%d\ngit_sync_confirm=%d\ngit_tree=%d\n", E.g.smart_commit, E.g.sync_confirm ? 1 : 0, E.g.tree_view ? 1 : 0);
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
    if (E.mru_pos < 0) D().used = ++E.use_clock;
    E.follow = true;
    E.tab_reveal = true;
    E.find_msg.clear();
    E.status_msg.clear();
    std::string t = D().name() + " - Hootedit";
    if (t != E.title_last) { E.title_last = t; SDL_SetWindowTitle(E.win, t.c_str()); }
    if (!D().path.empty() && E.tree.root().size() && D().path.compare(0, E.tree.root().size(), E.tree.root()) == 0) E.tree.reveal(D().path);
    git_follow_active();
    if (E.view == V_GIT) git_ui_timeline();
    redraw();
}

void remove_doc(int i) {
    g_gutter.erase(E.docs[(size_t)i].get());
    if (!E.docs[(size_t)i]->path.empty()) { E.closed_tabs.push_back(E.docs[(size_t)i]->path); if (E.closed_tabs.size() > 30) E.closed_tabs.erase(E.closed_tabs.begin()); }
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

char change_status(const GitChange &c) {
    if (c.x == 'U' || c.y == 'U' || (c.x == 'A' && c.y == 'A') || (c.x == 'D' && c.y == 'D')) return 'C';
    if (c.x == '?') return '?';
    return c.y != ' ' ? c.y : c.x;
}

/* what the Explorer shows from git: coloured names, a status letter, folders marked when something inside changed, deleted files as ghost rows */
void git_deco_rebuild() {
    E.deco.clear();
    std::vector<std::string> ghosts;
    if (E.git.repo) {
        for (const GitChange &c : E.git.changes) {
            std::string abs = E.git.root + "/" + c.path;
            char st = change_status(c);
            if (!c.path.empty() && c.path.back() == '/') abs.pop_back();   /* an untracked directory */
            E.deco[abs] = st;
            if (st == 'D') { struct stat sb; if (stat(abs.c_str(), &sb) != 0) ghosts.push_back(abs); }
            char dc = st == 'C' ? 'C' : st == '?' ? '?' : 'M';
            for (size_t sl = abs.rfind('/'); sl != std::string::npos && sl > E.git.root.size(); sl = abs.rfind('/', sl - 1)) {
                std::string dir = abs.substr(0, sl);
                auto it = E.deco.find(dir);
                if (it == E.deco.end() || (it->second == '?' && dc != '?') || dc == 'C') E.deco[dir] = dc;
                if (sl == 0) break;
            }
        }
    }
    std::sort(ghosts.begin(), ghosts.end());
    std::vector<std::string> old = E.tree.ghosts();
    std::sort(old.begin(), old.end());
    if (old != ghosts) { E.tree.set_ghosts(ghosts); E.tree.refresh(); }
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
    git_deco_rebuild();
    git_ui_reload();
    diff_reload_all();
    gutter_invalidate();
    redraw();
}

/* Ctrl+Tab goes to the previously used tab; more presses with Ctrl still held walk back through the tabs in order of use */
void mru_cycle(int dir) {
    if (E.docs.size() < 2) return;
    if (E.mru_pos < 0) {
        E.mru_walk.clear();
        for (auto &dp : E.docs) E.mru_walk.push_back(dp.get());
        std::stable_sort(E.mru_walk.begin(), E.mru_walk.end(), [](const Doc *a, const Doc *b) { return a->used > b->used; });
        E.mru_pos = 0;
    }
    int n = (int)E.mru_walk.size();
    int target = ((E.mru_pos + dir) % n + n) % n;
    for (size_t i = 0; i < E.docs.size(); i++) if (E.docs[i].get() == E.mru_walk[(size_t)target]) { set_active((int)i); break; }
    E.mru_pos = target;
}

void mru_end() {
    if (E.mru_pos < 0) return;
    E.mru_pos = -1;
    E.mru_walk.clear();
    if (!E.docs.empty()) D().used = ++E.use_clock;
}

void open_path(const std::string &path, bool preview = false) {   /* preview: a single click in the Explorer; the tab is italic and the next preview replaces it */
    for (size_t i = 0; i < E.docs.size(); i++) if (E.docs[i]->path == path) { if (!preview) E.docs[i]->preview = false; set_active((int)i); return; }
    bool replace_first = E.docs.size() == 1 && D().pristine();
    Doc &d = new_doc(false);
    double t_open = now_s();
    bool opened = d.open(path.c_str());
    t_open = (now_s() - t_open) * 1000.0;
    E.op_work = 0;
    if (opened) { size_t sl = path.rfind('/'); E.op_work = 0; begin_op("open " + (sl == std::string::npos ? path : path.substr(sl + 1)), "map + index " + fmt_ms(t_open) + ", " + fmt_mb((double)d.buf.size()) + ", " + std::to_string(d.buf.lines()) + " lines"); E.op_work = t_open; }
    if (!opened) { E.docs.pop_back(); E.status_msg = "cannot open " + path; redraw(); return; }
    if (preview) {
        d.preview = true;
        for (size_t i = 0; i + 1 < E.docs.size(); i++) if (E.docs[i]->preview && !E.docs[i]->modified()) { E.docs.erase(E.docs.begin() + (long)i); break; }
    }
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

/* ---- diff tabs: both sides come from git (a revision, the index) or from the file on disk ---- */

std::string rel_to_root(const std::string &abs) {
    const std::string &r = E.git.root;
    return !r.empty() && abs.size() > r.size() + 1 && abs.compare(0, r.size(), r) == 0 && abs[r.size()] == '/' ? abs.substr(r.size() + 1) : std::string();
}

std::string diff_fetch(const DiffView &v, const std::string &rev) {
    if (rev.empty()) return std::string();
    if (rev != "WORK") return git_show(v.root, rev, v.path);
    std::string out;
    FILE *f = fopen((v.root + "/" + v.path).c_str(), "rb");
    if (!f) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (8u << 20)) out.append(buf, n);
    fclose(f);
    return out;
}

void lex_states(const Lang *lang, const std::vector<std::string> &lines, std::vector<uint8_t> &st) {
    st.assign(lines.size() + 1, 0);
    std::vector<uint8_t> tok(256);
    uint8_t state = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        st[i] = state;
        if (lang == lang_plain()) continue;
        if (tok.size() < lines[i].size() + 1) tok.resize(lines[i].size() + 64);
        state = lex_line(lang, lines[i].data(), lines[i].size(), state, tok.data());
    }
    st[lines.size()] = state;
}

void diff_load(DiffView &v) {
    std::string l = diff_fetch(v, v.left_rev), r = diff_fetch(v, v.right_rev);
    v.binary = memchr(l.data(), 0, std::min<size_t>(l.size(), 8192)) || memchr(r.data(), 0, std::min<size_t>(r.size(), 8192));
    v.too_big = l.size() > (4u << 20) || r.size() > (4u << 20);
    v.m = DiffModel();
    v.sa.clear(); v.sb.clear();
    if (!v.binary && !v.too_big) {
        diff_build(v.m, l, r);
        const std::string &first = !v.m.b.empty() ? v.m.b[0] : !v.m.a.empty() ? v.m.a[0] : std::string();
        v.lang = lang_detect(v.path.c_str(), first.data(), first.size());
        lex_states(v.lang, v.m.a, v.sa);
        lex_states(v.lang, v.m.b, v.sb);
    } else v.lang = lang_plain();
    v.vdirty = true;
}

void diff_reload_all() {
    for (auto &dp : E.docs) {
        if (!dp->dv) continue;
        DiffView &v = *dp->dv;
        if (v.root != E.git.root) continue;
        bool live = v.left_rev == "WORK" || v.right_rev == "WORK" || v.left_rev == ":" || v.right_rev == ":" || v.left_rev == "HEAD" || v.right_rev == "HEAD";
        if (live) diff_load(v);
    }
}

Doc &open_diff_spec(const std::string &path, const std::string &lrev, const std::string &rrev, const std::string &llabel, const std::string &rlabel, const std::string &title, const std::string &info) {
    for (size_t i = 0; i < E.docs.size(); i++) {
        Doc &o = *E.docs[i];
        if (o.dv && o.dv->root == E.git.root && o.dv->path == path && o.dv->left_rev == lrev && o.dv->right_rev == rrev) { diff_load(*o.dv); set_active((int)i); return o; }
    }
    bool replace_first = E.docs.size() == 1 && D().pristine() && !E.blank_tab;
    Doc &d = new_doc(false);
    d.title = title;
    d.dv = std::make_shared<DiffView>();
    DiffView &v = *d.dv;
    v.root = E.git.root; v.path = path; v.left_rev = lrev; v.right_rev = rrev;
    v.left_label = llabel; v.right_label = rlabel; v.info = info;
    diff_load(v);
    d.lang = v.lang;
    if (replace_first) E.docs.erase(E.docs.begin());
    set_active((int)E.docs.size() - 1);
    return D();
}

std::string base_name(const std::string &p) { size_t sl = p.rfind('/'); return sl == std::string::npos ? p : p.substr(sl + 1); }

void open_change_diff(const GitChange &c, bool staged) {
    std::string name = base_name(c.path);
    if (c.x == '?' && c.y == '?') { open_diff_spec(c.path, "", "WORK", "(untracked)", "Working Tree", name + " (Untracked)", "New file, not tracked yet"); return; }
    if (staged) open_diff_spec(c.path, "HEAD", ":", "HEAD", "Index", name + " (Index)", "Staged changes: HEAD against the index");
    else open_diff_spec(c.path, ":", "WORK", "Index", "Working Tree", name + " (Working Tree)", "Unstaged changes: the index against the file on disk");
}

void open_commit_diff(const GitCommit &c, const GitFile &f) {
    std::string h7 = c.hash.substr(0, 7);
    std::string info = h7 + "  " + c.subject + "   " + c.author + ", " + git_ago(c.date, (long)time(nullptr));
    open_diff_spec(f.path, f.status == 'A' ? std::string() : c.hash + "^", f.status == 'D' ? std::string() : c.hash, f.status == 'A' ? "(new file)" : h7 + "^", f.status == 'D' ? "(deleted)" : h7, base_name(f.path) + " (" + h7 + ")", info);
}

void open_compare_diff(const std::string &path, const std::string &a, const std::string &b) {
    open_diff_spec(path, a, b, a.substr(0, 7), b.substr(0, 7), base_name(path) + " (" + a.substr(0, 7) + " .. " + b.substr(0, 7) + ")", "Comparing two revisions of " + path);
}

/* ---- the Source Control lists that come from git log ---- */

void git_ui_timeline() {
    GitUi &g = E.g;
    std::string rel = E.git.repo && !E.docs.empty() && !D().dv ? rel_to_root(D().path) : std::string();
    if (rel == g.tl_path && !rel.empty()) return;
    g.tl_path = rel;
    g.tl.clear();
    if (!rel.empty()) g.tl = git_log(E.git.root, 60, rel);
}

/* Lanes for the history graph: each lane waits for the hash of the commit it leads to. A commit takes the lane that waits for it
 * (new one if none does), its first parent continues in that lane, other parents get a lane of their own (or join one that
 * already waits for them), and further lanes that waited for the same commit end here. */
void graph_lanes(GitUi &g) {
    g.lanes.assign(g.log.size(), GitUi::Lane());
    std::vector<std::string> L;
    size_t maxc = 1;
    for (size_t i = 0; i < g.log.size(); i++) {
        const GitCommit &c = g.log[i];
        GitUi::Lane &r = g.lanes[i];
        int col = -1;
        for (size_t k = 0; k < L.size(); k++) if (L[k] == c.hash) { col = (int)k; break; }
        r.top.assign(L.size(), 0);
        for (size_t k = 0; k < L.size(); k++) r.top[k] = L[k].empty() ? 0 : 1;
        if (col < 0) {
            for (size_t k = 0; k < L.size(); k++) if (L[k].empty()) { col = (int)k; break; }
            if (col < 0) { col = (int)L.size(); L.push_back(""); }
            r.top.resize(L.size(), 0);
            r.top[(size_t)col] = 0;   /* a branch tip: nothing comes in from above */
        } else {
            for (size_t k = 0; k < L.size(); k++) if ((int)k != col && L[k] == c.hash) { r.conv.push_back((int)k); L[k].clear(); }
        }
        r.col = col;
        L[(size_t)col].clear();
        for (size_t pi = 0; pi < c.parent_hashes.size(); pi++) {
            const std::string &ph = c.parent_hashes[pi];
            if (pi == 0) { L[(size_t)col] = ph; continue; }   /* the first parent keeps the lane; lanes that wait for the same commit join up when it is reached */
            int at = -1;
            for (size_t k = 0; k < L.size(); k++) if (L[k] == ph) { at = (int)k; break; }
            if (at < 0) {
                for (size_t k = 0; k < L.size(); k++) if (L[k].empty() && (int)k != col) { at = (int)k; break; }
                if (at < 0) { at = (int)L.size(); L.push_back(""); }
                L[(size_t)at] = ph;
            }
            r.out.push_back(at);
        }
        r.bot.assign(L.size(), 0);
        for (size_t k = 0; k < L.size(); k++) r.bot[k] = L[k].empty() ? 0 : 1;
        r.top.resize(L.size(), 0);
        maxc = std::max(maxc, L.size());
        while (!L.empty() && L.back().empty()) L.pop_back();
    }
    g.lane_cols = (int)std::min<size_t>(maxc, 5);
}

void git_ui_reload() {
    GitUi &g = E.g;
    if (!E.git.repo) { g.log.clear(); g.tl.clear(); g.tl_path.clear(); g.log_sel = -1; g.sel_files.clear(); return; }
    if (E.view != V_GIT) { g.tl_path = "\x01"; return; }   /* loaded when the tab is shown */
    std::string keep = g.log_sel >= 0 && (size_t)g.log_sel < g.log.size() ? g.log[(size_t)g.log_sel].hash : std::string();
    g.log = git_log(E.git.root, 120, "", true);
    graph_lanes(g);
    g.log_sel = -1;
    if (!keep.empty() && g.cmp_a.empty()) for (size_t i = 0; i < g.log.size(); i++) if (g.log[i].hash == keep) g.log_sel = (int)i;
    if (g.log_sel < 0) { g.sel_files.clear(); g.cmp_a.clear(); g.cmp_b.clear(); }
    g.tl_path = "\x01";   /* force the timeline to reload */
    git_ui_timeline();
}

void git_do(bool ok, const char *what) {
    if (!ok) E.status_msg = std::string("git ") + what + " failed";
    git_refresh();
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
    remote_push_changed();
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
    if (ok) remote_push_changed();
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

/* Replace: the selection when it is a match of the find text, then on to the next one; Replace All is one undo step */
bool find_matcher(Regex &re) {
    if (E.find_q.empty()) return false;
    bool icase = !E.find_case;
    if (icase) for (unsigned char c : E.find_q) if (c >= 'A' && c <= 'Z') icase = false;
    std::string err;
    if (!re.compile(E.find_regex ? E.find_q : Searcher::escape_literal(E.find_q), icase, &err)) { E.find_msg = "invalid: " + err; return false; }
    return true;
}

void replace_one() {
    Doc &d = D();
    Regex re;
    if (!find_matcher(re)) return;
    if (d.has_sel()) {   /* is what is selected one whole match? */
        std::string sel = d.selection();
        Matcher m(re);
        size_t ms, me;
        if (sel.find('\n') == std::string::npos && m.find(sel.data(), sel.size(), 0, &ms, &me) && ms == 0 && me == sel.size()) {
            size_t lo = d.sel_lo();
            d.break_group();
            d.replace(lo, sel.size(), E.repl_q.data(), E.repl_q.size());
            d.break_group();
            find_from(lo + E.repl_q.size());
            return;
        }
    }
    find_from(d.sel_lo());   /* nothing matching selected yet: select the next match first */
}

void replace_all() {
    Doc &d = D();
    Regex re;
    if (!find_matcher(re)) return;
    Matcher m(re);
    std::string out, t;
    size_t nl = d.buf.lines(), count = 0;
    for (size_t line = 0; line < nl; line++) {
        d.buf.line_text(line, t);
        size_t from = 0;
        std::string o;
        while (from <= t.size()) {
            size_t ms, me;
            if (!m.find(t.data(), t.size(), from, &ms, &me) || me == ms) { if (me == ms && ms < t.size() && from <= ms) { /* an empty match: skip a character */ o.append(t, from, ms + 1 - from); from = ms + 1; continue; } break; }
            o.append(t, from, ms - from);
            o += E.repl_q;
            from = me;
            count++;
        }
        if (from <= t.size()) o.append(t, from, std::string::npos);
        out += o;
        if (line + 1 < nl) out += '\n';
    }
    if (!count) { E.find_msg = "No results"; return; }
    size_t keep = d.cur, top = d.top;
    d.break_group();
    d.replace(0, d.buf.size(), out.data(), out.size());
    d.break_group();
    d.move_to(std::min(keep, d.buf.size()), false, false);
    d.top = std::min(top, d.buf.lines() - 1);
    E.status_msg = sfmt("replaced %zu occurrence%s", count, count == 1 ? "" : "s");
    E.find_msg.clear();
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
    if (!E.quick_q.empty() && E.quick_q[0] == ':') { E.quick_hits.clear(); E.quick_pos.clear(); return; }   /* ":42" is a line number, not a file name */
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
void draw_dot(float cx, float cy, int size, uint32_t rgba);
bool hovered(int kind, int id);

/* ---- modal dialog and context menu (the VS Code look: a warning message, detail text, buttons) ---- */

GitJob g_job;

std::vector<std::string> wrap_lines(const std::string &t, float w, int style = FS_REGULAR) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t e = t.find('\n', pos);
        std::string para = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        std::string line;
        size_t p = 0;
        while (p < para.size()) {
            size_t sp = para.find(' ', p);
            std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_width(E.ui, trial.c_str(), style) > w) { out.push_back(line); line = word; }
            else line = trial;
            while (text_width(E.ui, line.c_str(), style) > w && line.size() > 8) {   /* one word longer than the box: break it */
                size_t k = line.size();
                while (k > 1 && text_width(E.ui, line.substr(0, k).c_str(), style) > w) k--;
                out.push_back(line.substr(0, k));
                line = line.substr(k);
            }
            if (sp == std::string::npos) break;
            p = sp + 1;
        }
        out.push_back(line);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return out;
}

void dlg_show(const std::string &title, const std::string &detail, std::vector<std::string> btns, std::function<void(int, const std::string &)> done,
              bool input = false, const std::string &text = "", const std::string &ph = "", bool mask = false) {
    Dialog &d = E.dlg;
    d.mask = mask;
    d.title = title; d.detail = detail; d.btns = std::move(btns); d.done = std::move(done);
    d.input = input; d.text = text; d.placeholder = ph; d.focus = 0; d.fresh = input && !text.empty() && !mask;
    E.mode = M_DIALOG;
    redraw();
}

void dlg_finish(int idx) {
    auto cb = std::move(E.dlg.done);
    std::string txt = E.dlg.text;
    E.dlg.done = nullptr;
    E.mode = M_EDIT;
    redraw();
    if (cb) cb(idx, txt);
}

void ctx_show(float x, float y, std::vector<CtxItem> items) {
    E.ctx.x = x; E.ctx.y = y; E.ctx.items = std::move(items); E.ctx.open.clear();
    E.mode = M_CTX;
    redraw();
}

CtxItem ci(const std::string &label, std::function<void()> act, const std::string &hint = "", bool enabled = true, bool checked = false) {
    CtxItem c; c.label = label; c.act = std::move(act); c.hint = hint; c.enabled = enabled; c.checked = checked; return c;
}
CtxItem ci_sep() { CtxItem c; c.sep = true; return c; }
CtxItem ci_sub(const std::string &label, std::vector<CtxItem> sub) { CtxItem c; c.label = label; c.sub = std::move(sub); return c; }

struct CtxGeom { float x, y, w, h; const std::vector<CtxItem> *items; };
float ctx_row_h(const CtxItem &c) { return (float)S(c.sep ? 9 : 26); }
CtxGeom ctx_geom(int level) {
    const std::vector<CtxItem> *items = &E.ctx.items;
    float x = E.ctx.x, y = E.ctx.y, w = 0, h = 0;
    float px = 0, pw = 0;
    for (int l = 0; l <= level; l++) {
        w = 0; h = (float)S(8);
        for (const CtxItem &c : *items) {
            h += ctx_row_h(c);
            if (c.sep) continue;
            float lw = text_width(E.ui, c.label.c_str()) + (c.hint.empty() ? 0.f : text_width(E.ui, c.hint.c_str()) + (float)S(28));
            w = std::max(w, lw);
        }
        w += (float)S(58);
        if (l == 0) {
            x = std::max(0.f, std::min(x, (float)E.W - w - (float)S(4)));
            y = std::max(0.f, std::min(y, (float)E.H - h - (float)S(4)));
        } else {
            x = px + pw - (float)S(2);
            if (x + w > (float)E.W - (float)S(4)) x = std::max(0.f, px - w + (float)S(2));
            y = std::max(0.f, std::min(y, (float)E.H - h - (float)S(4)));
        }
        if (l < level) {
            int oi = E.ctx.open[(size_t)l];
            float iy = y + (float)S(4);
            for (int i = 0; i < oi; i++) iy += ctx_row_h((*items)[(size_t)i]);
            px = x; pw = w; y = iy - (float)S(4);
            items = &(*items)[(size_t)oi].sub;
        }
    }
    return {x, y, w, h, items};
}

void draw_ctx_menu() {
    hit(0, 0, (float)E.W, (float)E.H, H_CTXBG, 0);
    for (int l = 0; l <= (int)E.ctx.open.size(); l++) {
        CtxGeom g = ctx_geom(l);
        r_rect(E.r, g.x - 1, g.y - 1, g.w + 2, g.h + 2, C_MENU_BORDER);
        r_rect(E.r, g.x, g.y, g.w, g.h, C_MENU);
        float y = g.y + (float)S(4);
        for (size_t i = 0; i < g.items->size(); i++) {
            const CtxItem &c = (*g.items)[i];
            float rh = ctx_row_h(c);
            if (c.sep) { r_rect(E.r, g.x + (float)S(8), y + rh / 2, g.w - (float)S(16), 1, C_MENU_BORDER); y += rh; continue; }
            int id = (l << 12) | (int)i;
            bool open_here = l < (int)E.ctx.open.size() && E.ctx.open[(size_t)l] == (int)i;
            if (c.enabled && (hovered(H_CTXITEM, id) || open_here)) r_rect(E.r, g.x + (float)S(3), y, g.w - (float)S(6), rh, C_MENU_SEL);
            uint32_t col = c.enabled ? C_UI : C_UI_FAINT;
            if (c.checked) draw_icon(IC_CHECK, g.x + (float)S(8), y + (rh - (float)S(14)) / 2, S(14), col);
            draw_text(E.ui, g.x + (float)S(28), ui_base(y, rh), c.label.c_str(), col, g.w - (float)S(40));
            if (!c.hint.empty()) draw_text(E.ui, g.x + g.w - (float)S(14) - text_width(E.ui, c.hint.c_str()), ui_base(y, rh), c.hint.c_str(), C_UI_FAINT, g.w);
            if (!c.sub.empty()) draw_icon(IC_CHEV_R, g.x + g.w - (float)S(22), y + (rh - (float)S(16)) / 2, S(16), col);
            hit(g.x, y, g.w, rh, H_CTXITEM, id);
            y += rh;
        }
    }
}

void ctx_click(int id) {
    int l = id >> 12, i = id & 0xfff;
    CtxGeom g = ctx_geom(l);
    if (i < 0 || (size_t)i >= g.items->size()) return;
    const CtxItem &c = (*g.items)[(size_t)i];
    if (c.sep || !c.enabled) return;
    if (!c.sub.empty()) { E.ctx.open.resize((size_t)l); E.ctx.open.push_back(i); redraw(); return; }
    auto act = c.act;
    E.mode = M_EDIT;
    E.ctx.items.clear(); E.ctx.open.clear();
    redraw();
    if (act) act();
}

void ctx_hover() {   /* a submenu opens while its item is hovered; moving to a sibling closes it */
    if (E.mode != M_CTX || E.hover_kind != H_CTXITEM) return;
    int l = E.hover_id >> 12, i = E.hover_id & 0xfff;
    if (l > (int)E.ctx.open.size()) return;
    CtxGeom g = ctx_geom(l);
    if (i < 0 || (size_t)i >= g.items->size()) return;
    bool want = !(*g.items)[(size_t)i].sub.empty() && (*g.items)[(size_t)i].enabled;
    bool has = (int)E.ctx.open.size() > l && E.ctx.open[(size_t)l] == i;
    if (has && want) return;
    if ((int)E.ctx.open.size() > l) E.ctx.open.resize((size_t)l);
    if (want) E.ctx.open.push_back(i);
    redraw();
}

void draw_dialog() {
    Dialog &d = E.dlg;
    r_rect(E.r, 0, 0, (float)E.W, (float)E.H, C_SHADE);
    hit(0, 0, (float)E.W, (float)E.H, H_DLGBG, 0);
    float pad = (float)S(22), lh = (float)S(21);
    float pw = std::min((float)E.W - (float)S(24), (float)S(540)), iw = pw - 2 * pad;
    std::vector<std::string> tl = wrap_lines(d.title, iw - (float)S(30), FS_BOLD), dl = wrap_lines(d.detail, iw - (float)S(30));
    float bw_sum = 0;
    std::vector<float> bw;
    for (const std::string &b : d.btns) { bw.push_back(text_width(E.ui, b.c_str()) + (float)S(30)); bw_sum += bw.back() + (float)S(8); }
    bool stack = bw_sum - (float)S(8) > iw;
    float bh = (float)S(30), btn_h = stack ? (float)d.btns.size() * (bh + (float)S(8)) - (float)S(8) : bh;
    float fixed = pad * 2 + btn_h + (d.input ? (float)S(44) : 0.f) + (float)S(16);
    size_t max_lines = (size_t)std::max(3.f, ((float)E.H - (float)S(24) - fixed) / lh);
    while (tl.size() + dl.size() > max_lines && dl.size() > 1) dl.pop_back();
    float ph = fixed + (float)(tl.size() + dl.size()) * lh + (dl.empty() ? 0.f : (float)S(8));
    float px = ((float)E.W - pw) / 2, py = std::max((float)S(12), ((float)E.H - ph) / 3);
    r_rect(E.r, px - 1, py - 1, pw + 2, ph + 2, C_MENU_BORDER);
    r_rect(E.r, px, py, pw, ph, C_MENU);
    r_rect(E.r, px, py, pw, (float)S(2), C_ACCENT);
    float y = py + pad;
    draw_dot(px + pad + (float)S(9), y + lh / 2, S(30), C_GIT_MOD);   /* the warning mark */
    draw_text(E.ui, px + pad + (float)S(9) - text_width(E.ui, "!", FS_BOLD) / 2, ui_base(y, lh), "!", C_EDITOR, (float)S(12), FS_BOLD);
    float tx = px + pad + (float)S(30);
    for (const std::string &l : tl) { draw_text(E.ui, tx, ui_base(y, lh), l.c_str(), C_UI, iw - (float)S(30), FS_BOLD); y += lh; }
    if (!dl.empty()) y += (float)S(8);
    for (const std::string &l : dl) { draw_text(E.ui, tx, ui_base(y, lh), l.c_str(), C_UI_DIM, iw - (float)S(30)); y += lh; }
    if (d.input) {
        y += (float)S(8);
        r_rect(E.r, px + pad, y, iw, (float)S(30), C_ACCENT);
        r_rect(E.r, px + pad + 1, y + 1, iw - 2, (float)S(28), C_FIELD);
        float pen = px + pad + (float)S(8);
        if (d.text.empty()) draw_text(E.ui, pen, ui_base(y, (float)S(30)), d.placeholder.c_str(), C_UI_FAINT, iw - (float)S(16));
        else {
            std::string shown = d.mask ? std::string(d.text.size() > 40 ? 40 : d.text.size(), '*') : d.text;
            float p0 = pen;
            if (d.fresh) r_rect(E.r, p0, y + (float)S(4), std::min(iw - (float)S(16), text_width(E.ui, shown.c_str())), (float)S(22), C_SELROW);
            pen = draw_text(E.ui, pen, ui_base(y, (float)S(30)), shown.c_str(), C_UI, iw - (float)S(16));
        }
        if (E.blink_on) r_rect(E.r, pen + 1, y + (float)S(6), std::max(1.f, E.scale), (float)S(18), C_CURSOR);
        y += (float)S(36);
    }
    y = py + ph - pad - btn_h;
    float bx = px + pw - pad;
    if (stack) {
        for (size_t k = 0; k < d.btns.size(); k++) {
            bool f = (int)k == d.focus;
            r_rect(E.r, px + pad, y, iw, bh, f ? C_ACCENT : hovered(H_DLGBTN, (int)k) ? C_SELROW : C_HOVER);
            float lw = text_width(E.ui, d.btns[k].c_str());
            draw_text(E.ui, px + pad + (iw - lw) / 2, ui_base(y, bh), d.btns[k].c_str(), f ? C_ON_ACCENT : C_UI, iw);
            hit(px + pad, y, iw, bh, H_DLGBTN, (int)k);
            y += bh + (float)S(8);
        }
    } else {
        for (size_t k = d.btns.size(); k-- > 0;) {   /* right aligned, in the order given: the first is the primary one */
            bx -= bw[k];
            bool f = (int)k == d.focus;
            r_rect(E.r, bx, y, bw[k], bh, f ? C_ACCENT : hovered(H_DLGBTN, (int)k) ? C_SELROW : C_HOVER);
            draw_text(E.ui, bx + (float)S(15), ui_base(y, bh), d.btns[k].c_str(), f ? C_ON_ACCENT : C_UI, bw[k]);
            hit(bx, y, bw[k], bh, H_DLGBTN, (int)k);
            bx -= (float)S(8);
        }
    }
}

/* ---- source control actions: commit, discard, sync, branches, stash, tags (VS Code's wording and confirmations) ---- */

void pick_custom(const std::string &title, std::vector<Ed::PItem> items, std::function<void(const Ed::PItem &)> cb);
void settings_save();
void open_side(bool open);
bool new_terminal();

std::string trim_nl(std::string m) {
    while (!m.empty() && (m.back() == '\n' || m.back() == ' ' || m.back() == '\r')) m.pop_back();
    size_t b = 0;
    while (b < m.size() && (m[b] == '\n' || m[b] == ' ')) b++;
    return m.substr(b);
}

void git_error(const std::string &what, const std::string &out) {
    std::string d = trim_nl(out);
    if (d.size() > 700) d = d.substr(0, 700) + "...";
    dlg_show("Git: " + what + " failed", d.empty() ? "git reported an error without a message." : d, {"OK"}, nullptr);
}

/* an open tab whose file git just rewrote: re-read it unless it has unsaved edits */
void reload_open_docs() {
    for (size_t i = 0; i < E.docs.size(); i++) {
        Doc &o = *E.docs[i];
        if (o.dv || o.mv || o.path.empty() || o.modified()) continue;
        struct stat st;
        if (stat(o.path.c_str(), &st) != 0) continue;
        auto nd = std::make_unique<Doc>();
        if (!nd->open(o.path.c_str())) continue;
        nd->top = std::min(o.top, nd->buf.lines() ? nd->buf.lines() - 1 : 0);
        nd->cur = nd->anchor = std::min(o.cur, nd->buf.size());
        g_gutter.erase(&o);
        E.docs[i] = std::move(nd);
    }
    E.rows.clear();
}

bool git_sync_run(const std::string &what, const std::string &args, std::string *out = nullptr, bool show_error = true) {
    std::string o;
    int c = git_exec(E.git.root, args, &o);
    if (out) *out = o;
    if (c != 0 && show_error) git_error(what, o);
    return c == 0;
}

void job_start(GitJob &j, const std::string &label, std::function<int(std::string &)> fn, std::function<void(int, const std::string &)> cb);

void git_start(const std::string &label, const std::string &args, std::function<void(int, const std::string &)> cb) {
    if (g_job.running) { E.status_msg = "git is busy: " + g_job.label; redraw(); return; }
    askpass_start();
    std::string root = E.git.root;
    job_start(g_job, label, [root, args](std::string &out) { return git_exec(root, args, &out); }, [cb](int c, const std::string &o) { E.status_msg.clear(); if (cb) cb(c, o); });
    E.status_msg = label + "..."; redraw();
}

void git_job_poll();

std::string git_remote_name() {
    std::string o;
    git_exec(E.git.root, "remote", &o);
    return git_first_line(o);
}

void git_pull_flow(std::function<void()> then = nullptr) {
    begin_op("git pull");
    git_start("Pulling", "pull", [then](int c, const std::string &o) {
        git_refresh(); reload_open_docs();
        if (c != 0) git_error("pull", o); else { E.status_msg = "pulled"; if (then) then(); }
    });
}

void git_push_flow(std::function<void()> then = nullptr) {
    if (!E.git.has_remote) { dlg_show("Your repository has no remotes configured to push to.", "", {"OK"}, nullptr); return; }
    auto go = [then](const std::string &args, const char *label) {
        git_start(label, args, [then](int c, const std::string &o) {
            git_refresh();
            if (c == 0) { E.status_msg = "pushed"; if (then) then(); return; }
            if (o.find("rejected") != std::string::npos || o.find("non-fast-forward") != std::string::npos || o.find("fetch first") != std::string::npos)
                dlg_show("Can't push refs to remote. Try running \"Pull\" first to integrate your changes.", git_first_line(o), {"Pull", "Cancel"}, [](int i, const std::string &) { if (i == 0) git_pull_flow(); });
            else git_error("push", o);
        });
    };
    if (E.git.upstream.empty()) {
        std::string br = E.git.branch, rem = git_remote_name();
        dlg_show("The branch '" + br + "' has no remote branch. Would you like to publish this branch?", "", {"OK", "Cancel"},
                 [go, br, rem](int i, const std::string &) { if (i == 0) go("push -u " + git_quote(rem) + " " + git_quote(br), "Publishing"); });
    } else go("push", "Pushing");
}

void git_sync_flow() {
    if (E.git.upstream.empty()) { git_push_flow(); return; }
    auto run = [] { git_pull_flow([] { git_push_flow(); }); };
    if (!E.g.sync_confirm) { run(); return; }
    dlg_show("This action will pull and push commits from and to \"" + E.git.upstream + "\".", "", {"OK", "OK, Don't Show Again", "Cancel"}, [run](int i, const std::string &) {
        if (i == 1) { E.g.sync_confirm = false; settings_save(); }
        if (i == 0 || i == 1) run();
    });
}

void git_fetch_flow() {
    begin_op("git fetch");
    git_start("Fetching", "fetch --prune", [](int c, const std::string &o) { git_refresh(); if (c != 0) git_error("fetch", o); else E.status_msg = "fetched"; });
}

/* variant: 0 commit, 1 amend, 2 signed off, 3 commit and push, 4 commit and sync */
void git_commit_flow(int variant) {
    GitUi &g = E.g;
    std::string m = trim_nl(g.msg);
    bool amend = variant == 1;
    if (m.empty() && !amend) { g.err = "Please provide a commit message"; redraw(); return; }
    bool have_staged = false;
    for (const GitChange &c : E.git.changes) if (c.x != ' ' && c.x != '?') have_staged = true;
    auto go = [variant, m, amend](bool stage_all) {
        GitUi &g2 = E.g;
        begin_op("git commit");
        if (stage_all) git_stage_all(E.git.root);
        std::string out;
        std::string args = "commit" + std::string(amend ? " --amend" : "") + (variant == 2 ? " -s" : "") + (m.empty() ? " --no-edit" : " -m " + git_quote(m));
        if (!git_sync_run("commit", args, &out, false)) { g2.err = git_first_line(out); if (g2.err.empty()) g2.err = "commit failed"; git_refresh(); return; }
        g2.msg.clear(); g2.err.clear(); E.status_msg = "committed";
        git_refresh();
        if (variant == 3) git_push_flow();
        else if (variant == 4) git_sync_flow();
    };
    if (!have_staged && !amend) {
        if (E.git.changes.empty()) { g.err = "Nothing to commit"; redraw(); return; }
        if (g.smart_commit == 2) { g.err = "There are no staged changes to commit"; redraw(); return; }
        if (g.smart_commit == 0) {
            dlg_show("There are no staged changes to commit.\n\nWould you like to stage all your changes and commit them directly?", "", {"Yes", "Always", "Never"}, [go](int i, const std::string &) {
                if (i == 1) { E.g.smart_commit = 1; settings_save(); }
                if (i == 2) { E.g.smart_commit = 2; settings_save(); return; }
                if (i == 0 || i == 1) go(true);
            });
            return;
        }
        go(true);
        return;
    }
    go(false);
}

void git_commit_now() { git_commit_flow(0); }

void git_undo_last_commit() {
    std::string msg;
    git_exec(E.git.root, "log -1 --format=%B", &msg);
    std::string out;
    if (!git_sync_run("undo last commit", "reset --soft HEAD~1", &out, false)) {
        if (!git_sync_run("undo last commit", "update-ref -d HEAD", &out)) return;   /* the first commit has no parent */
    }
    if (trim_nl(E.g.msg).empty()) E.g.msg = trim_nl(msg);
    git_refresh();
}

/* Discard Changes, as VS Code words it. tracked paths are restored from the index; untracked ones are deleted. */
void do_discard(const std::vector<std::string> &tracked, const std::vector<std::string> &untracked) {
    begin_op("git discard", std::to_string(tracked.size() + untracked.size()) + " files");
    auto chunk = [&](const char *verb, const std::vector<std::string> &v) {
        for (size_t i = 0; i < v.size(); i += 80) {
            std::string a = verb;
            for (size_t k = i; k < v.size() && k < i + 80; k++) a += " " + git_quote(v[k]);
            std::string o;
            if (!git_sync_run("discard changes", a, &o)) return false;
        }
        return true;
    };
    if (!tracked.empty() && !chunk("restore --", tracked)) { git_refresh(); return; }
    if (!untracked.empty() && !chunk("clean -fdq --", untracked)) { git_refresh(); return; }
    git_refresh();
    reload_open_docs();
    E.status_msg = "discarded changes";
}

void discard_files(const std::vector<int> &idxs) {   /* indexes into E.git.changes, all from the Changes group */
    std::vector<std::string> tr, un, all_del;
    int deleted = 0;
    for (int i : idxs) {
        if ((size_t)i >= E.git.changes.size()) continue;
        const GitChange &c = E.git.changes[(size_t)i];
        if (c.x == '?' && c.y == '?') un.push_back(c.path); else { tr.push_back(c.path); if (c.y == 'D') deleted++; }
    }
    size_t n = tr.size() + un.size();
    if (!n) return;
    std::string title, detail, yes;
    if (n == 1) {
        std::string nm = base_name(un.empty() ? tr[0] : un[0]);
        if (!un.empty()) { title = "Are you sure you want to DELETE '" + nm + "'?"; detail = "This is IRREVERSIBLE!\nThis file will be FOREVER LOST if you proceed."; yes = "Delete File"; }
        else if (deleted) { title = "Are you sure you want to restore '" + nm + "'?"; yes = "Restore File"; }
        else { title = "Are you sure you want to discard changes in '" + nm + "'?"; detail = "This action is irreversible!"; yes = "Discard 1 File"; }
    } else {
        title = "Are you sure you want to discard " + std::to_string(n) + " files?";
        detail = "This is IRREVERSIBLE!\nYour current working set will be FOREVER LOST.";
        if (!un.empty()) detail += "\n\nThis will DELETE " + std::to_string(un.size()) + " untracked file" + (un.size() == 1 ? "" : "s") + "!";
        yes = "Discard " + std::to_string(n) + " Files";
    }
    dlg_show(title, detail, {yes, "Cancel"}, [tr, un](int i, const std::string &) { if (i == 0) do_discard(tr, un); });
}

void discard_all_changes() {
    std::vector<int> idx;
    for (size_t i = 0; i < E.git.changes.size(); i++) { const GitChange &c = E.git.changes[i]; if ((c.x == '?' && c.y == '?') || (c.y != ' ' && c.y != '?')) idx.push_back((int)i); }
    if (idx.empty()) { E.status_msg = "There are no changes to discard"; redraw(); return; }
    if (idx.size() == 1) { discard_files(idx); return; }
    std::vector<std::string> tr, un;
    for (int i : idx) { const GitChange &c = E.git.changes[(size_t)i]; (c.x == '?' && c.y == '?' ? un : tr).push_back(c.path); }
    size_t n = idx.size();
    std::string detail = "This is IRREVERSIBLE!\nYour current working set will be FOREVER LOST.";
    std::vector<std::string> btns = {"Discard All " + std::to_string(n) + " Files"};
    if (!un.empty()) { detail += "\n\nThis will DELETE " + std::to_string(un.size()) + " untracked file" + (un.size() == 1 ? "" : "s") + "!"; btns.push_back("Discard Tracked Changes"); }
    btns.push_back("Cancel");
    dlg_show("Are you sure you want to discard ALL changes in " + std::to_string(n) + " files?", detail, btns, [tr, un](int i, const std::string &) {
        if (i == 0) do_discard(tr, un);
        else if (i == 1 && !un.empty()) do_discard(tr, {});
    });
}

void git_add_ignore(const std::string &path) {
    std::string f = E.git.root + "/.gitignore";
    FILE *fp = fopen(f.c_str(), "a");
    if (!fp) { E.status_msg = "cannot write .gitignore"; return; }
    fprintf(fp, "/%s\n", path.c_str());
    fclose(fp);
    git_refresh();
}

/* ---- branches ---- */
std::vector<Ed::PItem> branch_items(bool include_remote, bool skip_current) {
    std::string o;
    git_exec(E.git.root, "for-each-ref --format=%(HEAD)%(refname) refs/heads" + std::string(include_remote ? " refs/remotes" : ""), &o);
    std::vector<Ed::PItem> v;
    size_t pos = 0;
    while (pos < o.size()) {
        size_t e = o.find('\n', pos);
        if (e == std::string::npos) e = o.size();
        std::string l = o.substr(pos, e - pos);
        pos = e + 1;
        if (l.size() < 2) continue;
        bool cur = l[0] == '*';
        std::string ref = l.substr(1);
        bool rem = ref.compare(0, 13, "refs/remotes/") == 0;
        std::string name = ref.substr(rem ? 13 : 11);
        if (rem && name.size() >= 5 && name.compare(name.size() - 5, 5, "/HEAD") == 0) continue;
        if (cur && skip_current) continue;
        v.push_back({name, rem ? 1 : 0, cur});
    }
    return v;
}

void git_checkout(const std::string &ref, bool remote) {
    std::string o;
    std::string args = remote ? "checkout --track " + git_quote(ref) : "checkout " + git_quote(ref);
    if (git_exec(E.git.root, args, &o) == 0) { git_refresh(); reload_open_docs(); E.status_msg = "checked out " + ref; return; }
    if (o.find("overwritten") != std::string::npos || o.find("commit your changes or stash") != std::string::npos) {
        dlg_show("Your local changes would be overwritten by checkout.", "Please clean your repository working tree before checkout.", {"Stash & Checkout", "Force Checkout", "Cancel"}, [ref, remote](int i, const std::string &) {
            std::string o2;
            if (i == 0) { if (!git_sync_run("stash", "stash push --include-untracked", &o2)) return; }
            else if (i == 1) { if (!git_sync_run("checkout", std::string("checkout -f ") + (remote ? "--track " : "") + git_quote(ref), &o2)) return; git_refresh(); reload_open_docs(); return; }
            else return;
            if (git_sync_run("checkout", std::string("checkout ") + (remote ? "--track " : "") + git_quote(ref), &o2)) { git_refresh(); reload_open_docs(); }
        });
        return;
    }
    git_error("checkout", o);
}

std::string branch_clean(std::string n) { for (char &c : n) if (c == ' ') c = '-'; return n; }

void git_create_branch(const std::string &from) {
    dlg_show("Create Branch", from.empty() ? "Please provide a new branch name" : "Please provide a new branch name (from " + from + ")", {"OK", "Cancel"}, [from](int i, const std::string &t) {
        std::string n = branch_clean(trim_nl(t));
        if (i != 0 || n.empty()) return;
        std::string o;
        if (git_sync_run("create branch", "checkout -b " + git_quote(n) + (from.empty() ? "" : " " + git_quote(from)), &o)) { git_refresh(); E.status_msg = "created branch " + n; }
    }, true, "", "Branch name");
}

void git_branch_picker() {   /* the status bar branch / the "Checkout to..." command */
    std::vector<Ed::PItem> items = {{"+ Create new branch...", -1, false}, {"+ Create new branch from...", -2, false}};
    for (auto &b : branch_items(true, false)) items.push_back({b.label, b.id, b.current});
    pick_custom("Select a branch or tag to checkout", items, [](const Ed::PItem &it) {
        if (it.id == -1) git_create_branch("");
        else if (it.id == -2) pick_custom("Select a ref to create the branch from", [] { std::vector<Ed::PItem> v; for (auto &b : branch_items(true, false)) v.push_back({b.label, b.id, false}); return v; }(), [](const Ed::PItem &r) { git_create_branch(r.label); });
        else if (!it.current) git_checkout(it.label, it.id == 1);
    });
}

void git_branch_ops(int which) {   /* 0 delete, 1 rename, 2 merge */
    if (which == 1) {
        dlg_show("Rename Branch", "Please provide a new branch name", {"OK", "Cancel"}, [](int i, const std::string &t) {
            std::string n = branch_clean(trim_nl(t));
            if (i == 0 && !n.empty() && git_sync_run("rename branch", "branch -m " + git_quote(n))) git_refresh();
        }, true, E.git.branch, "Branch name");
        return;
    }
    auto items = branch_items(false, true);
    if (items.empty()) { E.status_msg = "no other branches"; redraw(); return; }
    pick_custom(which == 0 ? "Select a branch to delete" : "Select a branch to merge from", items, [which](const Ed::PItem &it) {
        std::string name = it.label, o;
        if (which == 2) { if (git_sync_run("merge", "merge " + git_quote(name))) { git_refresh(); reload_open_docs(); E.status_msg = "merged " + name; } return; }
        if (git_exec(E.git.root, "branch -d " + git_quote(name), &o) == 0) { git_refresh(); return; }
        if (o.find("not fully merged") != std::string::npos)
            dlg_show("The branch '" + name + "' is not fully merged. Delete anyway?", "", {"Delete Branch", "Cancel"}, [name](int i, const std::string &) { if (i == 0 && git_sync_run("delete branch", "branch -D " + git_quote(name))) git_refresh(); });
        else git_error("delete branch", o);
    });
}

/* ---- remotes, pull variants and rebase ---- */
std::vector<std::string> git_remotes() {
    std::string o;
    git_exec(E.git.root, "remote", &o);
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos < o.size()) { size_t e = o.find('\n', pos); if (e == std::string::npos) e = o.size(); if (e > pos) v.push_back(o.substr(pos, e - pos)); pos = e + 1; }
    return v;
}

void git_pick_remote(const std::string &title, std::function<void(const std::string &)> then) {
    std::vector<std::string> rs = git_remotes();
    if (rs.empty()) { dlg_show("Your repository has no remotes configured.", "", {"OK"}, nullptr); return; }
    if (rs.size() == 1) { then(rs[0]); return; }
    std::vector<Ed::PItem> items;
    for (const std::string &r : rs) items.push_back({r, 0, false});
    pick_custom(title, items, [then](const Ed::PItem &it) { then(it.label); });
}

void git_pull_rebase() {
    begin_op("git pull --rebase");
    git_start("Pulling (rebase)", "pull --rebase", [](int c, const std::string &o) {
        git_refresh(); reload_open_docs();
        if (c != 0 && !E.git.op.empty()) E.status_msg = "rebase stopped: resolve the conflicts, stage the files, then Continue";
        else if (c != 0) git_error("pull --rebase", o); else E.status_msg = "pulled (rebase)";
    });
}

void git_pull_from() {
    git_pick_remote("Select a remote to pull from", [](const std::string &rem) {
        std::vector<Ed::PItem> items;
        for (auto &b : branch_items(true, false)) if (b.id == 1 && b.label.compare(0, rem.size() + 1, rem + "/") == 0) items.push_back({b.label.substr(rem.size() + 1), 0, false});
        if (items.empty()) { dlg_show("No branches known for '" + rem + "'. Fetch first.", "", {"OK"}, nullptr); return; }
        pick_custom("Select a branch to pull from " + rem, items, [rem](const Ed::PItem &it) {
            begin_op("git pull");
            git_start("Pulling", "pull " + git_quote(rem) + " " + git_quote(it.label), [](int c, const std::string &o) { git_refresh(); reload_open_docs(); if (c != 0) git_error("pull", o); else E.status_msg = "pulled"; });
        });
    });
}

void git_push_to() {
    git_pick_remote("Select a remote to push to", [](const std::string &rem) {
        begin_op("git push");
        git_start("Pushing", "push -u " + git_quote(rem) + " " + git_quote(E.git.branch), [](int c, const std::string &o) { git_refresh(); if (c != 0) git_error("push", o); else E.status_msg = "pushed"; });
    });
}

void git_rebase_onto() {
    auto items = branch_items(true, true);
    if (items.empty()) { E.status_msg = "no other branches"; redraw(); return; }
    pick_custom("Select a branch to rebase '" + E.git.branch + "' onto", items, [](const Ed::PItem &it) {
        std::string name = it.label;
        dlg_show("Rebase '" + E.git.branch + "' onto '" + name + "'?", "Your commits are replayed on top of it. Stop with Abort if it goes wrong.", {"Rebase", "Cancel"}, [name](int i, const std::string &) {
            if (i != 0) return;
            begin_op("git rebase");
            git_start("Rebasing", "rebase " + git_quote(name), [name](int c, const std::string &o) {
                git_refresh(); reload_open_docs();
                if (c != 0 && !E.git.op.empty()) E.status_msg = "rebase stopped: resolve the conflicts, stage the files, then Continue";
                else if (c != 0) git_error("rebase", o); else E.status_msg = "rebased onto " + name;
            });
        });
    });
}

void git_remote_add() {
    dlg_show("Add Remote", "Remote name", {"Next", "Cancel"}, [](int i, const std::string &t) {
        std::string n = trim_nl(t);
        if (i != 0 || n.empty()) return;
        if (n.find_first_of(" \t/") != std::string::npos) { dlg_show("Not a valid remote name", "A remote name cannot contain spaces or slashes.", {"OK"}, nullptr); return; }
        dlg_show("Add Remote", "URL of '" + n + "'", {"Add", "Cancel"}, [n](int i2, const std::string &t2) {
            std::string u = trim_nl(t2);
            if (i2 == 0 && !u.empty() && git_sync_run("add remote", "remote add " + git_quote(n) + " " + git_quote(u))) { git_refresh(); E.status_msg = "added remote " + n; }
        }, true, "", "https://... or git@host:path.git");
    }, true, "", "origin");
}

void git_remote_remove() {
    std::vector<std::string> rs = git_remotes();
    if (rs.empty()) { dlg_show("Your repository has no remotes configured.", "", {"OK"}, nullptr); return; }
    std::vector<Ed::PItem> items;
    for (const std::string &r : rs) items.push_back({r, 0, false});
    pick_custom("Select a remote to remove", items, [](const Ed::PItem &it) {
        std::string n = it.label;
        dlg_show("Remove the remote '" + n + "'?", "Its remote-tracking branches are removed too. Nothing changes on the server.", {"Remove Remote", "Cancel"}, [n](int i, const std::string &) {
            if (i == 0 && git_sync_run("remove remote", "remote remove " + git_quote(n))) { git_refresh(); E.status_msg = "removed remote " + n; }
        });
    });
}

/* continue, skip or abort an unfinished merge, rebase, cherry-pick or revert: 0 continue, 1 skip, 2 abort */
void git_op_do(int what) {
    const std::string op = E.git.op;
    if (op.empty()) return;
    std::string args;
    if (what == 0) args = op == "merge" ? "commit --no-edit" : "-c core.editor=true " + op + " --continue";
    else if (what == 1) args = op + " --skip";
    else args = op + " --abort";
    if (what == 1 && op != "rebase" && op != "cherry-pick") return;
    auto go = [op, args, what] {
        begin_op("git " + op);
        git_start(what == 0 ? "Continuing" : what == 1 ? "Skipping" : "Aborting", args, [op, what](int c, const std::string &o) {
            git_refresh(); reload_open_docs();
            if (c != 0) git_error(op + (what == 0 ? " --continue" : what == 1 ? " --skip" : " --abort"), o);
            else E.status_msg = what == 2 ? op + " aborted" : E.git.op.empty() ? op + " finished" : op + " continues: more conflicts to resolve";
        });
    };
    if (what == 2) dlg_show("Abort the " + op + "?", "Everything done since it started is thrown away.", {"Abort " + op, "Cancel"}, [go](int i, const std::string &) { if (i == 0) go(); });
    else go();
}

/* ---- stash and tags ---- */
void git_stash_push(bool untracked) {
    if (E.git.changes.empty()) { dlg_show("There are no changes to stash.", "", {"OK"}, nullptr); return; }
    dlg_show("Stash", "Optionally provide a stash message", {"OK", "Cancel"}, [untracked](int i, const std::string &t) {
        if (i != 0) return;
        std::string m = trim_nl(t);
        if (git_sync_run("stash", std::string("stash push") + (untracked ? " --include-untracked" : "") + (m.empty() ? "" : " -m " + git_quote(m)))) { git_refresh(); reload_open_docs(); E.status_msg = "stashed"; }
    }, true, "", "Stash message");
}

std::vector<Ed::PItem> stash_items() {
    std::string o;
    git_exec(E.git.root, "stash list", &o);
    std::vector<Ed::PItem> v;
    size_t pos = 0;
    int n = 0;
    while (pos < o.size()) { size_t e = o.find('\n', pos); if (e == std::string::npos) e = o.size(); if (e > pos) v.push_back({o.substr(pos, e - pos), n++, false}); pos = e + 1; }
    return v;
}

void git_stash_op(const char *verb, bool latest) {   /* apply, pop, drop */
    auto items = stash_items();
    if (items.empty()) { dlg_show("There are no stashes in the repository.", "", {"OK"}, nullptr); return; }
    std::string v = verb;
    auto go = [v](int n) {
        std::string ref = "stash@{" + std::to_string(n) + "}", o;
        if (git_exec(E.git.root, "stash " + v + " " + git_quote(ref), &o) != 0) { git_error("stash " + v, o); git_refresh(); return; }
        git_refresh(); reload_open_docs();
    };
    if (latest) go(0); else pick_custom("Pick a stash to " + v, items, [go](const Ed::PItem &it) { go(it.id); });
}

void git_stash_drop_all() {
    dlg_show("Are you sure you want to drop ALL stashes?", "This is IRREVERSIBLE!\nYour stashes will be FOREVER LOST.", {"Drop All Stashes", "Cancel"}, [](int i, const std::string &) { if (i == 0 && git_sync_run("stash", "stash clear")) git_refresh(); });
}

void git_tag_create(const std::string &at) {
    dlg_show("Create Tag", "Tag name", {"OK", "Cancel"}, [at](int i, const std::string &t) {
        std::string n = branch_clean(trim_nl(t));
        if (i == 0 && !n.empty() && git_sync_run("tag", "tag " + git_quote(n) + (at.empty() ? "" : " " + git_quote(at)))) { git_refresh(); E.status_msg = "created tag " + n; }
    }, true, "", "Tag name");
}

void git_tag_delete() {
    std::string o;
    git_exec(E.git.root, "tag", &o);
    std::vector<Ed::PItem> v;
    size_t pos = 0;
    while (pos < o.size()) { size_t e = o.find('\n', pos); if (e == std::string::npos) e = o.size(); if (e > pos) v.push_back({o.substr(pos, e - pos), 0, false}); pos = e + 1; }
    if (v.empty()) { dlg_show("This repository has no tags.", "", {"OK"}, nullptr); return; }
    pick_custom("Select a tag to delete", v, [](const Ed::PItem &it) { if (git_sync_run("delete tag", "tag -d " + git_quote(it.label))) git_refresh(); });
}

/* ---- the menus ---- */
void git_more_menu(float x, float y) {
    GitUi &g = E.g;
    bool ch = !E.git.changes.empty();
    std::vector<CtxItem> commit = {ci("Commit", [] { git_commit_flow(0); }), ci("Commit (Amend)", [] { git_commit_flow(1); }), ci("Commit (Signed Off)", [] { git_commit_flow(2); }), ci_sep(),
                                   ci("Commit and Push", [] { git_commit_flow(3); }), ci("Commit and Sync", [] { git_commit_flow(4); }), ci_sep(), ci("Undo Last Commit", [] { git_undo_last_commit(); })};
    std::vector<CtxItem> changes = {ci("Stage All Changes", [] { git_stage_all(E.git.root); git_refresh(); }, "", ch), ci("Unstage All Changes", [] { git_unstage(E.git.root, "."); git_refresh(); }, "", ch),
                                    ci("Discard All Changes", [] { discard_all_changes(); }, "", ch)};
    std::vector<CtxItem> branch = {ci("Merge...", [] { git_branch_ops(2); }), ci("Rebase Branch...", [] { git_rebase_onto(); }), ci_sep(), ci("Create Branch...", [] { git_create_branch(""); }), ci("Create Branch From...", [] {
                                       pick_custom("Select a ref to create the branch from", [] { std::vector<Ed::PItem> v; for (auto &b : branch_items(true, false)) v.push_back({b.label, b.id, false}); return v; }(), [](const Ed::PItem &r) { git_create_branch(r.label); }); }),
                                   ci_sep(), ci("Rename Branch...", [] { git_branch_ops(1); }), ci("Delete Branch...", [] { git_branch_ops(0); }), ci_sep(), ci("Publish Branch...", [] { git_push_flow(); }, "", E.git.upstream.empty())};
    std::vector<CtxItem> stash = {ci("Stash", [] { git_stash_push(false); }), ci("Stash (Include Untracked)", [] { git_stash_push(true); }), ci_sep(), ci("Apply Latest Stash", [] { git_stash_op("apply", true); }),
                                  ci("Apply Stash...", [] { git_stash_op("apply", false); }), ci_sep(), ci("Pop Latest Stash", [] { git_stash_op("pop", true); }), ci("Pop Stash...", [] { git_stash_op("pop", false); }), ci_sep(),
                                  ci("Drop Stash...", [] { git_stash_op("drop", false); }), ci("Drop All Stashes...", [] { git_stash_drop_all(); })};
    std::vector<CtxItem> tags = {ci("Create Tag...", [] { git_tag_create(""); }), ci("Delete Tag...", [] { git_tag_delete(); })};
    std::vector<CtxItem> remote = {ci("Add Remote...", [] { git_remote_add(); }), ci("Remove Remote...", [] { git_remote_remove(); }, "", E.git.has_remote), ci_sep(), ci("Fetch From All Remotes", [] { begin_op("git fetch --all"); git_start("Fetching", "fetch --all --prune", [](int c, const std::string &o) { git_refresh(); if (c != 0) git_error("fetch", o); else E.status_msg = "fetched"; }); }, "", E.git.has_remote)};
    std::vector<CtxItem> pull = {ci("Pull", [] { git_pull_flow(); }), ci("Pull (Rebase)", [] { git_pull_rebase(); }), ci("Pull from...", [] { git_pull_from(); })};
    std::vector<CtxItem> push = {ci("Push", [] { git_push_flow(); }), ci("Push to...", [] { git_push_to(); })};
    std::vector<CtxItem> items = {ci_sub("Pull", pull), ci_sub("Push", push), ci("Fetch", [] { git_fetch_flow(); }), ci("Sync", [] { git_sync_flow(); }), ci_sep(),
                                  ci("Checkout to...", [] { git_branch_picker(); }), ci_sep(), ci_sub("Commit", commit), ci_sub("Changes", changes), ci_sub("Branch", branch), ci_sub("Remote", remote), ci_sub("Stash", stash), ci_sub("Tags", tags), ci_sep(),
                                  ci(g.tree_view ? "View as List" : "View as Tree", [] { E.g.tree_view = !E.g.tree_view; settings_save(); redraw(); })};
    ctx_show(x, y, std::move(items));
}

void git_commit_menu(float x, float y) {   /* the arrow on the Commit button */
    ctx_show(x, y, {ci("Commit", [] { git_commit_flow(0); }), ci("Commit (Amend)", [] { git_commit_flow(1); }), ci("Commit (Signed Off)", [] { git_commit_flow(2); }), ci_sep(),
                    ci("Commit and Push", [] { git_commit_flow(3); }), ci("Commit and Sync", [] { git_commit_flow(4); }), ci_sep(), ci("Undo Last Commit", [] { git_undo_last_commit(); })});
}

/* ---- the Remote tab: saved connections (ssh, sftp, ftp, ftps, smb), their folders, and files edited in place ---- */

struct RConn {
    bool connected = false, busy = false, open = true;
    std::string pw, err;                                         /* the password lives in memory for this session only */
    std::map<std::string, std::vector<RemoteEntry>> dirs;
    std::set<std::string> expanded, loading;
};
struct RemoteUi {
    std::vector<RemoteProfile> profiles;
    std::vector<RConn> conns;
    bool loaded = false;
    struct Opened { int prof; std::string remote; long mtime; };
    std::map<std::string, Opened> files;                         /* local copy -> where it came from, to upload on save */
};
RemoteUi g_rem;
GitJob g_rjob;
struct RRow { int prof; std::string path; int kind; };          /* kind: 0 connection, 1 folder, 2 file, 3 loading */
std::vector<RRow> g_rrows;

void remote_ensure() {
    if (g_rem.loaded) return;
    g_rem.loaded = true;
    g_rem.profiles = remote_load();
    g_rem.conns.assign(g_rem.profiles.size(), RConn());
}

std::string remote_root(const RemoteProfile &p) { return p.path.empty() ? (p.proto == "ssh" || p.proto == "sftp" ? "." : "/") : p.path; }

void job_start(GitJob &j, const std::string &label, std::function<int(std::string &)> fn, std::function<void(int, const std::string &)> cb) {
    if (j.th.joinable()) j.th.join();
    j.running = true; j.done = false; j.label = label; j.cb = std::move(cb); j.out.clear();
    j.th = std::thread([&j, fn] { std::string o; int c = fn(o); j.out = o; j.code = c; j.done = true; app_wake(); });
}

void job_poll(GitJob &j) {
    if (!j.running || !j.done) return;
    j.th.join();
    j.running = false;
    auto cb = std::move(j.cb);
    int c = j.code;
    std::string o = j.out;
    if (cb) cb(c, o);
    redraw();
}

std::string remote_cache_path(const RemoteProfile &p, const std::string &remote) {
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    std::string d = x && *x ? x : std::string(h ? h : ".") + "/.cache";
    mkdir(d.c_str(), 0755);
    d += "/hootedit";
    mkdir(d.c_str(), 0755);
    d += "/remote";
    mkdir(d.c_str(), 0755);
    std::string nm = p.name;
    for (char &c : nm) if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
    d += "/" + nm;
    mkdir(d.c_str(), 0700);
    std::string rel = remote;
    while (!rel.empty() && (rel[0] == '/' || rel[0] == '.')) rel.erase(0, 1);
    size_t pos = 0;
    while ((pos = rel.find('/', pos)) != std::string::npos) { mkdir((d + "/" + rel.substr(0, pos)).c_str(), 0700); pos++; }
    return d + "/" + rel;
}

void remote_error(const std::string &what, const std::string &msg) { dlg_show(what, msg, {"OK"}, nullptr); }

void remote_list_dir(int pi, const std::string &path, bool then_open = true) {
    if (pi < 0 || (size_t)pi >= g_rem.conns.size()) return;
    RConn &c = g_rem.conns[(size_t)pi];
    if (c.loading.count(path) || g_rjob.running) return;
    c.loading.insert(path);
    if (then_open) c.expanded.insert(path);
    RemoteProfile p = g_rem.profiles[(size_t)pi];
    std::string pw = c.pw;
    auto res = std::make_shared<std::vector<RemoteEntry>>();
    job_start(g_rjob, "Listing " + p.name, [p, pw, path, res](std::string &err) { return remote_list(p, pw, path, *res, &err); }, [pi, path, res](int code, const std::string &err) {
        if (pi >= (int)g_rem.conns.size()) return;
        RConn &c2 = g_rem.conns[(size_t)pi];
        c2.loading.erase(path);
        if (code != 0) { c2.expanded.erase(path); remote_error("Cannot list " + path, err); return; }
        std::sort(res->begin(), res->end(), [](const RemoteEntry &a, const RemoteEntry &b) { return a.dir != b.dir ? a.dir : a.name < b.name; });
        c2.dirs[path] = *res;
    });
}

std::string remote_parent(const std::string &path) {
    size_t sl = path.rfind('/');
    return sl == std::string::npos ? "." : sl == 0 ? "/" : path.substr(0, sl);
}
std::string remote_join(const std::string &dir, const std::string &name) { return dir == "." || dir.empty() ? name : dir.back() == '/' ? dir + name : dir + "/" + name; }

/* run one change on the server in the background, then list the folder it touched again */
void remote_change(int pi, RemoteOp op, const std::string &path, const std::string &to, const std::string &done) {
    if (pi < 0 || (size_t)pi >= g_rem.conns.size()) return;
    if (g_rjob.running) { E.status_msg = "another remote operation is running"; return; }
    RemoteProfile p = g_rem.profiles[(size_t)pi];
    std::string pw = g_rem.conns[(size_t)pi].pw;
    job_start(g_rjob, "Working on " + p.name, [p, pw, op, path, to](std::string &err) { return remote_op(p, pw, op, path, to, &err); }, [pi, op, path, to, done](int code, const std::string &err) {
        if (code != 0) { remote_error("The server could not complete this", err); return; }
        if (pi >= (int)g_rem.conns.size()) return;
        RConn &c = g_rem.conns[(size_t)pi];
        for (auto &f : g_rem.files) if (f.second.prof == pi && op == ROP_RENAME && f.second.remote == path) f.second.remote = to;   /* an open tab keeps saving to the new name */
        for (auto it = c.dirs.begin(); it != c.dirs.end();) { if (op == ROP_DELETE || op == ROP_RMDIR || op == ROP_RENAME) { if (it->first == path || it->first.compare(0, path.size() + 1, path + "/") == 0) { c.expanded.erase(it->first); it = c.dirs.erase(it); continue; } } ++it; }
        std::string par = remote_parent(op == ROP_RENAME ? to : path);
        c.dirs.erase(par);
        if (op == ROP_RENAME && remote_parent(path) != par) c.dirs.erase(remote_parent(path));
        E.status_msg = done;
        remote_list_dir(pi, par, false);
    });
}

void remote_ask_name(int pi, const std::string &dir, RemoteOp op) {
    bool dirq = op == ROP_MKDIR;
    dlg_show(dirq ? "New folder in " + dir : "New file in " + dir, "Created on the server right away.", {"Create", "Cancel"}, [pi, dir, op, dirq](int i, const std::string &t) {
        std::string n = trim_nl(t);
        if (i != 0 || n.empty()) return;
        if (n.find('/') != std::string::npos) { remote_error("Not a valid name", "A name cannot contain a slash."); return; }
        remote_change(pi, op, remote_join(dir, n), "", dirq ? "folder created" : "file created");
    }, true, "", dirq ? "folder name" : "file name");
}

void remote_ask_rename(int pi, const std::string &path, bool is_dir) {
    std::string cur = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    dlg_show("Rename " + cur, "The new name on the server.", {"Rename", "Cancel"}, [pi, path, is_dir](int i, const std::string &t) {
        std::string n = trim_nl(t);
        if (i != 0 || n.empty()) return;
        if (n.find('/') != std::string::npos) { remote_error("Not a valid name", "A name cannot contain a slash."); return; }
        remote_change(pi, ROP_RENAME, path, remote_join(remote_parent(path), n), is_dir ? "folder renamed" : "file renamed");
    }, true, cur, "new name");
}

void remote_ask_delete(int pi, const std::string &path, bool is_dir) {
    std::string cur = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
    dlg_show(is_dir ? "Are you sure you want to delete the folder '" + cur + "' and its contents?" : "Are you sure you want to delete '" + cur + "'?", "It is removed on the server. This action is irreversible!", {"Delete", "Cancel"}, [pi, path, is_dir](int i, const std::string &) {
        if (i == 0) remote_change(pi, is_dir ? ROP_RMDIR : ROP_DELETE, path, "", is_dir ? "folder deleted" : "file deleted");
    });
}

void remote_open_file(int pi, const std::string &path, int line = 0);
void goto_line(const std::string &q);

void remote_search(int pi, const std::string &dir) {
    if (pi < 0 || (size_t)pi >= g_rem.conns.size()) return;
    dlg_show("Search in " + dir, "Text to find in the files below this folder, searched on the server with grep (case is ignored unless you type a capital).", {"Search", "Cancel"}, [pi, dir](int i, const std::string &t) {
        std::string pat = trim_nl(t);
        if (i != 0 || pat.empty() || g_rjob.running) return;
        RemoteProfile p = g_rem.profiles[(size_t)pi];
        auto res = std::make_shared<std::vector<RemoteHit>>();
        job_start(g_rjob, "Searching " + p.name, [p, dir, pat, res](std::string &err) { return remote_grep(p, dir, pat, *res, &err); }, [pi, dir, res](int code, const std::string &err) {
            if (code != 0) { remote_error("The search failed", err); return; }
            if (res->empty()) { E.status_msg = "no matches"; return; }
            std::vector<Ed::PItem> items;
            for (size_t k = 0; k < res->size(); k++) { const RemoteHit &h = (*res)[k]; std::string rel = h.path.compare(0, dir.size() + 1, dir + "/") == 0 ? h.path.substr(dir.size() + 1) : h.path; items.push_back({rel + ":" + std::to_string(h.line) + "    " + h.text, (int)k, false}); }
            pick_custom(std::to_string(res->size()) + (res->size() >= 300 ? "+" : "") + " matches", items, [pi, res](const Ed::PItem &it) {
                const RemoteHit &h = (*res)[(size_t)it.id];
                remote_open_file(pi, h.path, h.line);
            });
        });
    }, true, "", "text");
}

void remote_connect_now(int pi) {
    if (pi < 0 || (size_t)pi >= g_rem.conns.size()) return;
    RConn &c = g_rem.conns[(size_t)pi];
    if (c.busy || g_rjob.running) { E.status_msg = "another remote operation is running"; return; }
    RemoteProfile p = g_rem.profiles[(size_t)pi];
    std::string missing;
    if (!remote_tools(p, &missing)) { remote_error("'" + missing + "' is not installed", "Hootedit uses the " + missing + " command to talk to " + p.proto + " servers. Install it and try again."); return; }
    if (!askpass_start() && (p.proto == "ssh" || p.proto == "sftp")) E.status_msg = "password prompts are unavailable";
    c.busy = true; c.err.clear();
    std::string pw = c.pw, root = remote_root(p);
    auto res = std::make_shared<std::vector<RemoteEntry>>();
    job_start(g_rjob, "Connecting to " + p.name, [p, pw, root, res](std::string &err) {
        int rc = 0;
        if (p.proto == "ssh" || p.proto == "sftp") rc = remote_connect(p, pw, &err);
        if (rc == 0) rc = remote_list(p, pw, root, *res, &err);   /* ftp and smb have no session: the first listing is the login check */
        return rc;
    }, [pi, root, res, p](int code, const std::string &err) {
        if (pi >= (int)g_rem.conns.size()) return;
        RConn &c2 = g_rem.conns[(size_t)pi];
        c2.busy = false;
        if (code != 0) { c2.connected = false; c2.pw.clear(); c2.err = err; remote_error("Cannot connect to " + p.name, err); return; }
        std::sort(res->begin(), res->end(), [](const RemoteEntry &a, const RemoteEntry &b) { return a.dir != b.dir ? a.dir : a.name < b.name; });
        c2.connected = true; c2.open = true;
        c2.dirs[root] = *res;
        E.status_msg = "connected to " + p.name;
    });
}

void remote_connect(int pi) {
    remote_ensure();
    if (pi < 0 || (size_t)pi >= g_rem.profiles.size()) return;
    const RemoteProfile &p = g_rem.profiles[(size_t)pi];
    if (remote_needs_password(p) && g_rem.conns[(size_t)pi].pw.empty() && !p.user.empty() && p.user != "anonymous") {
        dlg_show("Password for " + remote_label(p), "Used for this session only. Hootedit does not store passwords.", {"Connect", "Cancel"}, [pi](int i, const std::string &t) {
            if (i != 0 || (size_t)pi >= g_rem.conns.size()) return;
            g_rem.conns[(size_t)pi].pw = t;
            remote_connect_now(pi);
        }, true, "", "Password", true);
        return;
    }
    remote_connect_now(pi);
}

void remote_disconnect_now(int pi) {
    if (pi < 0 || (size_t)pi >= g_rem.conns.size()) return;
    RemoteProfile p = g_rem.profiles[(size_t)pi];
    RConn &c = g_rem.conns[(size_t)pi];
    c.connected = false; c.dirs.clear(); c.expanded.clear(); c.pw.clear();
    std::thread([p] { remote_disconnect(p); }).detach();
    E.status_msg = "disconnected from " + p.name;
}

void remote_open_file(int pi, const std::string &path, int line) {
    if (pi < 0 || (size_t)pi >= g_rem.conns.size() || g_rjob.running) return;
    RemoteProfile p = g_rem.profiles[(size_t)pi];
    std::string pw = g_rem.conns[(size_t)pi].pw, local = remote_cache_path(p, path);
    job_start(g_rjob, "Downloading " + base_name(path), [p, pw, path, local](std::string &err) { return remote_get(p, pw, path, local, &err); }, [pi, path, local, line](int code, const std::string &err) {
        if (code != 0) { remote_error("Cannot open " + base_name(path), err); return; }
        struct stat st;
        long mt = stat(local.c_str(), &st) == 0 ? (long)st.st_mtime : 0;
        g_rem.files[local] = {pi, path, mt};
        if (!E.pinned) open_side(false);
        open_path(local);
        if (line > 0) goto_line(std::to_string(line));
        E.status_msg = "opened " + path + " (saving uploads it)";
    });
}

/* after a save: any open tab that came from a server and changed on disk is uploaded again */
void remote_push_changed() {
    if (g_rem.files.empty() || g_rjob.running) return;
    for (auto &dp : E.docs) {
        auto it = g_rem.files.find(dp->path);
        if (it == g_rem.files.end() || dp->modified()) continue;
        struct stat st;
        if (stat(dp->path.c_str(), &st) != 0 || (long)st.st_mtime == it->second.mtime) continue;
        it->second.mtime = (long)st.st_mtime;
        int pi = it->second.prof;
        if (pi < 0 || (size_t)pi >= g_rem.profiles.size()) continue;
        RemoteProfile p = g_rem.profiles[(size_t)pi];
        std::string pw = g_rem.conns[(size_t)pi].pw, local = dp->path, remote = it->second.remote;
        job_start(g_rjob, "Uploading " + base_name(remote), [p, pw, local, remote](std::string &err) { return remote_put(p, pw, local, remote, &err); }, [remote](int code, const std::string &err) {
            if (code != 0) remote_error("Upload failed: " + remote, err + "\nYour edits are saved in the local copy; save again to retry.");
            else E.status_msg = "uploaded " + remote;
        });
        return;   /* one at a time; the next save picks up the rest */
    }
}

void remote_ask_folder(RemoteProfile p) {
    bool smb = p.proto == "smb";
    dlg_show(smb ? "Share and folder" : "Remote folder to open", smb ? "For example /share/projects" : "Leave empty for the home folder (ssh, sftp) or the root (ftp)", {"Add", "Cancel"}, [p](int i2, const std::string &t2) {
        if (i2 != 0) return;
        RemoteProfile q = p;
        q.path = trim_nl(t2);
        g_rem.profiles.push_back(q);
        g_rem.conns.push_back(RConn());
        remote_save(g_rem.profiles);
        remote_connect((int)g_rem.profiles.size() - 1);
    }, true, "", smb ? "/share" : "/path (optional)");
}

void remote_ask_address(const std::string &proto) {
    const char *ph = proto == "smb" ? "user@server  (guest if empty)" : "user@host   or   user@host:port";
    dlg_show("New " + proto + " connection", "Server address", {"Next", "Cancel"}, [proto](int i, const std::string &t) {
        std::string a = trim_nl(t);
        if (i != 0 || a.empty()) return;
        RemoteProfile p;
        p.proto = proto;
        size_t at = a.find('@');
        if (at != std::string::npos) { p.user = a.substr(0, at); a = a.substr(at + 1); }
        size_t col = a.rfind(':');
        if (col != std::string::npos && a.find(']') == std::string::npos) { p.port = atoi(a.c_str() + col + 1); a = a.substr(0, col); }
        p.host = a;
        p.name = a;
        remote_ask_folder(p);
    }, true, "", ph);
}

void remote_new_connection() {
    remote_ensure();
    std::vector<Ed::PItem> items = {{"SSH (shell and files)", 0, false}, {"SFTP", 1, false}, {"FTP", 2, false}, {"FTPS (FTP over TLS)", 3, false}, {"SMB / Samba share", 4, false}};
    pick_custom("Select the connection type", items, [](const Ed::PItem &it) {
        static const char *protos[5] = {"ssh", "sftp", "ftp", "ftps", "smb"};
        std::string proto = protos[it.id];
        std::vector<SshHost> hosts = it.id < 2 ? remote_ssh_hosts() : std::vector<SshHost>();
        if (hosts.empty()) { remote_ask_address(proto); return; }
        /* hosts from ~/.ssh/config: ssh applies the user, port and key of the entry itself */
        std::vector<Ed::PItem> hi;
        for (size_t k = 0; k < hosts.size(); k++) hi.push_back({hosts[k].name + "    " + hosts[k].detail, (int)k, false});
        hi.push_back({"Enter an address...", -1, false});
        pick_custom("Select a host from ~/.ssh/config", hi, [proto, hosts](const Ed::PItem &h) {
            if (h.id < 0) { remote_ask_address(proto); return; }
            RemoteProfile p;
            p.proto = proto; p.host = hosts[(size_t)h.id].name; p.name = p.host;
            remote_ask_folder(p);
        });
    });
}

void remote_open_terminal(int pi) {
    if (pi < 0 || (size_t)pi >= g_rem.profiles.size()) return;
    std::string line = remote_ssh_line(g_rem.profiles[(size_t)pi]) + "\n";
    E.term_open = true;
    if (E.term_choice != "builtin") { launch_external(E.term_choice.substr(E.term_choice.find(':') + 1)); return; }
    if (new_terminal() && cur_term() && cur_term()->core) { E.term_focus = true; tcore_text(cur_term()->core, line.c_str()); }
}

void remote_menu(int pi, const std::string &path, int kind, float x, float y) {
    if (pi < 0 || (size_t)pi >= g_rem.profiles.size()) return;
    RConn &c = g_rem.conns[(size_t)pi];
    const RemoteProfile &p = g_rem.profiles[(size_t)pi];
    bool ssh = p.proto == "ssh" || p.proto == "sftp";
    if (kind == 0) {
        std::vector<CtxItem> v = {ci(c.connected ? "Reconnect" : "Connect", [pi] { if (g_rem.conns[(size_t)pi].connected) remote_disconnect_now(pi); remote_connect(pi); }),
                                  ci("Disconnect", [pi] { remote_disconnect_now(pi); }, "", c.connected), ci("New File...", [pi] { remote_ask_name(pi, remote_root(g_rem.profiles[(size_t)pi]), ROP_NEWFILE); }, "", c.connected), ci("New Folder...", [pi] { remote_ask_name(pi, remote_root(g_rem.profiles[(size_t)pi]), ROP_MKDIR); }, "", c.connected), ci("Search...", [pi] { remote_search(pi, remote_root(g_rem.profiles[(size_t)pi])); }, "", c.connected && ssh), ci("Refresh", [pi] { RConn &k = g_rem.conns[(size_t)pi]; k.dirs.clear(); k.expanded.clear(); remote_connect_now(pi); }, "", c.connected), ci_sep(),
                                  ci("Open SSH Terminal", [pi] { remote_open_terminal(pi); }, "", ssh), ci_sep(),
                                  ci("Edit Connections File", [] { const char *x2 = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME"); open_path((x2 && *x2 ? std::string(x2) : std::string(h ? h : ".") + "/.config") + "/hootedit/remotes.ini"); }),
                                  ci("Remove Connection", [pi] {
                                      dlg_show("Remove the connection '" + g_rem.profiles[(size_t)pi].name + "'?", "Only the saved entry is removed. Nothing on the server changes.", {"Remove", "Cancel"}, [pi](int i, const std::string &) {
                                          if (i != 0 || (size_t)pi >= g_rem.profiles.size()) return;
                                          remote_disconnect_now(pi);
                                          g_rem.profiles.erase(g_rem.profiles.begin() + pi);
                                          g_rem.conns.erase(g_rem.conns.begin() + pi);
                                          for (auto it = g_rem.files.begin(); it != g_rem.files.end();) { if (it->second.prof == pi) it = g_rem.files.erase(it); else { if (it->second.prof > pi) it->second.prof--; ++it; } }
                                          remote_save(g_rem.profiles);
                                      }); })};
        ctx_show(x, y, std::move(v));
    } else if (kind == 1) {
        ctx_show(x, y, {ci("New File...", [pi, path] { remote_ask_name(pi, path, ROP_NEWFILE); }), ci("New Folder...", [pi, path] { remote_ask_name(pi, path, ROP_MKDIR); }), ci_sep(),
                        ci("Rename...", [pi, path] { remote_ask_rename(pi, path, true); }), ci("Delete", [pi, path] { remote_ask_delete(pi, path, true); }), ci_sep(),
                        ci("Search in Folder...", [pi, path] { remote_search(pi, path); }, "", ssh), ci_sep(),
                        ci("Refresh", [pi, path] { g_rem.conns[(size_t)pi].dirs.erase(path); remote_list_dir(pi, path); }), ci("Copy Path", [path] { SDL_SetClipboardText(path.c_str()); })});
    } else if (kind == 2) {
        ctx_show(x, y, {ci("Open", [pi, path] { remote_open_file(pi, path); }), ci_sep(), ci("Rename...", [pi, path] { remote_ask_rename(pi, path, false); }), ci("Delete", [pi, path] { remote_ask_delete(pi, path, false); }), ci_sep(),
                        ci("Copy Path", [path] { SDL_SetClipboardText(path.c_str()); })});
    }
}

void remote_row_click(int idx) {
    if (idx < 0 || (size_t)idx >= g_rrows.size()) return;
    RRow r = g_rrows[(size_t)idx];
    RConn &c = g_rem.conns[(size_t)r.prof];
    if (r.kind == 0) {
        if (!c.connected) remote_connect(r.prof); else c.open = !c.open;
    } else if (r.kind == 1) {
        if (c.expanded.count(r.path)) c.expanded.erase(r.path);
        else if (c.dirs.count(r.path)) c.expanded.insert(r.path);
        else remote_list_dir(r.prof, r.path);
    } else if (r.kind == 2) remote_open_file(r.prof, r.path);
}

void tip_poll() {
    if (E.tip_at > 0 && now_s() >= E.tip_at) { E.tip_at = 0; E.tip_on = true; redraw(); }
}

void askpass_poll() {
    if (E.mode == M_DIALOG) return;
    AskReq r;
    if (!askpass_take(r)) return;
    std::string low = r.prompt;
    for (char &c : low) c = (char)tolower((unsigned char)c);
    bool secret = low.find("password") != std::string::npos || low.find("passphrase") != std::string::npos || low.find("pin") != std::string::npos || low.find("token") != std::string::npos;
    std::string title = trim_nl(r.prompt);
    while (!title.empty() && (title.back() == ':' || title.back() == ' ')) title.pop_back();
    if (title.empty()) title = "Authentication required";
    int fd = r.fd;
    if (low.find("(yes/no") != std::string::npos) {   /* ssh asking whether to trust a new host key */
        dlg_show(trim_nl(r.prompt), "The authenticity of this host cannot be verified yet. Only continue if you trust it.", {"Yes", "No"}, [fd](int i, const std::string &) { if (i < 0) askpass_reply(fd, false, ""); else askpass_reply(fd, true, i == 0 ? "yes" : "no"); });
        return;
    }
    dlg_show(title, "Asked by git or ssh. Hootedit passes your answer on and does not store it.", {"OK", "Cancel"}, [fd](int i, const std::string &t) { askpass_reply(fd, i == 0, t); }, true, "", secret ? "Enter secret" : "Enter value", secret);
}

void git_job_poll() { job_poll(g_job); }

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


/* ---- git decorations in the editor: change bars in the gutter and blame for the cursor line ---- */

GutterCache *gutter_for(Doc &d) {
    if (d.dv || d.mv || !E.git.repo || d.path.empty() || d.buf.lines() > 50000) return nullptr;
    std::string rel = rel_to_root(d.path);
    if (rel.empty()) return nullptr;
    GutterCache &c = g_gutter[&d];
    if (c.base_stale) {
        std::string base = git_show(E.git.root, "HEAD", rel);
        diff_split_lines(base, c.base_lines);
        c.ids.clear();
        for (const std::string &l : c.base_lines) c.ids.emplace(l, (int)c.ids.size());
        c.base_stale = false;
        c.computed = false;
    }
    if (!c.computed || c.rev != d.buf.revision()) {
        size_t nl = d.buf.lines();
        if (nl > 1 && d.buf.line_len(nl - 1) == 0) nl--;   /* the empty line after the final newline is not a line of text */
        std::vector<int> a(c.base_lines.size()), b(nl);
        for (size_t i = 0; i < a.size(); i++) a[i] = c.ids[c.base_lines[i]];
        std::unordered_map<std::string, int> extra;
        std::string t;
        for (size_t i = 0; i < b.size(); i++) {
            d.buf.line_text(i, t);
            if (!t.empty() && t.back() == '\r') t.pop_back();
            auto it = c.ids.find(t);
            if (it != c.ids.end()) { b[i] = it->second; continue; }
            auto ex = extra.emplace(t, (int)(c.ids.size() + extra.size()));
            b[i] = ex.first->second;
        }
        if (d.buf.size() == 0) b.clear();
        c.hunks = diff_ids(a, b, 800, nullptr);
        c.rev = d.buf.revision();
        c.computed = true;
    }
    return &c;
}

const GitBlame *blame_for(Doc &d, size_t line) {
    if (d.dv || d.mv || d.modified() || !E.git.repo || d.path.empty()) return nullptr;
    GutterCache *c = gutter_for(d);
    if (!c) return nullptr;
    if (!c->blame_tried) {
        c->blame_tried = true;
        std::string rel = rel_to_root(d.path);
        c->blame_ok = d.buf.size() < (2u << 20) && git_blame(E.git.root, rel, c->blame);
    }
    return c->blame_ok && line < c->blame.size() ? &c->blame[line] : nullptr;
}

std::string blame_text(const GitBlame &b, bool summary = true) {   /* the status bar passes false: who and when only */
    if (b.hash.find_first_not_of('0') == std::string::npos) return "You, Uncommitted changes";
    std::string who = b.author + ", " + git_ago(b.time, (long)time(nullptr));
    return summary ? who + "  \xE2\x80\xA2  " + b.summary : who;
}

void open_blame_commit(const GitBlame &b, const std::string &path) {
    if (b.hash.find_first_not_of('0') == std::string::npos) { open_diff_spec(path, ":", "WORK", "Index", "Working Tree", base_name(path) + " (Working Tree)", "Unstaged changes"); return; }
    GitCommit c;
    c.hash = b.hash; c.subject = b.summary; c.author = b.author; c.date = b.time;
    open_commit_diff(c, GitFile{'M', path});
}

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
    std::vector<std::string> words;   /* every word typed has to appear, in any order: "git pu" finds "Git: Pull" */
    for (size_t p0 = 0; p0 < f.size();) {
        size_t e = f.find(' ', p0);
        if (e == std::string::npos) e = f.size();
        if (e > p0) words.push_back(f.substr(p0, e - p0));
        p0 = e + 1;
    }
    for (size_t i = 0; i < E.pick_items.size(); i++) {
        std::string l = lower_ascii(E.pick_items[i].label);
        bool ok = true;
        for (const std::string &w : words) if (l.find(w) == std::string::npos) { ok = false; break; }
        if (ok) v.push_back((int)i);
    }
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


/* ---- the Source Control view (modelled on VS Code's): summary, commit box, staged and other changes, history graph, file timeline ---- */

struct GEnt { int idx; bool staged; char st; };
std::vector<std::string> g_gitdirs;   /* tree view folder keys of the current frame, indexed by the hit id */

void git_entries(std::vector<GEnt> &staged, std::vector<GEnt> &other, std::vector<GEnt> &merge) {
    for (size_t i = 0; i < E.git.changes.size(); i++) {
        const GitChange &c = E.git.changes[i];
        bool conflict = (c.x == 'U' || c.y == 'U' || (c.x == 'A' && c.y == 'A') || (c.x == 'D' && c.y == 'D'));
        if (c.x == '?' && c.y == '?') other.push_back({(int)i, false, '?'});
        else if (conflict) merge.push_back({(int)i, false, 'C'});
        else {
            if (c.x != ' ') staged.push_back({(int)i, true, c.x});
            if (c.y != ' ') other.push_back({(int)i, false, c.y});
        }
    }
}

uint32_t status_color(char st) {
    switch (st) {
    case 'M': return C_GIT_MOD;
    case 'A': case 'R': return C_GIT_ADD;
    case 'D': return C_GIT_DEL;
    case '?': return C_GIT_UNT;
    case 'C': return C_GIT_CONF;
    default: return C_UI_DIM;
    }
}

const char *status_letter(char st) {
    switch (st) { case 'M': return "M"; case 'A': return "A"; case 'D': return "D"; case 'R': return "R"; case '?': return "U"; case 'C': return "!"; default: return "?"; }
}

void draw_pill(float x, float y, float w, float h, uint32_t col) {
    int cap = (int)(h * 2.36f);   /* IC_DOT's circle is 0.425 of its box wide */
    draw_icon(IC_DOT, x + h / 2 - (float)cap / 2, y + h / 2 - (float)cap / 2, cap, col);
    draw_icon(IC_DOT, x + w - h / 2 - (float)cap / 2, y + h / 2 - (float)cap / 2, cap, col);
    r_rect(E.r, x + h / 2, y, std::max(0.f, w - h), h, col);
}

float draw_badge(float right, float y, float h, int n, uint32_t bg, uint32_t fg) {   /* a count in a pill, right aligned at `right`; returns its left edge */
    std::string t = std::to_string(n);
    float w = std::max(h, text_width(E.ui, t.c_str()) + (float)S(10));
    draw_pill(right - w, y, w, h, bg);
    draw_text(E.ui, right - w + (w - text_width(E.ui, t.c_str())) / 2, ui_base(y, h) - (float)S(1), t.c_str(), fg, w);
    return right - w;
}

void git_ref_pills(const std::string &refs, float *right, float y, float h, float minx) {   /* branch and tag labels, right aligned; drawn from the right edge inwards */
    std::vector<std::pair<std::string, int>> labs;   /* label, kind: 0 local branch, 1 remote, 2 tag */
    size_t pos = 0;
    while (pos < refs.size()) {
        size_t e = refs.find(", ", pos);
        if (e == std::string::npos) e = refs.size();
        std::string r = refs.substr(pos, e - pos);
        pos = e + 2;
        if (r == "HEAD") continue;
        if (r.compare(0, 8, "HEAD -> ") == 0) r = r.substr(8);
        int kind = r.compare(0, 5, "tag: ") == 0 ? 2 : r.find('/') != std::string::npos ? 1 : 0;
        if (kind == 2) r = r.substr(5);
        labs.push_back({r, kind});
    }
    for (size_t i = 0; i < labs.size() && i < 3; i++) {
        float w = text_width(E.ui, labs[i].first.c_str()) + (float)S(14);
        if (*right - w < minx) break;
        uint32_t bg = labs[i].second == 0 ? C_ACCENT : labs[i].second == 1 ? ui_mix(C_CHROME, C_ACCENT, 0.40) : ui_mix(C_CHROME, C_GIT_MOD, 0.45);
        uint32_t fg = labs[i].second == 0 ? C_ON_ACCENT : C_UI;
        draw_pill(*right - w, y, w, h, bg);
        draw_text(E.ui, *right - w + (float)S(7), ui_base(y, h) - (float)S(1), labs[i].first.c_str(), fg, w - (float)S(10));
        *right -= w + (float)S(4);
    }
}

void draw_git_view(float rail, float sw, float cx, float cw, float list_top, float bot, float &ry) {
    GitUi &g = E.g;
    g_gitdirs.clear();
    const float rowh = (float)S(26), full_x = rail + 1, full_w = sw - rail - 2, right = sw - (float)S(10);
    auto visible = [&](float h) { return ry + h >= list_top && ry <= bot; };
    std::vector<GEnt> staged, other, merge;
    git_entries(staged, other, merge);
    int n_mod = 0, n_add = 0, n_del = 0, n_unt = 0;
    for (const GitChange &c : E.git.changes) {
        if (c.x == '?') n_unt++;
        else if (c.x == 'D' || c.y == 'D') n_del++;
        else if (c.x == 'A' || c.x == 'R') n_add++;
        else n_mod++;
    }

    /* title and refresh */
    if (visible(rowh)) {
        draw_text(E.ui, cx, ui_base(ry, rowh), "SOURCE CONTROL", C_GROUP, cw, FS_BOLD);
        float bx = right - (float)S(22);
        if (hovered(H_GITREFRESH, 0)) r_rect(E.r, bx - (float)S(3), ry + (float)S(2), (float)S(24), rowh - (float)S(4), C_HOVER);
        draw_icon(IC_REFRESH, bx, ry + (rowh - (float)S(16)) / 2, S(16), C_UI_DIM);
        hit(bx - (float)S(3), ry + (float)S(2), (float)S(24), rowh - (float)S(4), H_GITREFRESH, 0);
        float mx2 = bx - (float)S(26);   /* the "..." more actions button */
        if (hovered(H_GITMORE, 0)) r_rect(E.r, mx2 - (float)S(3), ry + (float)S(2), (float)S(24), rowh - (float)S(4), C_HOVER);
        for (int k = 0; k < 3; k++) draw_dot(mx2 + (float)S(2) + (float)k * (float)S(6), ry + rowh / 2, S(6), C_UI_DIM);
        hit(mx2 - (float)S(3), ry + (float)S(2), (float)S(24), rowh - (float)S(4), H_GITMORE, 0);
        if (g_job.running) draw_text(E.ui, mx2 - (float)S(8) - text_width(E.ui, (g_job.label + "...").c_str()), ui_base(ry, rowh), (g_job.label + "...").c_str(), C_ACCENT, (float)S(140));
    }
    ry += rowh;

    /* branch, sync state and the number of changes */
    if (visible(rowh)) {
        draw_icon(IC_GIT, cx, ry + (rowh - (float)S(14)) / 2, S(14), C_ACCENT);
        float pen = draw_text(E.ui, cx + (float)S(22), ui_base(ry, rowh), E.git.branch.c_str(), C_UI, cw * 0.55f, FS_BOLD);
        if (E.git.ahead) { draw_icon(IC_ARROW_U, pen + (float)S(6), ry + (rowh - (float)S(14)) / 2, S(14), C_UI_DIM); pen = draw_text(E.ui, pen + (float)S(20), ui_base(ry, rowh), std::to_string(E.git.ahead).c_str(), C_UI_DIM, (float)S(40)); }
        if (E.git.behind) { draw_icon(IC_ARROW_D, pen + (float)S(6), ry + (rowh - (float)S(14)) / 2, S(14), C_UI_DIM); draw_text(E.ui, pen + (float)S(20), ui_base(ry, rowh), std::to_string(E.git.behind).c_str(), C_UI_DIM, (float)S(40)); }
        if (hovered(H_GITBRANCH, 0)) r_rect(E.r, cx - (float)S(3), ry + 1, std::min(cw * 0.6f, pen - cx + (float)S(8)), rowh - 2, C_HOVER);
        hit(cx - (float)S(3), ry, std::min(cw * 0.6f, pen - cx + (float)S(8)), rowh, H_GITBRANCH, 0);
        std::string tot = E.git.changes.empty() ? "clean" : sfmt("%zu change%s", E.git.changes.size(), E.git.changes.size() == 1 ? "" : "s");
        draw_text(E.ui, right - text_width(E.ui, tot.c_str()), ui_base(ry, rowh), tot.c_str(), E.git.changes.empty() ? C_GIT_UNT : C_UI_DIM, (float)S(120));
    }
    ry += rowh;
    if (!E.git.changes.empty()) {   /* modified / added / deleted / untracked, coloured like the file rows */
        if (visible(rowh)) {
            float x = cx;
            struct { int n; const char *l, *sh; uint32_t c; } sm[4] = {{n_mod, "modified", "M", C_GIT_MOD}, {n_add, "added", "A", C_GIT_ADD}, {n_del, "deleted", "D", C_GIT_DEL}, {n_unt, "untracked", "U", C_GIT_UNT}};
            float need = 0;
            for (auto &it : sm) if (it.n) need += text_width(E.ui, sfmt("%d %s", it.n, it.l).c_str()) + (float)S(22);
            bool compact = need > cw;   /* the long words do not fit the sidebar: use the letters VS Code shows on the rows */
            for (auto &it : sm) {
                if (!it.n) continue;
                std::string t = sfmt("%d %s", it.n, compact ? it.sh : it.l);
                draw_dot(x + (float)S(4), ry + rowh / 2, S(8), it.c);
                x = draw_text(E.ui, x + (float)S(12), ui_base(ry, rowh), t.c_str(), C_UI_DIM, sw - x) + (float)S(10);
            }
        }
        ry += (float)S(22);
    }

    if (!E.git.op.empty()) {   /* an unfinished merge, rebase, cherry-pick or revert: resolve, stage, then Continue (or Skip, or Abort) */
        std::string lab = E.git.op == "merge" ? "MERGING" : E.git.op == "rebase" ? "REBASING" : E.git.op == "cherry-pick" ? "CHERRY-PICKING" : "REVERTING";
        if (visible(rowh)) {
            float lw = draw_text(E.ui, cx, ui_base(ry, rowh), lab.c_str(), C_GIT_MOD, cw, FS_BOLD);
            draw_text(E.ui, lw + (float)S(8), ui_base(ry, rowh), "resolve, stage, continue", C_UI_FAINT, cx + cw - lw - (float)S(8));
        }
        ry += rowh;
        if (visible(rowh)) {
            const bool can_skip = E.git.op == "rebase" || E.git.op == "cherry-pick";
            const char *names[3] = {"Continue", "Skip", "Abort"};
            int nb = can_skip ? 3 : 2;
            float bw = (cw - (float)(nb - 1) * (float)S(6)) / (float)nb, x = cx;
            for (int k = 0; k < 3; k++) {
                if (k == 1 && !can_skip) continue;
                bool hv = hovered(H_GITOP, k);
                r_rect(E.r, x, ry, bw, rowh - (float)S(2), k == 0 ? (hv ? ui_mix(C_ACCENT, C_UI, 0.14) : C_ACCENT) : (hv ? C_SELROW : C_FIELD));
                float tw = text_width(E.ui, names[k]);
                draw_text(E.ui, x + (bw - tw) / 2, ui_base(ry, rowh - (float)S(2)), names[k], k == 0 ? C_ON_ACCENT : C_UI, bw);
                hit(x, ry, bw, rowh - (float)S(2), H_GITOP, k);
                x += bw + (float)S(6);
            }
        }
        ry += rowh + (float)S(4);
    }

    /* commit message and button */
    {
        std::vector<std::string> ml;
        size_t pos = 0;
        while (true) { size_t e = g.msg.find('\n', pos); ml.push_back(g.msg.substr(pos, e == std::string::npos ? std::string::npos : e - pos)); if (e == std::string::npos) break; pos = e + 1; }
        int shown = std::min<int>(5, (int)ml.size());
        float lh = (float)S(18), bh = (float)shown * lh + (float)S(12);
        bool foc = E.mode == M_GITMSG;
        if (visible(bh)) {
            r_rect(E.r, cx, ry, cw, bh, foc ? C_ACCENT : C_BOX_LINE);
            r_rect(E.r, cx + 1, ry + 1, cw - 2, bh - 2, C_FIELD);
            if (g.msg.empty()) draw_text(E.ui, cx + (float)S(8), ui_base(ry + (float)S(6), lh), "Message (Ctrl+Enter to commit)", C_UI_FAINT, cw - (float)S(16));
            float pen = cx + (float)S(8);
            for (int i = 0; i < shown; i++) {
                const std::string &line = ml[ml.size() - (size_t)shown + (size_t)i];
                pen = draw_text(E.ui, cx + (float)S(8), ui_base(ry + (float)S(6) + (float)i * lh, lh), line.c_str(), C_UI, cw - (float)S(16));
            }
            if (foc && E.blink_on) r_rect(E.r, pen + 1, ry + (float)S(6) + (float)(shown - 1) * lh + (float)S(2), std::max(1.f, E.scale), lh - (float)S(4), C_CURSOR);
            hit(cx, ry, cw, bh, H_GITMSG, 0);
        }
        ry += bh + (float)S(6);
        if (!g.err.empty()) { if (visible(rowh)) draw_text(E.ui, cx, ui_base(ry, (float)S(20)), g.err.c_str(), C_RED, cw); ry += (float)S(22); }
        float bth = (float)S(30);
        if (visible(bth)) {
            bool clean = E.git.changes.empty() || (staged.empty() && other.empty());
            bool sync = clean && !E.git.upstream.empty() && (E.git.ahead || E.git.behind);
            bool publish = clean && E.git.upstream.empty() && E.git.has_remote && E.git.branch.compare(0, 4, "HEAD") != 0;
            int mode = sync ? 1 : publish ? 2 : 0;
            bool can = !clean || mode != 0;
            std::string lab = mode == 1 ? sfmt("Sync Changes  %s%s%s", E.git.behind ? sfmt("%d down", E.git.behind).c_str() : "", E.git.behind && E.git.ahead ? "  " : "", E.git.ahead ? sfmt("%d up", E.git.ahead).c_str() : "")
                              : mode == 2 ? "Publish Branch" : clean ? (merge.empty() ? "Nothing to commit" : "Resolve the conflicts first") : !staged.empty() ? sfmt("Commit  (%zu staged)", staged.size()) : sfmt("Commit All  (%zu)", other.size());
            float ddw = mode == 0 && can ? (float)S(26) : 0.f;
            uint32_t bg = !can ? C_FIELD : hovered(H_GITCOMMIT, mode) ? ui_mix(C_ACCENT, C_UI, 0.14) : C_ACCENT;
            uint32_t fg = can ? C_ON_ACCENT : C_UI_FAINT;
            r_rect(E.r, cx, ry, cw - ddw, bth, bg);
            float lw = text_width(E.ui, lab.c_str()) + (float)S(22);
            float lx = cx + (cw - ddw - lw) / 2;
            if (can) draw_icon(mode == 1 ? IC_REFRESH : mode == 2 ? IC_ARROW_U : IC_CHECK, lx, ry + (bth - (float)S(16)) / 2, S(16), fg);
            draw_text(E.ui, lx + (can ? (float)S(22) : 0.f), ui_base(ry, bth), lab.c_str(), fg, cw - ddw);
            hit(cx, ry, cw - ddw, bth, H_GITCOMMIT, mode);
            if (ddw > 0) {   /* the arrow: Commit (Amend), Commit and Push, Undo Last Commit ... */
                r_rect(E.r, cx + cw - ddw, ry, ddw, bth, hovered(H_GITDD, 0) ? ui_mix(C_ACCENT, C_UI, 0.14) : C_ACCENT);
                r_rect(E.r, cx + cw - ddw, ry + (float)S(5), 1, bth - (float)S(10), ui_mix(C_ACCENT, C_ON_ACCENT, 0.4));
                draw_icon(IC_CHEV_D, cx + cw - ddw + (ddw - (float)S(16)) / 2, ry + (bth - (float)S(16)) / 2, S(16), fg);
                hit(cx + cw - ddw, ry, ddw, bth, H_GITDD, 0);
            }
        }
        ry += bth + (float)S(10);
    }

    auto section = [&](int id, const char *name, bool open, int count, int all_id) {
        float h = (float)S(26);
        if (visible(h)) {
            if (hovered(H_GITSEC, id)) r_rect(E.r, full_x, ry, full_w, h, C_HOVER);
            draw_icon(open ? IC_CHEV_D : IC_CHEV_R, cx - (float)S(4), ry + (h - (float)S(16)) / 2, S(16), C_GROUP);
            float pen = draw_text(E.ui, cx + (float)S(14), ui_base(ry, h), name, C_UI_DIM, cw, FS_BOLD);
            hit(full_x, ry, full_w, h, H_GITSEC, id);
            float r = right;
            if (all_id == 0 && count > 0) {   /* discard all changes */
                float dx = right - (float)S(44);
                if (hovered(H_GITDISC, -1)) r_rect(E.r, dx - (float)S(2), ry + (float)S(3), (float)S(22), h - (float)S(6), C_HOVER);
                draw_icon(IC_UNDO, dx, ry + (h - (float)S(16)) / 2, S(16), C_UI_DIM);
                hit(dx - (float)S(2), ry + (float)S(3), (float)S(22), h - (float)S(6), H_GITDISC, -1);
                r = dx - (float)S(6);
            }
            if (all_id >= 0 && count > 0) {   /* stage all / unstage all */
                float bx = right - (float)S(20);
                if (hovered(H_GITALL, all_id)) r_rect(E.r, bx - (float)S(2), ry + (float)S(3), (float)S(22), h - (float)S(6), C_HOVER);
                draw_icon(all_id == 0 ? IC_PLUS : IC_MINUS, bx, ry + (h - (float)S(16)) / 2, S(16), C_UI_DIM);
                hit(bx - (float)S(2), ry + (float)S(3), (float)S(22), h - (float)S(6), H_GITALL, all_id);
                r = std::min(r, bx - (float)S(6));
            }
            if (count >= 0) draw_badge(r, ry + (h - (float)S(16)) / 2, (float)S(16), count, C_ACCENT, C_ON_ACCENT);
            (void)pen;
        }
        ry += h;
    };

    auto file_rows = [&](const std::vector<GEnt> &ents, int sect) {
        std::vector<int> order;
        for (size_t k = 0; k < ents.size(); k++) order.push_back((int)k);
        if (g.tree_view) std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return E.git.changes[(size_t)ents[(size_t)a].idx].path < E.git.changes[(size_t)ents[(size_t)b].idx].path; });
        std::vector<std::string> prev;   /* folder path of the previous row, to know which folder rows to emit */
        std::string hidden_under;        /* a collapsed folder: everything below it is skipped */
        for (int ok : order) {
            const GEnt &en = ents[(size_t)ok];
            const GitChange &c = E.git.changes[(size_t)en.idx];
            int id = en.idx * 2 + (en.staged ? 1 : 0);
            std::string base = base_name(c.path), dir = c.path.size() > base.size() ? c.path.substr(0, c.path.size() - base.size() - 1) : std::string();
            int depth = 0;
            if (g.tree_view) {
                std::vector<std::string> parts;
                size_t p0 = 0;
                while (p0 < dir.size()) { size_t e = dir.find('/', p0); if (e == std::string::npos) e = dir.size(); parts.push_back(dir.substr(p0, e - p0)); p0 = e + 1; }
                size_t common = 0;
                while (common < parts.size() && common < prev.size() && parts[common] == prev[common]) common++;
                std::string acc;
                bool skip = false;
                for (size_t d = 0; d < parts.size(); d++) {
                    acc += (d ? "/" : "") + parts[d];
                    std::string key = sfmt("%d:", sect) + acc;
                    bool closed = g.collapsed.count(key) != 0;
                    if (d >= common) {
                        if (visible(rowh)) {
                            if (hovered(H_GITDIR, (int)g_gitdirs.size())) r_rect(E.r, full_x, ry, full_w, rowh, C_HOVER);
                            float fx = cx + (float)d * (float)S(12);
                            draw_icon(closed ? IC_CHEV_R : IC_CHEV_D, fx - (float)S(4), ry + (rowh - (float)S(16)) / 2, S(16), C_GROUP);
                            draw_text(E.ui, fx + (float)S(12), ui_base(ry, rowh), parts[d].c_str(), C_UI, right - fx - (float)S(16));
                            hit(full_x, ry, full_w, rowh, H_GITDIR, (int)g_gitdirs.size());
                        }
                        g_gitdirs.push_back(key);
                        ry += rowh;
                    }
                    if (closed) { skip = true; break; }
                }
                prev = parts;
                if (skip) { prev.resize(std::min(prev.size(), parts.size())); continue; }
                depth = (int)parts.size();
            }
            if (visible(rowh)) {
                bool hv = hovered(H_GITROW, id) || hovered(H_GITSTG, id) || hovered(H_GITOPEN, id) || hovered(H_GITDISC, id);
                if (hv) r_rect(E.r, full_x, ry, full_w, rowh, C_HOVER);
                uint32_t col = status_color(en.st);
                float ix = cx + (float)depth * (float)S(12);
                draw_dot(ix + (float)S(8), ry + rowh / 2, S(8), file_color(base));
                const bool conf = en.st == 'C';
                int nb = en.staged || conf ? 2 : 3;   /* open, (discard), stage / unstage */
                float lim = right - (float)S(18) - (hv ? (float)(nb * 22) * E.scale : 0.f);
                float pen = draw_text(E.ui, ix + (float)S(20), ui_base(ry, rowh), base.c_str(), en.st == 'D' ? C_UI_FAINT : C_UI, lim - ix - (float)S(20));
                if (!g.tree_view && !dir.empty() && pen + (float)S(30) < lim) draw_text(E.ui, pen + (float)S(8), ui_base(ry, rowh), dir.c_str(), C_UI_FAINT, lim - pen - (float)S(8));
                float lw = text_width(E.ui, status_letter(en.st));
                draw_text(E.ui, right - (float)S(8) - lw, ui_base(ry, rowh), status_letter(en.st), col, (float)S(20), FS_BOLD);
                hit(full_x, ry, full_w, rowh, H_GITROW, id);
                if (hv) {
                    float bx = right - (float)S(22) * (float)nb - (float)S(2);
                    for (int b = 0; b < nb; b++) {
                        int kind = b == 0 ? H_GITOPEN : (b == 1 && !en.staged && !conf) ? H_GITDISC : H_GITSTG;
                        Icon ic = b == 0 ? IC_OPEN_FILE : (b == 1 && !en.staged && !conf) ? IC_UNDO : en.staged ? IC_MINUS : IC_PLUS;
                        if (hovered(kind, id)) r_rect(E.r, bx - (float)S(1), ry + (float)S(3), (float)S(20), rowh - (float)S(6), C_SELROW);
                        draw_icon(ic, bx + (float)S(1), ry + (rowh - (float)S(16)) / 2, S(16), C_UI_DIM);
                        hit(bx - (float)S(1), ry + (float)S(3), (float)S(20), rowh - (float)S(6), kind, id);
                        bx += (float)S(22);
                    }
                }
            }
            ry += rowh;
        }
    };

    if (E.git.changes.empty()) { if (visible(rowh)) draw_text(E.ui, cx, ui_base(ry, rowh), "No changes. The working tree matches HEAD.", C_UI_FAINT, cw); ry += rowh; }
    if (!merge.empty()) { section(4, "Merge Changes", g.open_merge, (int)merge.size(), -1); if (g.open_merge) file_rows(merge, 4); }
    if (!staged.empty()) { section(0, "Staged Changes", g.open_staged, (int)staged.size(), 1); if (g.open_staged) file_rows(staged, 0); }
    if (!other.empty()) { section(1, "Changes", g.open_changes, (int)other.size(), 0); if (g.open_changes) file_rows(other, 1); }
    ry += (float)S(6);

    /* the history graph */
    section(2, "GRAPH", g.open_graph, (int)g.log.size(), -1);
    if (g.open_graph) {
        const float gh = (float)S(24);
        const float lw = (float)S(10), lx0 = cx + (float)S(6), th = std::max(1.f, (float)S(2));
        const float textx = lx0 + (float)g.lane_cols * lw + (float)S(2);
        const uint32_t pal[5] = {C_GUT_MOD, C_GUT_ADD, C_GIT_MOD, C_ACCENT, C_GUT_DEL};
        auto lane_x = [&](int k) { return lx0 + (float)std::min(k, 4) * lw; };
        auto through = [&](size_t i, float y, float h) {   /* the lanes that continue below commit i, drawn over a row of height h */
            if (i >= g.lanes.size()) return;
            const GitUi::Lane &ln = g.lanes[i];
            for (size_t k = 0; k < ln.bot.size() && k < 5; k++) if (ln.bot[k]) r_rect(E.r, lane_x((int)k) - th / 2, y, th, h, pal[k % 5]);
        };
        for (size_t i = 0; i < g.log.size(); i++) {
            const GitCommit &c = g.log[i];
            bool sel = (int)i == g.log_sel, marked = !g.mark_hash.empty() && c.hash == g.mark_hash;
            if (visible(gh)) {
                if (sel) r_rect(E.r, full_x, ry, full_w, gh, C_SELROW);
                else if (hovered(H_GITLOG, (int)i)) r_rect(E.r, full_x, ry, full_w, gh, C_HOVER);
                if (marked) r_rect(E.r, full_x, ry, (float)S(2), gh, C_ACCENT);
                const GitUi::Lane &ln = g.lanes[i];
                auto has = [](const std::vector<int> &v, int k) { return std::find(v.begin(), v.end(), k) != v.end(); };
                const float mid = ry + gh / 2;
                for (size_t k = 0; k < std::max(ln.top.size(), ln.bot.size()) && k < 5; k++) {
                    if ((int)k == ln.col) continue;
                    bool t = k < ln.top.size() && ln.top[k], b = k < ln.bot.size() && ln.bot[k], cv = has(ln.conv, (int)k), ou = has(ln.out, (int)k);
                    float x = lane_x((int)k), x2 = lane_x(ln.col);
                    uint32_t col = pal[k % 5];
                    if (t && b && !cv && !ou) r_rect(E.r, x - th / 2, ry, th, gh, col);
                    if (cv) { r_rect(E.r, x - th / 2, ry, th, gh / 2, col); r_rect(E.r, std::min(x, x2), mid - th / 2, std::fabs(x2 - x) + th / 2, th, col); }
                    if (ou) { r_rect(E.r, x - th / 2, mid, th, gh / 2, col); r_rect(E.r, std::min(x, x2), mid - th / 2, std::fabs(x2 - x) + th / 2, th, col); }
                }
                float cxl = lane_x(ln.col);
                uint32_t ccol = pal[(size_t)ln.col % 5];
                if (!ln.top.empty() && (size_t)ln.col < ln.top.size() && ln.top[(size_t)ln.col]) r_rect(E.r, cxl - th / 2, ry, th, gh / 2, ccol);
                if ((size_t)ln.col < ln.bot.size() && ln.bot[(size_t)ln.col]) r_rect(E.r, cxl - th / 2, mid, th, gh / 2, ccol);
                bool head = c.refs.find("HEAD") != std::string::npos;
                draw_dot(cxl, mid, S(11), head ? C_ACCENT : ccol);
                if (head || c.parents > 1) draw_dot(cxl, mid, S(5), sel ? C_SELROW : C_CHROME);   /* HEAD and merges are hollow */
                float pr = right;
                git_ref_pills(c.refs, &pr, ry + (gh - (float)S(16)) / 2, (float)S(16), cx + cw * 0.35f);
                draw_text(E.ui, textx, ui_base(ry, gh), c.subject.c_str(), C_UI, pr - textx - (float)S(4), head ? FS_BOLD : FS_REGULAR);
                hit(full_x, ry, full_w, gh, H_GITLOG, (int)i);
            }
            ry += gh;
            if (sel) {   /* the commit's details and files, indented under it */
                float mh = (float)S(22);
                if (visible(mh)) {
                    std::string meta = g.cmp_a.empty() ? sfmt("%s  %s, %s", c.hash.substr(0, 7).c_str(), c.author.c_str(), git_ago(c.date, (long)time(nullptr)).c_str())
                                                       : sfmt("comparing %s with %s", g.cmp_a.substr(0, 7).c_str(), g.cmp_b.substr(0, 7).c_str());
                    draw_text(E.ui, textx, ui_base(ry, mh), meta.c_str(), C_UI_FAINT, right - textx);
                    through(i, ry, mh);
                }
                ry += mh;
                for (size_t k = 0; k < g.sel_files.size(); k++) {
                    const GitFile &f = g.sel_files[k];
                    if (visible(rowh)) {
                        if (hovered(H_GITCFILE, (int)k)) r_rect(E.r, full_x, ry, full_w, rowh, C_HOVER);
                        through(i, ry, rowh);
                        std::string base = base_name(f.path), dir = f.path.size() > base.size() ? f.path.substr(0, f.path.size() - base.size() - 1) : std::string();
                        float lim = right - (float)S(22);
                        float pen = draw_text(E.ui, textx, ui_base(ry, rowh), base.c_str(), status_color(f.status), lim - textx);
                        if (!dir.empty() && pen + (float)S(30) < lim) draw_text(E.ui, pen + (float)S(8), ui_base(ry, rowh), dir.c_str(), C_UI_FAINT, lim - pen - (float)S(8));
                        draw_text(E.ui, right - (float)S(8) - text_width(E.ui, status_letter(f.status)), ui_base(ry, rowh), status_letter(f.status), status_color(f.status), (float)S(20), FS_BOLD);
                        hit(full_x, ry, full_w, rowh, H_GITCFILE, (int)k);
                    }
                    ry += rowh;
                }
                if (g.sel_files.empty()) { if (visible(mh)) draw_text(E.ui, textx, ui_base(ry, mh), "no file changes", C_UI_FAINT, cw); ry += mh; }
            }
        }
        if (g.log.empty()) { if (visible(rowh)) draw_text(E.ui, cx, ui_base(ry, rowh), "No commits yet", C_UI_FAINT, cw); ry += rowh; }
        if (!g.mark_hash.empty()) { if (visible(rowh)) draw_text(E.ui, cx, ui_base(ry, rowh), sfmt("Marked %s. Shift+click to compare", g.mark_label.c_str()).c_str(), C_UI_FAINT, cw); ry += rowh; }
        else { if (visible(rowh)) draw_text(E.ui, cx, ui_base(ry, rowh), "Ctrl+click marks, Shift+click compares", C_UI_FAINT, cw); ry += rowh; }
    }
    ry += (float)S(6);

    /* history of the file in the active tab */
    {
        std::string title = g.tl_path.empty() || g.tl_path[0] == '\x01' ? std::string("TIMELINE") : "TIMELINE  " + base_name(g.tl_path);
        section(3, title.c_str(), g.open_tl, g.tl_path.empty() || g.tl_path[0] == '\x01' ? -1 : (int)g.tl.size(), -1);
        if (g.open_tl) {
            const float th = (float)S(24);
            for (size_t i = 0; i < g.tl.size(); i++) {
                const GitCommit &c = g.tl[i];
                bool marked = !g.mark_hash.empty() && c.hash == g.mark_hash;
                if (visible(th)) {
                    if (hovered(H_GITTL, (int)i)) r_rect(E.r, full_x, ry, full_w, th, C_HOVER);
                    if (marked) r_rect(E.r, full_x, ry, (float)S(2), th, C_ACCENT);
                    std::string ago = git_ago(c.date, (long)time(nullptr));
                    float aw = text_width(E.ui, ago.c_str());
                    draw_dot(cx + (float)S(6), ry + th / 2, S(8), C_GUT_MOD);
                    draw_text(E.ui, cx + (float)S(20), ui_base(ry, th), c.subject.c_str(), C_UI, right - cx - (float)S(28) - aw);
                    draw_text(E.ui, right - aw, ui_base(ry, th), ago.c_str(), C_UI_FAINT, aw + 2);
                    hit(full_x, ry, full_w, th, H_GITTL, (int)i);
                }
                ry += th;
            }
            if (g.tl.empty()) { if (visible(rowh)) draw_text(E.ui, cx, ui_base(ry, rowh), g.tl_path.empty() || g.tl_path[0] == '\x01' ? "Open a tracked file to see its history" : "No history for this file yet", C_UI_FAINT, cw); ry += rowh; }
        }
    }
}

void draw_remote_view(float rail, float sw, float cx, float cw, float list_top, float bot, float &ry) {
    remote_ensure();
    g_rrows.clear();
    const float rowh = (float)S(26), full_x = rail + 1, full_w = sw - rail - 2, right = sw - (float)S(10);
    auto visible = [&](float h) { return ry + h >= list_top && ry <= bot; };
    if (visible(rowh)) {
        draw_text(E.ui, cx, ui_base(ry, rowh), "REMOTE EXPLORER", C_GROUP, cw, FS_BOLD);
        float bx = right - (float)S(22);
        if (hovered(H_RNEW, 0)) r_rect(E.r, bx - (float)S(3), ry + (float)S(2), (float)S(24), rowh - (float)S(4), C_HOVER);
        draw_icon(IC_PLUS, bx, ry + (rowh - (float)S(16)) / 2, S(16), C_UI_DIM);
        hit(bx - (float)S(3), ry + (float)S(2), (float)S(24), rowh - (float)S(4), H_RNEW, 0);
    }
    ry += rowh;
    if (g_rem.profiles.empty()) {
        if (visible(rowh * 3)) {
            draw_text(E.ui, cx, ui_base(ry, rowh), "No connections yet", C_UI, cw, FS_BOLD);
            draw_text(E.ui, cx, ui_base(ry + rowh, rowh), "SSH, SFTP, FTP, FTPS and SMB", C_UI_DIM, cw);
            float by = ry + rowh * 2 + (float)S(4), bh = (float)S(30);
            r_rect(E.r, cx, by, cw, bh, hovered(H_RNEW, 1) ? ui_mix(C_ACCENT, C_UI, 0.14) : C_ACCENT);
            float lw = text_width(E.ui, "New Connection");
            draw_text(E.ui, cx + (cw - lw) / 2, ui_base(by, bh), "New Connection", C_ON_ACCENT, cw);
            hit(cx, by, cw, bh, H_RNEW, 1);
        }
        ry += rowh * 3 + (float)S(10);
        return;
    }
    std::function<void(int, const std::string &, int)> emit = [&](int pi, const std::string &dir, int depth) {
        RConn &c = g_rem.conns[(size_t)pi];
        auto it = c.dirs.find(dir);
        if (it == c.dirs.end()) return;
        for (const RemoteEntry &e : it->second) {
            std::string full = dir == "." ? e.name : (dir.empty() || dir == "/" ? "/" : dir + "/") + e.name;
            if (dir.back() == '/' && dir.size() > 1) full = dir + e.name;
            int idx = (int)g_rrows.size();
            g_rrows.push_back({pi, full, e.dir ? 1 : 2});
            bool opened = e.dir && c.expanded.count(full);
            if (visible(rowh)) {
                if (hovered(H_RROW, idx)) r_rect(E.r, full_x, ry, full_w, rowh, C_HOVER);
                float x = cx + (float)depth * (float)S(12);
                if (e.dir) { draw_icon(opened ? IC_CHEV_D : IC_CHEV_R, x - (float)S(4), ry + (rowh - (float)S(16)) / 2, S(16), C_GROUP); draw_icon(IC_FOLDER, x + (float)S(12), ry + (rowh - (float)S(14)) / 2, S(14), C_UI_DIM); }
                else draw_dot(x + (float)S(8), ry + rowh / 2, S(8), file_color(e.name));
                draw_text(E.ui, x + (float)S(e.dir ? 30 : 20), ui_base(ry, rowh), e.name.c_str(), C_UI, right - x - (float)S(34));
                hit(full_x, ry, full_w, rowh, H_RROW, idx);
            }
            ry += rowh;
            if (opened) {
                if (c.dirs.count(full)) emit(pi, full, depth + 1);
            } else if (e.dir && c.loading.count(full)) { /* the arrow turns into a row of its own below */ }
            if (e.dir && c.loading.count(full)) { if (visible(rowh)) draw_text(E.ui, cx + (float)(depth + 1) * (float)S(12) + (float)S(20), ui_base(ry, rowh), "loading...", C_UI_FAINT, cw); ry += rowh; }
        }
        if (it->second.empty()) { if (visible(rowh)) draw_text(E.ui, cx + (float)depth * (float)S(12) + (float)S(20), ui_base(ry, rowh), "(empty)", C_UI_FAINT, cw); ry += rowh; }
    };
    for (size_t pi = 0; pi < g_rem.profiles.size(); pi++) {
        const RemoteProfile &p = g_rem.profiles[pi];
        RConn &c = g_rem.conns[pi];
        int idx = (int)g_rrows.size();
        g_rrows.push_back({(int)pi, remote_root(p), 0});
        if (visible(rowh)) {
            if (hovered(H_RROW, idx)) r_rect(E.r, full_x, ry, full_w, rowh, C_HOVER);
            draw_icon(c.connected && c.open ? IC_CHEV_D : IC_CHEV_R, cx - (float)S(4), ry + (rowh - (float)S(16)) / 2, S(16), C_GROUP);
            draw_dot(cx + (float)S(20), ry + rowh / 2, S(12), c.busy ? C_ACCENT : c.connected ? C_GIT_ADD : C_UI_FAINT);
            float pen = draw_text(E.ui, cx + (float)S(30), ui_base(ry, rowh), p.name.c_str(), C_UI, cw * 0.5f, FS_BOLD);
            std::string tag = p.proto;
            for (char &ch : tag) ch = (char)toupper((unsigned char)ch);
            float tw = text_width(E.ui, tag.c_str());
            draw_text(E.ui, right - tw, ui_base(ry, rowh), tag.c_str(), C_UI_FAINT, tw + 2);
            if (pen + (float)S(10) < right - tw - (float)S(8)) draw_text(E.ui, pen + (float)S(8), ui_base(ry, rowh), remote_label(p).c_str(), C_UI_FAINT, right - tw - pen - (float)S(16));
            hit(full_x, ry, full_w, rowh, H_RROW, idx);
        }
        ry += rowh;
        if (c.connected && c.open) emit((int)pi, remote_root(p), 1);
        else if (!c.connected && !c.busy) {
            if (visible(rowh) && (!c.err.empty())) draw_text(E.ui, cx + (float)S(20), ui_base(ry, rowh), c.err.c_str(), C_RED, cw - (float)S(20));
            if (!c.err.empty()) ry += rowh;
        }
    }
    ry += (float)S(8);
    if (visible(rowh)) { draw_text(E.ui, cx, ui_base(ry, rowh), "Right-click a connection for more.", C_UI_FAINT, cw); draw_text(E.ui, cx, ui_base(ry + rowh, rowh), "Saving a file uploads it.", C_UI_FAINT, cw); }
    ry += rowh * 2;
}

/* ---- outline: a cheap symbol scan of the active file (no parser): types, functions, markdown headings ---- */

struct OItem { std::string name; size_t line; int depth; char kind; };

struct OutlineCache { const Doc *d = nullptr; uint64_t rev = 0; std::vector<OItem> items; } g_outline;

bool ident_ch(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$' || c == ':' || c == '~' || c == '.'; }

void outline_scan(const Doc &d, std::vector<OItem> &out) {
    out.clear();
    bool md = std::string(lang_name(d.lang)) == "Markdown";
    static const char *mods[] = {"export ", "default ", "public ", "private ", "protected ", "static ", "async ", "pub ", "pub(crate) ", "final ", "abstract ", "inline ", "virtual ", "extern ", "unsafe ", "sealed ", "partial ", "open ", "override "};
    static const char *types[] = {"class ", "struct ", "enum ", "union ", "namespace ", "interface ", "trait ", "impl ", "mod ", "module ", "object ", "protocol ", "extension ", "type "};
    static const char *funcs[] = {"def ", "fn ", "func ", "function ", "function* ", "fun ", "sub ", "proc "};
    static const char *ctl[] = {"if", "for", "while", "switch", "return", "else", "do", "case", "catch", "sizeof", "delete", "new", "throw", "using", "typedef", "goto", "elif", "with", "assert", "await", "yield", "not", "and", "or"};
    std::vector<int> stack;   /* indents of the enclosing items: depth = how many are shallower */
    std::string t;
    size_t n = std::min<size_t>(d.buf.lines(), 30000);
    for (size_t l = 0; l < n && out.size() < 3000; l++) {
        if (d.buf.line_len(l) > 300) continue;
        d.buf.line_text(l, t);
        int ind = 0; size_t i = 0;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) { ind += t[i] == '\t' ? 4 : 1; i++; }
        if (i >= t.size()) continue;
        std::string s = t.substr(i);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
        if (s.empty()) continue;
        auto push = [&](const std::string &name, char kind) {
            while (!stack.empty() && stack.back() >= ind) stack.pop_back();
            out.push_back({name, l, std::min<int>((int)stack.size(), 4), kind});
            stack.push_back(ind);
        };
        if (md) {
            if (s[0] != '#') continue;
            size_t h = 0; while (h < s.size() && s[h] == '#') h++;
            if (h > 6 || h >= s.size() || s[h] != ' ') continue;
            out.push_back({s.substr(h + 1), l, (int)h - 1, 'H'});
            continue;
        }
        if (s[0] == '#' || s[0] == '/' || s[0] == '*' || s[0] == '}' || s[0] == '"' || s[0] == '\'') continue;
        std::string k = s;
        for (bool again = true; again;) { again = false; for (const char *m : mods) { size_t ml = strlen(m); if (k.compare(0, ml, m) == 0) { k.erase(0, ml); again = true; } } }
        auto take = [&](size_t from) { size_t e = from; while (e < k.size() && ident_ch(k[e])) e++; return k.substr(from, e - from); };
        bool done = false;
        for (const char *ty : types) {
            size_t tl = strlen(ty);
            if (k.compare(0, tl, ty) != 0) continue;
            size_t from = tl;
            if (k.compare(from, 6, "class ") == 0) from += 6; else if (k.compare(from, 7, "struct ") == 0) from += 7;
            std::string nm;
            if (!strcmp(ty, "impl ")) { nm = k.substr(from); size_t b = nm.find('{'); if (b != std::string::npos) nm.erase(b); while (!nm.empty() && nm.back() == ' ') nm.pop_back(); }
            else nm = take(from);
            if (nm.empty() || (k.back() == ';' && k.find('{') == std::string::npos)) { done = true; break; }
            push(nm, 'C'); done = true; break;
        }
        if (done) continue;
        for (const char *fn : funcs) {
            size_t fl = strlen(fn);
            if (k.compare(0, fl, fn) != 0) continue;
            size_t from = fl;
            if (k[from] == '(') { size_t c = k.find(')', from); if (c == std::string::npos) break; from = c + 1; while (from < k.size() && k[from] == ' ') from++; }   /* Go receiver */
            std::string nm = take(from);
            if (!nm.empty()) push(nm, 'F');
            done = true; break;
        }
        if (done) continue;
        /* C-like definitions: "type name(...) {", "Class::method(...) const", "name(...) {" inside a body */
        char last = k.back();
        if (!(isalpha((unsigned char)k[0]) || k[0] == '_' || k[0] == '~')) continue;
        if (last == ';' || last == ',' || last == '=' || last == '(') continue;
        size_t par = k.find('(');
        if (par == std::string::npos || par == 0) continue;
        if (k.find('=') < par || k.find('"') < par) continue;
        size_t e = par; while (e > 0 && k[e - 1] == ' ') e--;
        size_t b = e; while (b > 0 && ident_ch(k[b - 1])) b--;
        if (b == e) continue;
        std::string nm = k.substr(b, e - b);
        bool ctlw = false;
        size_t fw = 0; while (fw < k.size() && ident_ch(k[fw])) fw++;
        std::string first = k.substr(0, fw);
        for (const char *c : ctl) if (first == c) ctlw = true;
        if (ctlw) continue;
        bool typed = b > 0, scoped = nm.find("::") != std::string::npos;
        bool ret_paren = last == ')' || last == '{' || k.find(") const") != std::string::npos || k.find(") noexcept") != std::string::npos || k.find(") override") != std::string::npos;
        if (!ret_paren) continue;
        if (!typed && !scoped && !(last == '{' && k.size() >= 3 && k[k.size() - 2] == ' ' && k[k.size() - 3] == ')' && k.find("=>") == std::string::npos && k.find('\'') == std::string::npos)) continue;
        push(nm, 'F');
    }
}

const std::vector<OItem> &outline_for(const Doc &d) {
    if (g_outline.d != &d || g_outline.rev != d.buf.revision()) { g_outline.d = &d; g_outline.rev = d.buf.revision(); outline_scan(d, g_outline.items); }
    return g_outline.items;
}

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
    const char *names[4] = {"FILES", "SEARCH", "GIT", "REMOTE"};
    for (int i = 0; i < 4; i++) {
        const AtlasGlyph *g = vlabel(names[i], E.ui, FS_BOLD);
        if (!g) continue;
        bool badge = i == 2 && E.git.repo && !E.git.changes.empty();
        float th = (float)g->h + (float)S(28) + (badge ? (float)S(18) : 0.f);
        bool on = (int)E.view == i;
        if (on) { r_rect(E.r, 0, ty, rail, th, C_SELROW); r_rect(E.r, 0, ty, (float)S(2), th, C_ACCENT); }
        else if (hovered(H_ACTIVITY, i)) r_rect(E.r, 0, ty, rail, th, C_HOVER);
        RInst inst;
        r_make_glyph(E.r, &inst, g, (rail - (float)g->w) / 2 + 1, ty + ((th - (badge ? (float)S(18) : 0.f)) - (float)g->h) / 2, on ? C_ACCENT : C_UI_DIM);
        r_push(E.r, &inst, 1);
        if (badge) {
            std::string nb = std::to_string(E.git.changes.size());
            float bw = std::max((float)S(16), text_width(E.ui, nb.c_str()) + (float)S(8));
            draw_badge((rail + bw) / 2, ty + th - (float)S(22), (float)S(16), (int)E.git.changes.size(), C_ACCENT, C_ON_ACCENT);
        }   /* how many files changed, like the badge on VS Code's source control icon */
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
        float orh = (float)S(24);
        auto section = [&](const std::string &label, bool open, int id) {
            float y0 = ry;
            group(label, true, open, 0);
            hit(rail + 1, y0, sw - rail - 2, (float)S(24), H_SECT, id);
        };
        if (E.docs.size() > 1 || !D().pristine()) {   /* OPEN EDITORS: every tab, with the unsaved mark and a close button */
            section(sfmt("OPEN EDITORS  %zu", E.docs.size()), E.opened_open, 0);
            if (E.opened_open) {
                size_t rl = E.tree.root().size() + 1;
                for (size_t i = 0; i < E.docs.size(); i++, ry += orh) {
                    if (ry + orh < list_top || ry > bot) continue;
                    const Doc &dd = *E.docs[i];
                    bool cur = (int)i == E.active, hv = hovered(H_OPENED, (int)i) || hovered(H_TABCLOSE, 1000 + (int)i);
                    if (cur) { r_rect(E.r, rail + 1, ry, sw - rail - 2, orh, C_SELROW); r_rect(E.r, rail + 1, ry, (float)S(2), orh, C_ACCENT); }
                    else if (hv) r_rect(E.r, rail + 1, ry, sw - rail - 2, orh, C_HOVER);
                    float cxr = sw - (float)S(30);
                    std::string nm = dd.name();
                    float pen = draw_text(E.ui, cx + (float)S(14), ui_base(ry, orh), nm.c_str(), cur ? C_ACCENT : C_UI, cxr - cx - (float)S(14), dd.preview ? FS_ITALIC : FS_REGULAR);
                    if (!dd.path.empty() && !dd.title.size()) {
                        std::string rel = dd.path.size() > rl && dd.path.compare(0, rl, E.tree.root() + "/") == 0 ? dd.path.substr(rl) : dd.path;
                        size_t sl = rel.rfind('/');
                        if (sl != std::string::npos && cxr - pen - (float)S(16) > (float)S(30)) draw_text(E.ui, pen + (float)S(8), ui_base(ry, orh), rel.substr(0, sl).c_str(), C_UI_FAINT, cxr - pen - (float)S(12));
                    }
                    bool mod = dd.modified();
                    float ix = sw - (float)S(26), iy = ry + (orh - (float)S(16)) / 2;
                    if (mod && !hovered(H_TABCLOSE, 1000 + (int)i)) draw_icon(IC_DOT, ix, iy, S(16), C_UI);
                    else if (hv || cur) draw_icon(IC_CLOSE, ix, iy, S(16), hovered(H_TABCLOSE, 1000 + (int)i) ? C_UI : C_UI_DIM);
                    hit(rail + 1, ry, sw - rail - 2 - (float)S(30), orh, H_OPENED, (int)i);
                    hit(sw - (float)S(30), ry, (float)S(30), orh, H_TABCLOSE, 1000 + (int)i);
                }
            }
        }
        if (E.outline_on && !D().dv && !D().mv) {   /* OUTLINE: symbols of the active file, click to jump */
            const std::vector<OItem> &oi = outline_for(D());
            section(sfmt("OUTLINE  %zu", oi.size()), E.outline_open, 1);
            if (E.outline_open) {
                if (oi.empty()) { draw_text(E.ui, cx + (float)S(14), ui_base(ry, orh), "No symbols found", C_UI_FAINT, cw); ry += orh; }
                size_t curline = D().buf.line_of(D().cur);
                long here = -1;
                for (size_t i = 0; i < oi.size(); i++) if (oi[i].line <= curline) here = (long)i;
                for (size_t i = 0; i < oi.size(); i++, ry += orh) {
                    if (ry + orh < list_top || ry > bot) continue;
                    if ((long)i == here) { r_rect(E.r, rail + 1, ry, sw - rail - 2, orh, C_SELROW); r_rect(E.r, rail + 1, ry, (float)S(2), orh, C_ACCENT); }
                    else if (hovered(H_OUTLINE, (int)i)) r_rect(E.r, rail + 1, ry, sw - rail - 2, orh, C_HOVER);
                    const OItem &it = oi[i];
                    float x = cx + (float)S(14 + 12 * it.depth);
                    const char *tag = it.kind == 'C' ? "C" : it.kind == 'H' ? "#" : "f";
                    uint32_t tc = it.kind == 'C' ? C_ORANGE : it.kind == 'H' ? C_UI_DIM : C_ACCENT;
                    draw_text(E.ui, x, ui_base(ry, orh), tag, tc, (float)S(14), FS_BOLD);
                    draw_text(E.ui, x + (float)S(16), ui_base(ry, orh), it.name.c_str(), (long)i == here ? C_ACCENT : C_UI, sw - x - (float)S(24));
                    hit(rail + 1, ry, sw - rail - 2, orh, H_OUTLINE, (int)i);
                }
            }
        }
        std::string name = E.tree.root_name();
        for (char &c : name) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        group(name, true, E.root_open, H_ROOT);
        if (E.root_open) {
            const std::vector<Tree::Row> &rows = E.tree.rows();
            const float tree_y0 = ry;
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
                char gs = 0;
                { auto di = E.deco.find(r.path); if (di != E.deco.end()) gs = di->second; }
                if (r.ghost) gs = 'D';
                uint32_t ncol = cur ? C_ACCENT : gs ? status_color(gs) : C_UI;
                float badge_w = gs && !r.dir ? (float)S(22) : gs ? (float)S(18) : 0.f;
                float nx = tx + (float)S(open_idx >= 0 && !r.dir ? 4 : 0);
                float npen = draw_text(E.ui, nx, ui_base(ry, rowh), r.name.c_str(), ncol, sw - tx - (float)S(8) - badge_w);
                if (r.ghost) r_rect(E.r, nx, ry + rowh / 2, std::max(0.f, npen - nx), std::max(1.f, E.scale), ncol);   /* struck through: deleted */
                if (gs && !r.dir) draw_text(E.ui, sw - (float)S(18), ui_base(ry, rowh), status_letter(gs), ncol, (float)S(14), FS_BOLD);
                else if (gs) draw_dot(sw - (float)S(14), ry + rowh / 2, S(8), ncol);
                hit(rail + 1, ry, sw - rail - 2, rowh, H_TREE, (int)i);
            }
            /* sticky folders: the parents of the first visible row stay pinned under the filter box while you scroll a long folder */
            if (tree_y0 < list_top && !rows.empty()) {
                std::vector<size_t> chain;
                for (int pass = 0; pass < 4; pass++) {
                    size_t f = (size_t)std::max(0.f, std::ceil((list_top + (float)chain.size() * rowh - tree_y0) / rowh));
                    if (f >= rows.size()) { chain.clear(); break; }
                    std::vector<size_t> c2;
                    int dpt = rows[f].depth;
                    for (size_t j = f; j-- > 0 && dpt > 0;) if (rows[j].dir && rows[j].depth < dpt) { dpt = rows[j].depth; if (tree_y0 + (float)j * rowh < list_top + (float)c2.size() * 0.f) c2.push_back(j); }
                    std::reverse(c2.begin(), c2.end());
                    if (c2.size() > 3) c2.erase(c2.begin(), c2.end() - 3);
                    if (c2.size() == chain.size()) { chain = c2; break; }
                    chain = c2;
                }
                for (size_t k = 0; k < chain.size(); k++) {
                    const Tree::Row &r = rows[chain[k]];
                    float sy = list_top + (float)k * rowh, x = cx + (float)S(14 * r.depth);
                    r_rect(E.r, rail + 1, sy, sw - rail - 2, rowh, C_CHROME);
                    if (hovered(H_TREE, (int)chain[k])) r_rect(E.r, rail + 1, sy, sw - rail - 2, rowh, C_HOVER);
                    draw_icon(IC_CHEV_D, x - (float)S(2), sy + (rowh - (float)S(16)) / 2, S(16), C_UI_DIM);
                    draw_text(E.ui, x + (float)S(18), ui_base(sy, rowh), r.name.c_str(), C_UI, sw - x - (float)S(26), FS_BOLD);
                    if (k + 1 == chain.size()) r_rect(E.r, rail + 1, sy + rowh - 1, sw - rail - 2, 1, C_BORDER);
                    hit(rail + 1, sy, sw - rail - 2, rowh, H_TREE, (int)chain[k]);
                }
            }
        }
        float content = (ry + E.side_scroll) - list_top;
        float maxs = std::max(0.f, content - (bot - list_top) + rowh);
        if (E.side_scroll > maxs) E.side_scroll = maxs;
    } else if (E.view == V_REMOTE) {
        draw_remote_view(rail, sw, cx, cw, list_top, bot, ry);
        float content = (ry + E.side_scroll) - list_top;
        float maxs = std::max(0.f, content - (bot - list_top) + (float)S(20));
        if (E.side_scroll > maxs) E.side_scroll = maxs;
    } else if (!E.git.repo) {
        draw_text(E.ui, cx, ui_base(ry, rowh), E.git.available ? "No git repository here" : "git is not installed", C_UI, cw, FS_BOLD);
        if (E.git.available) {
            draw_text(E.ui, cx, ui_base(ry + rowh, rowh), "Open a folder, or a file,", C_UI_DIM, cw);
            draw_text(E.ui, cx, ui_base(ry + rowh * 2 - (float)S(4), rowh), "that is inside a repository.", C_UI_DIM, cw);
        }
    } else {
        draw_git_view(rail, sw, cx, cw, list_top, bot, ry);
        float content = (ry + E.side_scroll) - list_top;
        float maxs = std::max(0.f, content - (bot - list_top) + (float)S(20));
        if (E.side_scroll > maxs) E.side_scroll = maxs;
    }
    r_clip_off(E.r);
}


/* ---- the diff tab: side by side or inline, folded unchanged regions, change navigation ---- */

int diff_rows_visible(const Layout &L) { return std::max(1, (L.ed_bottom - L.text_y - S(30)) / E.rowh); }

void diff_clamp(DiffView &v, const Layout &L) {
    long maxtop = std::max<long>(0, (long)v.vrows.size() - 1);
    if (v.top < 0) v.top = 0;
    if (v.top > maxtop) v.top = maxtop;
    (void)L;
}

void diff_goto_change(DiffView &v, int k, const Layout &L) {
    if (v.vdirty) diff_layout(v, v.vinline);
    if (k < 0 || (size_t)k >= v.change_v.size() || v.change_v[(size_t)k] < 0) return;
    v.cur_change = k;
    v.top = std::max<long>(0, v.change_v[(size_t)k] - diff_rows_visible(L) / 3);
    diff_clamp(v, L);
}

void diff_step(DiffView &v, int dir, const Layout &L) {
    if (v.m.hunks.empty()) return;
    int n = (int)v.m.hunks.size();
    int k = v.cur_change < 0 ? (dir > 0 ? 0 : n - 1) : (v.cur_change + dir + n) % n;
    diff_goto_change(v, k, L);
}

/* one line of code with syntax colours at (x, y): `left` columns scrolled off, at most `cols` shown */
void emit_code(float x, float y, const std::string &text, int left, int cols, const Lang *lang, uint8_t state) {
    static std::vector<RInst> v;
    v.clear();
    if (E.tok.size() < text.size() + 1) E.tok.resize(text.size() + 64);
    if (lang != lang_plain()) lex_line(lang, text.data(), text.size(), state, E.tok.data());
    else memset(E.tok.data(), 0, text.size());
    int col = 0;
    for (size_t i = 0; i < text.size() && col < left + cols;) {
        size_t a, i0 = i;
        uint32_t cp = u8_decode(text.data() + i, text.size() - i, &a);
        i += a;
        if (cp == '\t') { col = (col / TAB_W + 1) * TAB_W; continue; }
        if (col >= left && cp != ' ') {
            const AtlasGlyph *g = atlas_glyph(E.atlas, E.font, cp, FS_REGULAR);
            if (g && !g->blank) { RInst inst; r_make_glyph(E.r, &inst, g, (float)((col - left) * E.cw), (float)E.rowasc, TOKCOL[E.tok[i0]]); v.push_back(inst); }
        }
        col++;
    }
    if (!v.empty()) r_push_at(E.r, v.data(), (int)v.size(), x, y);
}

void draw_diff(const Layout &L, float ex, float ew) {
    Doc &d = D();
    DiffView &v = *d.dv;
    const float tb_h = (float)S(30), body_y = (float)L.text_y + tb_h;
    const int rows = diff_rows_visible(L);
    bool inl = v.mode == 2 || (v.mode == 0 && ew < (float)S(900));
    if (v.vdirty || v.vinline != inl) diff_layout(v, inl);
    if (v.top < 0) { v.top = 0; if (v.cur_change >= 0) diff_goto_change(v, v.cur_change, L); }
    diff_clamp(v, L);
    const DiffModel &m = v.m;

    /* toolbar: what is compared, the change counter and the buttons */
    r_rect(E.r, ex, (float)L.text_y, ew, tb_h, C_CHROME);
    r_rect(E.r, ex, (float)L.text_y + tb_h - 1, ew, 1, C_BORDER);
    float by = (float)L.text_y + (tb_h - (float)S(22)) / 2, bh = (float)S(22), rx = ex + ew - (float)S(10);
    auto button = [&](int id, const char *label, Icon ic, bool has_icon) {
        float tw = label ? text_width(E.ui, label) : 0.f;
        float w = (float)S(12) + (has_icon ? (float)S(16) : 0.f) + (label ? tw + (has_icon ? (float)S(4) : 0.f) : 0.f);
        float x = rx - w;
        if (hovered(H_DIFFBTN, id)) r_rect(E.r, x, by, w, bh, C_HOVER);
        float px = x + (float)S(6);
        if (has_icon) { draw_icon(ic, px, by + (bh - (float)S(16)) / 2, S(16), C_UI_DIM); px += (float)S(20); }
        if (label) draw_text(E.ui, px, ui_base(by, bh), label, C_UI_DIM, tw + 2);
        hit(x, by, w, bh, H_DIFFBTN, id);
        rx = x - (float)S(2);
    };
    button(3, "Open File", IC_OPEN_FILE, true);
    if (v.right_rev == "WORK" && (v.left_rev == ":" || v.left_rev.empty())) { button(5, "Revert Change", IC_UNDO, true); button(4, "Stage Change", IC_PLUS, true); }
    else if (v.left_rev == "HEAD" && v.right_rev == ":") button(6, "Unstage Change", IC_MINUS, true);
    button(2, v.vinline ? "Side by side" : "Inline", IC_FILE, false);
    button(1, nullptr, IC_ARROW_D, true);
    button(0, nullptr, IC_ARROW_U, true);
    {
        std::string cnt = m.hunks.empty() ? "no changes" : v.cur_change >= 0 ? sfmt("change %d of %zu", v.cur_change + 1, m.hunks.size()) : sfmt("%zu change%s", m.hunks.size(), m.hunks.size() == 1 ? "" : "s");
        float cw2 = text_width(E.ui, cnt.c_str());
        draw_text(E.ui, rx - cw2 - (float)S(6), ui_base(by, bh), cnt.c_str(), C_UI_FAINT, cw2 + 2);
        rx -= cw2 + (float)S(16);
    }
    {
        std::string lab = v.left_label + "  \xE2\x86\x94  " + v.right_label;
        float lw = draw_text(E.ui, ex + (float)S(12), ui_base(by, bh), lab.c_str(), C_UI, std::max(0.f, rx - ex - (float)S(24)), FS_BOLD);
        if (!v.info.empty() && lw + (float)S(30) < rx) draw_text(E.ui, lw + (float)S(14), ui_base(by, bh), v.info.c_str(), C_UI_FAINT, rx - lw - (float)S(20));
    }

    if (v.binary || v.too_big || m.hunks.empty()) {
        const char *msg = v.binary ? "Binary files differ" : v.too_big ? "File too large to compare" : "No differences: both sides are identical";
        float w = text_width(E.big, msg);
        draw_text(E.big, ex + (ew - w) / 2, body_y + (float)S(80), msg, C_UI_FAINT, ew);
        return;
    }

    int nd = 1;
    for (size_t n = std::max(m.a.size(), m.b.size()); n >= 10; n /= 10) nd++;
    const float numw = (float)((nd + 2) * E.cw);
    const float half = v.vinline ? ew : (ew - 1) / 2;
    const float tx_a = ex + numw + (float)S(6), tx_b = ex + half + 1 + numw + (float)S(6);   /* text origin: left column, right column */
    const float itx = ex + 2 * numw + (float)S(6);                                           /* inline */
    const int cols = std::max(1, (int)((v.vinline ? ew - 2 * numw - (float)S(24) : half - numw - (float)S(24)) / (float)E.cw));
    int nrows = (int)std::min<long>(rows, (long)v.vrows.size() - v.top);

    auto number = [&](float x0, int line, uint32_t col, float y) {
        if (line < 0) return;
        char nb[24];
        snprintf(nb, sizeof nb, "%d", line + 1);
        draw_text(E.font, x0 + (numw - (float)S(14) - (float)(strlen(nb) * (size_t)E.cw)) / 2 + (float)S(2), y + (float)E.rowasc, nb, col, numw);
    };
    auto range_rect = [&](float tx, const std::string &t, int b0, int b1, uint32_t col, float y) {
        int c0 = std::max(0, col_of(t, (size_t)b0) - v.left), c1 = std::min(cols, col_of(t, (size_t)b1) - v.left);
        if (c1 > c0) r_rect(E.r, tx + (float)(c0 * E.cw), y, (float)((c1 - c0) * E.cw), (float)E.rowh, col);
    };
    auto hatch = [&](float x0, float w, float y) {
        r_clip(E.r, (int)x0, (int)y, (int)w, E.rowh);
        for (float hx = x0; hx < x0 + w; hx += (float)E.rowh) draw_icon(IC_HATCH, hx, y, E.rowh, C_DIFF_FILL);
        r_clip_off(E.r);
    };

    if (!v.vinline) r_rect(E.r, ex + half, body_y, 1, (float)(rows * E.rowh), C_BORDER);   /* under the rows, so a fold bar covers it */
    /* the change under the mouse gets stage / revert (or unstage) buttons on its first visible row, like VS Code */
    int hov_hunk = -1;
    const bool can_wt = v.right_rev == "WORK" && (v.left_rev == ":" || v.left_rev.empty()), can_unstage = v.left_rev == "HEAD" && v.right_rev == ":";
    if ((can_wt || can_unstage) && E.mx >= ex && E.mx < ex + ew && E.my >= body_y && E.my < body_y + (float)(nrows * E.rowh) && (E.mode == M_EDIT || E.mode == M_FIND)) {
        const DiffView::VRow &mv = v.vrows[(size_t)(v.top + (int)((E.my - body_y) / (float)E.rowh))];
        if (mv.sub != 3 && m.rows[(size_t)mv.row].kind != DK_SAME) hov_hunk = m.rows[(size_t)mv.row].hunk;
    }
    bool hov_drawn = false;
    for (int r = 0; r < nrows; r++) {
        const DiffView::VRow &vr = v.vrows[(size_t)(v.top + r)];
        float y = body_y + (float)(r * E.rowh);
        if (vr.sub == 3) {   /* a folded stretch of unchanged lines */
            bool hv = hovered(H_DIFFFOLD, (int)(v.top + r));
            r_rect(E.r, ex, y, ew, (float)E.rowh, hv ? C_SELROW : C_DIFF_FOLD);
            std::string t = sfmt("%d unchanged lines hidden", vr.n);
            float tw = text_width(E.ui, t.c_str());
            float cx0 = ex + (ew - tw) / 2;
            draw_icon(IC_CHEV_R, cx0 - (float)S(20), y + ((float)E.rowh - (float)S(16)) / 2, S(16), C_UI_DIM);
            draw_text(E.ui, cx0, ui_base(y, (float)E.rowh), t.c_str(), C_UI_DIM, tw + 2);
            hit(ex, y, ew, (float)E.rowh, H_DIFFFOLD, (int)(v.top + r));
            continue;
        }
        const DiffRow &row = m.rows[(size_t)vr.row];
        bool changed = row.kind != DK_SAME;
        const std::string *ta = row.a >= 0 ? &m.a[(size_t)row.a] : nullptr, *tb = row.b >= 0 ? &m.b[(size_t)row.b] : nullptr;
        bool cur = changed && v.cur_change >= 0 && row.hunk == v.cur_change;
        auto hunk_buttons = [&]() {
            if (hov_drawn || !changed || row.hunk != hov_hunk) return;
            hov_drawn = true;
            const float bs = (float)E.rowh, gap = (float)S(2);
            const int nb = can_wt ? 2 : 1;
            float bx = ex + ew - (float)S(16) - (float)nb * (bs + gap);
            r_rect(E.r, bx - gap, y, (float)nb * (bs + gap) + gap, bs, C_CHROME);
            r_rect(E.r, bx - gap, y, (float)nb * (bs + gap) + gap, 1, C_BORDER);
            r_rect(E.r, bx - gap, y + bs - 1, (float)nb * (bs + gap) + gap, 1, C_BORDER);
            const int ops[2] = {can_wt ? 4 : 6, 5};
            const Icon ics[2] = {can_wt ? IC_PLUS : IC_MINUS, IC_UNDO};
            for (int k = 0; k < nb; k++) {
                int id = row.hunk * 8 + ops[k];
                float x = bx + (float)k * (bs + gap);
                if (hovered(H_DIFFHUNK, id)) r_rect(E.r, x, y, bs, bs, C_HOVER);
                draw_icon(ics[k], x + (bs - (float)S(16)) / 2, y + (bs - (float)S(16)) / 2, S(16), C_UI);
                hit(x, y, bs, bs, H_DIFFHUNK, id);
            }
        };
        if (v.vinline) {
            bool del = vr.sub == 1, add = vr.sub == 2;
            const std::string *t = del ? ta : add ? tb : tb;
            int ln = del ? row.a : row.b;
            uint32_t bg = del ? C_DIFF_DEL : add ? C_DIFF_ADD : 0;
            if (bg) r_rect(E.r, ex, y, ew, (float)E.rowh, bg);
            if (row.kind == DK_MOD) { if (del) range_rect(itx, *ta, row.ia0, row.ia1, C_DIFF_DEL_TXT, y); else range_rect(itx, *tb, row.ib0, row.ib1, C_DIFF_ADD_TXT, y); }
            if (cur) r_rect(E.r, ex, y, (float)S(2), (float)E.rowh, C_ACCENT);
            number(ex, vr.sub == 0 ? row.a : del ? row.a : -1, C_GUTTER, y);
            number(ex + numw, vr.sub == 0 ? row.b : add ? row.b : -1, C_GUTTER, y);
            if (del || add) draw_text(E.font, ex + 2 * numw - (float)S(16), y + (float)E.rowasc, del ? "-" : "+", del ? C_GUT_DEL : C_GUT_ADD, (float)S(16));
            r_clip(E.r, (int)itx, (int)y, (int)(ex + ew - itx), E.rowh);
            if (t) emit_code(itx, y, *t, v.left, cols, v.lang, del ? v.sa[(size_t)ln] : v.sb[(size_t)ln]);
            r_clip_off(E.r);
            hunk_buttons();
            continue;
        }
        /* side by side: left column (old), divider, right column (new) */
        uint32_t bga = row.kind == DK_MOD || row.kind == DK_DEL ? C_DIFF_DEL : 0, bgb = row.kind == DK_MOD || row.kind == DK_ADD ? C_DIFF_ADD : 0;
        if (bga) r_rect(E.r, ex, y, half, (float)E.rowh, bga);
        if (bgb) r_rect(E.r, ex + half + 1, y, half, (float)E.rowh, bgb);
        if (!ta) hatch(ex, half, y);
        if (!tb) hatch(ex + half + 1, half, y);
        if (row.kind == DK_MOD) { range_rect(tx_a, *ta, row.ia0, row.ia1, C_DIFF_DEL_TXT, y); range_rect(tx_b, *tb, row.ib0, row.ib1, C_DIFF_ADD_TXT, y); }
        if (cur) { r_rect(E.r, ex, y, (float)S(2), (float)E.rowh, C_ACCENT); r_rect(E.r, ex + half + 1, y, (float)S(2), (float)E.rowh, C_ACCENT); }
        number(ex, row.a, C_GUTTER, y);
        number(ex + half + 1, row.b, C_GUTTER, y);
        r_clip(E.r, (int)tx_a, (int)y, (int)(ex + half - tx_a), E.rowh);
        if (ta) emit_code(tx_a, y, *ta, v.left, cols, v.lang, v.sa[(size_t)row.a]);
        r_clip_off(E.r);
        r_clip(E.r, (int)tx_b, (int)y, (int)(ex + ew - tx_b), E.rowh);
        if (tb) emit_code(tx_b, y, *tb, v.left, cols, v.lang, v.sb[(size_t)row.b]);
        r_clip_off(E.r);
        hunk_buttons();
    }

    /* scrollbar with the changes marked on the track, like VS Code's overview ruler */
    {
        float track_h = (float)(rows * E.rowh), sx = (float)(E.W - S(10));
        long total = (long)v.vrows.size();
        if (total > rows) {
            float th = std::max((float)S(24), track_h * (float)rows / (float)total), ty = body_y + (track_h - th) * (float)v.top / (float)std::max<long>(1, total - rows);
            for (size_t k = 0; k < v.change_v.size(); k++) {
                if (v.change_v[k] < 0) continue;
                const DiffHunk &h = m.hunks[k];
                uint32_t col = h.an == 0 ? C_GUT_ADD : h.bn == 0 ? C_GUT_DEL : C_GUT_MOD;
                r_rect(E.r, sx, body_y + track_h * (float)v.change_v[k] / (float)total, (float)S(8), std::max(2.f, E.scale * 2), col);
            }
            r_rect(E.r, sx + (float)S(2), ty, (float)S(6), th, ui_mix(C_EDITOR, C_FG, 0.20));
        }
    }
}

bool diff_key(const SDL_KeyboardEvent &k, bool ctrl, bool shift) {   /* keys in a diff tab; true when consumed */
    DiffView &v = *D().dv;
    Layout L = layout();
    int page = std::max(1, diff_rows_visible(L) - 2);
    SDL_Keycode sym = k.keysym.sym;
    bool alt = k.keysym.mod & KMOD_ALT;
    if (ctrl || alt) {
        if (sym == SDLK_SLASH || sym == SDLK_RIGHTBRACKET || sym == SDLK_LEFTBRACKET || sym == SDLK_d || sym == SDLK_l || sym == SDLK_k || sym == SDLK_RETURN || sym == SDLK_BACKSPACE || sym == SDLK_DELETE) return !(alt && !ctrl && false);
        if (ctrl && sym == SDLK_HOME) { v.top = 0; touch(); return true; }
        if (ctrl && sym == SDLK_END) { v.top = (long)v.vrows.size(); diff_clamp(v, L); touch(); return true; }
        return false;
    }
    switch (sym) {
    case SDLK_UP: v.top -= 1; break;
    case SDLK_DOWN: v.top += 1; break;
    case SDLK_PAGEUP: v.top -= page; break;
    case SDLK_PAGEDOWN: v.top += page; break;
    case SDLK_HOME: v.top = 0; break;
    case SDLK_END: v.top = (long)v.vrows.size(); break;
    case SDLK_LEFT: v.left = std::max(0, v.left - 4); break;
    case SDLK_RIGHT: v.left += 4; break;
    case SDLK_F7: diff_step(v, shift ? -1 : 1, L); touch(); return true;
    case SDLK_i: v.mode = v.vinline ? 1 : 2; v.vdirty = true; break;
    case SDLK_e: v.all_open = !v.all_open; v.vdirty = true; break;
    case SDLK_o: case SDLK_RETURN: case SDLK_KP_ENTER: {
        std::string p = v.root + "/" + v.path;
        struct stat st;
        if (stat(p.c_str(), &st) == 0) open_path(p); else E.status_msg = "the file is not on disk any more";
        touch();
        return true;
    }
    case SDLK_BACKSPACE: case SDLK_DELETE: case SDLK_TAB: return true;
    default: return false;
    }
    diff_clamp(v, L);
    touch();
    return true;
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

/* ---- merge conflicts in the editor: the <<<<<<< ======= >>>>>>> blocks of a file git lists as conflicted ---- */

const std::vector<Conflict> *conflicts_for(Doc &d) {
    if (d.dv || d.mv || !E.git.repo || d.path.empty() || d.buf.size() > (size_t)8 << 20) return nullptr;
    ConflictCache &c = g_conf[&d];
    if (c.rev == d.buf.revision() && c.listed) return &c.v;
    std::string rel = rel_to_root(d.path);
    bool conflicted = false;
    for (const GitChange &g : E.git.changes) if (g.path == rel && (g.x == 'U' || g.y == 'U' || (g.x == 'A' && g.y == 'A') || (g.x == 'D' && g.y == 'D'))) { conflicted = true; break; }
    if (!conflicted) { c.v.clear(); c.rev = d.buf.revision(); c.listed = true; return nullptr; }
    c.v.clear();
    std::string t;
    Conflict cur;
    for (size_t i = 0, n = d.buf.lines(); i < n; i++) {
        if (d.buf.line_len(i) < 7) continue;
        d.buf.line_text(i, t);
        if (t.compare(0, 7, "<<<<<<<") == 0 && (t.size() == 7 || t[7] == ' ')) { cur = Conflict(); cur.a = (long)i; }
        else if (cur.a >= 0 && t.compare(0, 7, "|||||||") == 0 && cur.mid < 0) cur.base = (long)i;
        else if (cur.a >= 0 && t == "=======") cur.mid = (long)i;
        else if (cur.a >= 0 && cur.mid >= 0 && t.compare(0, 7, ">>>>>>>") == 0 && (t.size() == 7 || t[7] == ' ')) { cur.b = (long)i; c.v.push_back(cur); cur = Conflict(); }
    }
    c.rev = d.buf.revision(); c.listed = true;
    return c.v.empty() ? nullptr : &c.v;
}

/* 0 none, 1 "<<<<<<<" line, 2 current (ours), 3 "|||||||" line, 4 base, 5 "=======" line, 6 incoming (theirs), 7 ">>>>>>>" line */
int conflict_kind(const std::vector<Conflict> &v, long line, size_t *idx) {
    size_t lo = 0, hi = v.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (v[m].b < line) lo = m + 1; else hi = m; }
    if (lo >= v.size() || line < v[lo].a) return 0;
    const Conflict &c = v[lo];
    *idx = lo;
    if (line == c.a) return 1;
    if (line == c.b) return 7;
    if (line == c.mid) return 5;
    if (c.base >= 0 && line == c.base) return 3;
    if (line < (c.base >= 0 ? c.base : c.mid)) return 2;
    if (c.base >= 0 && line < c.mid) return 4;
    return 6;
}

void conflict_accept(Doc &d, size_t idx, int op) {   /* 0 current, 1 incoming, 2 both */
    const std::vector<Conflict> *vp = conflicts_for(d);
    if (!vp || idx >= vp->size()) return;
    Conflict c = (*vp)[idx];
    std::string t, text;
    auto take = [&](long from, long to) { for (long i = from; i < to; i++) { d.buf.line_text((size_t)i, t); text += t + "\n"; } };
    long ours_end = c.base >= 0 ? c.base : c.mid;
    if (op == 0 || op == 2) take(c.a + 1, ours_end);
    if (op == 1 || op == 2) take(c.mid + 1, c.b);
    size_t nl = d.buf.lines(), size = d.buf.size();
    size_t pos = d.buf.line_start((size_t)c.a);
    size_t end = (size_t)(c.b + 1) < nl ? d.buf.line_start((size_t)(c.b + 1)) : size;
    if (end == size && size) { std::string last; d.buf.copy(size - 1, 1, last); if (last != "\n" && !text.empty()) text.pop_back(); }
    d.break_group();
    d.replace(pos, end - pos, text.data(), text.size());
    d.break_group();
    E.status_msg = op == 0 ? "kept the current change" : op == 1 ? "kept the incoming change" : "kept both changes";
}

/* Accept Current | Accept Incoming | Accept Both on the "<<<<<<<" line, like VS Code's merge conflict actions */
void draw_conflict_actions(const Layout &L, Doc &d) {
    if (E.mode != M_EDIT) return;
    const std::vector<Conflict> *vp = conflicts_for(d);
    if (!vp) return;
    static const char *names[4] = {"Accept Current", "Accept Incoming", "Accept Both", "Merge Editor"};
    float total = 0;
    for (int k = 0; k < 4; k++) total += text_width(E.ui, names[k]) + (float)S(20);
    float rx = (float)E.W - (float)S(16);
    for (size_t i = 0; i < vp->size(); i++) {
        const Conflict &c = (*vp)[i];
        long row = c.a - (long)d.top;
        if (row < 0 || row >= L.rows) continue;
        float y = (float)(L.text_y + row * E.rowh);
        float x = rx - total;
        if (x < (float)L.text_x + (float)(20 * E.cw)) continue;   /* too narrow to show the actions next to the marker text */
        r_rect(E.r, x - (float)S(6), y, total + (float)S(6), (float)E.rowh, ui_mix(C_EDITOR, C_FG, 0.10));
        for (int k = 0; k < 4; k++) {
            float w = text_width(E.ui, names[k]) + (float)S(20);
            int id = (int)i * 4 + k;
            if (hovered(H_CONFLICT, id)) r_rect(E.r, x, y + 1, w, (float)E.rowh - 2, C_HOVER);
            draw_text(E.ui, x + (float)S(10), ui_base(y, (float)E.rowh), names[k], k >= 2 ? C_UI_DIM : C_ACCENT, w);
            hit(x, y, w, (float)E.rowh, H_CONFLICT, id);
            x += w;
        }
    }
}

/* The peek: the HEAD lines of a change (red) and the lines now in the buffer (green) in a panel under the changed lines, with Revert, Open Diff and next / previous change */
void draw_peek(const Layout &L, Doc &d) {
    if (g_peek.doc != &d || E.mode != M_EDIT) return;
    GutterCache *gc = gutter_for(d);
    size_t hk = 0;
    bool found = false;
    if (gc) for (; hk < gc->hunks.size(); hk++) if (gc->hunks[hk].b0 == g_peek.b0) { found = true; break; }
    if (!found) { g_peek.doc = nullptr; return; }
    const DiffHunk h = gc->hunks[hk];
    const int cap = 10;
    int nd = std::min(h.an, cap), na = std::min(h.bn, cap);
    int n = 1 + nd + na + (h.an > cap || h.bn > cap ? 1 : 0);
    long endrow = (long)(h.bn > 0 ? h.b0 + h.bn : h.b0) - (long)d.top;
    if (endrow < 0 || endrow > L.rows) return;
    float x = (float)L.text_x - (float)S(16), w = (float)E.W - x - (float)S(12), rh = (float)E.rowh;
    float y = (float)(L.text_y) + (float)endrow * rh, ymax = (float)(L.text_y + L.rows * E.rowh);
    float ph = (float)n * rh;
    if (y + ph > ymax) y = std::max((float)L.text_y, ymax - ph);
    r_rect(E.r, x, y, w, ph, ui_mix(C_EDITOR, C_FG, 0.05));
    r_rect(E.r, x, y, w, 1, C_ACCENT);
    r_rect(E.r, x, y + ph - 1, w, 1, C_BORDER);
    hit(x, y, w, ph, H_PEEK, 0);   /* swallows clicks, so they do not move the cursor underneath */
    /* header */
    std::string cnt = sfmt("Change %zu of %zu", hk + 1, gc->hunks.size());
    draw_text(E.ui, x + (float)S(10), ui_base(y, rh), cnt.c_str(), C_UI_DIM, w * 0.4f);
    float rx = x + w - (float)S(6);
    auto btn = [&](int id, const char *label, Icon ic) {
        float tw = label ? text_width(E.ui, label) : 0.f;
        float bw = (float)S(8) + (float)S(16) + (label ? tw + (float)S(4) : 0.f);
        float bx = rx - bw;
        if (hovered(H_PEEK, id)) r_rect(E.r, bx, y + 1, bw, rh - 1, C_HOVER);
        draw_icon(ic, bx + (float)S(4), y + (rh - (float)S(16)) / 2, S(16), C_UI);
        if (label) draw_text(E.ui, bx + (float)S(24), ui_base(y, rh), label, C_UI_DIM, tw + 2);
        hit(bx, y + 1, bw, rh - 1, H_PEEK, id);
        rx = bx - (float)S(2);
    };
    btn(3, nullptr, IC_CLOSE);
    btn(2, "Open Diff", IC_OPEN_FILE);
    btn(1, "Revert", IC_UNDO);
    btn(5, nullptr, IC_ARROW_D);
    btn(4, nullptr, IC_ARROW_U);
    /* the lines */
    float ly = y + rh;
    const float tx = x + (float)S(28);
    const int cols = std::max(1, (int)((x + w - tx - (float)S(8)) / (float)E.cw));
    auto line = [&](const std::string &t, bool add) {
        r_rect(E.r, x, ly, w, rh, add ? C_DIFF_ADD : C_DIFF_DEL);
        draw_text(E.font, x + (float)S(10), ly + (float)E.rowasc, add ? "+" : "-", add ? C_GUT_ADD : C_GUT_DEL, (float)S(16));
        r_clip(E.r, (int)tx, (int)ly, (int)(x + w - tx), E.rowh);
        std::string shown = t.size() > (size_t)cols * 4 ? t.substr(0, (size_t)cols * 4) : t;
        draw_text(E.font, tx, ly + (float)E.rowasc, shown.c_str(), C_FG, x + w - tx - (float)S(6));
        r_clip_off(E.r);
        ly += rh;
    };
    for (int i = 0; i < nd; i++) line(gc->base_lines[(size_t)(h.a0 + i)], false);
    std::string t;
    for (int i = 0; i < na; i++) { d.buf.line_text((size_t)(h.b0 + i), t); line(t, true); }
    if (h.an > cap || h.bn > cap) draw_text(E.ui, tx, ui_base(ly, rh), sfmt("%d more lines not shown", std::max(h.an - cap, 0) + std::max(h.bn - cap, 0)).c_str(), C_UI_FAINT, w);
}

void peek_revert(Doc &d) {
    GutterCache *gc = gutter_for(d);
    if (!gc) return;
    for (const DiffHunk &h : gc->hunks) {
        if (h.b0 != g_peek.b0) continue;
        std::string ins;
        for (int i = 0; i < h.an; i++) ins += gc->base_lines[(size_t)(h.a0 + i)] + "\n";
        size_t nl = d.buf.lines(), size = d.buf.size();
        size_t pos = (size_t)h.b0 < nl ? d.buf.line_start((size_t)h.b0) : size;
        size_t end = (size_t)(h.b0 + h.bn) < nl ? d.buf.line_start((size_t)(h.b0 + h.bn)) : size;
        std::string last;
        if (size) d.buf.copy(size - 1, 1, last);
        bool no_eol = size && last != "\n";
        if (end == size && no_eol && !ins.empty()) {   /* the buffer's last line has no newline: keep it that way */
            if (h.bn == 0) ins = "\n" + ins;
            ins.pop_back();
        }
        d.break_group();
        d.replace(pos, end - pos, ins.data(), ins.size());
        d.break_group();
        g_peek.doc = nullptr;
        E.status_msg = "change reverted (Ctrl+Z undoes it)";
        return;
    }
}

void peek_open_diff(Doc &d, int b0) {
    std::string rel = rel_to_root(d.path);
    if (rel.empty()) return;
    Doc &nd = open_diff_spec(rel, ":", "WORK", "Index", "Working Tree", base_name(rel) + " (Working Tree)", "Unstaged changes: the index against the file on disk");
    if (nd.dv) { DiffView &v = *nd.dv; for (size_t k = 0; k < v.m.hunks.size(); k++) if (v.m.hunks[k].b0 + std::max(v.m.hunks[k].bn, 1) > b0) { v.cur_change = (int)k; break; } v.top = -1; }   /* top = -1: scroll to cur_change when drawn */
}

/* A rested pointer over a line number, or over the faint blame text of the cursor line, shows who changed the line and why */
void draw_blame_tip(const Layout &L, Doc &d) {
    if (!E.tip_on || E.mode != M_EDIT) return;
    if (E.my < (float)L.text_y || E.my >= (float)(L.text_y + L.rows * E.rowh)) return;
    size_t line = d.top + (size_t)((E.my - (float)L.text_y) / (float)E.rowh);
    if (line >= d.buf.lines()) return;
    bool in_gutter = E.mx >= (float)L.ed_x && E.mx < (float)L.text_x - (float)S(18);
    bool on_note = false;
    if (!in_gutter && E.g.blame_inline && line == d.buf.line_of(d.cur) && !d.has_sel()) {
        std::string t;
        d.buf.line_text(line, t);
        float x0 = (float)L.text_x + (float)((col_of(t, t.size()) - d.left + 4) * E.cw);
        on_note = E.mx >= x0;
    }
    if (!in_gutter && !on_note) return;
    const GitBlame *bl = blame_for(d, line);
    if (!bl) return;
    std::string l1 = bl->summary.empty() ? std::string("Uncommitted changes") : bl->summary;
    bool unc = bl->hash.find_first_not_of('0') == std::string::npos;
    std::string l2 = unc ? "You, not committed yet" : bl->author + ", " + git_ago(bl->time, (long)time(nullptr)) + "  (" + bl->hash.substr(0, 7) + ")";
    std::string l3 = unc ? "" : "Click the line's blame text to open the commit";
    float w = std::max(text_width(E.ui, l1.c_str()), std::max(text_width(E.ui, l2.c_str()), text_width(E.ui, l3.c_str()))) + (float)S(24);
    w = std::min(w, (float)E.W - (float)S(20));
    float lh = (float)S(22), h = lh * (l3.empty() ? 2 : 3) + (float)S(10);
    float x = std::min(E.mx + (float)S(12), (float)E.W - w - (float)S(8)), y = E.my + (float)S(18);
    if (y + h > (float)(L.text_y + L.rows * E.rowh)) y = E.my - h - (float)S(8);
    r_rect(E.r, x - 1, y - 1, w + 2, h + 2, C_BORDER);
    r_rect(E.r, x, y, w, h, C_CHROME);
    draw_text(E.ui, x + (float)S(12), ui_base(y + (float)S(5), lh), l1.c_str(), C_UI, w - (float)S(24), FS_BOLD);
    draw_text(E.ui, x + (float)S(12), ui_base(y + (float)S(5) + lh, lh), l2.c_str(), C_UI_DIM, w - (float)S(24));
    if (!l3.empty()) draw_text(E.ui, x + (float)S(12), ui_base(y + (float)S(5) + 2 * lh, lh), l3.c_str(), C_UI_FAINT, w - (float)S(24));
}

/* ---- the merge editor: Incoming | Current on top, the Result below, one set of buttons per conflict ---- */
const char *const MERGE_OPS[] = {"Reset", "Accept Current", "Accept Incoming", "Accept Both", "", "Accept Base"};

struct MergeGeom { float tb_h, body_y; int top_rows, res_rows; };   /* rows include each pane's title row */
MergeGeom merge_geom(const Layout &L) {
    MergeGeom g;
    g.tb_h = (float)S(30);
    g.body_y = (float)L.text_y + g.tb_h;
    int total = std::max(6, (int)(((float)L.ed_bottom - g.body_y) / (float)E.rowh));
    g.top_rows = std::max(3, total * 55 / 100);
    g.res_rows = std::max(2, total - g.top_rows);
    return g;
}

void merge_clamp(MergeView &v, const MergeGeom &g) {
    v.top = std::max<long>(0, std::min<long>(v.top, (long)v.rows.size() - (g.top_rows - 1)));
    v.rtop = std::max<long>(0, std::min<long>(v.rtop, (long)v.res.size() - (g.res_rows - 1)));
}

void merge_goto(MergeView &v, int k) {
    if (v.conf_row.empty()) return;
    v.cur = std::max(0, std::min(k, (int)v.conf_row.size() - 1));
    v.top = std::max(0, v.conf_row[(size_t)v.cur] - 1);
}

void merge_open(const std::string &rel) {
    std::string abs = E.git.root + "/" + rel;
    for (size_t i = 0; i < E.docs.size(); i++) if (E.docs[i]->mv && E.docs[i]->mv->root == E.git.root && E.docs[i]->mv->path == rel) { set_active((int)i); return; }
    std::string text;
    FILE *f = fopen(abs.c_str(), "rb");
    if (f) { char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); fclose(f); }
    auto mv = std::make_shared<MergeView>();
    if (!f || !merge_parse(text, *mv)) { open_path(abs); E.status_msg = "no conflict markers left in " + base_name(rel); return; }
    mv->root = E.git.root; mv->path = rel;
    merge_layout(*mv);
    merge_goto(*mv, 0);
    bool replace_first = E.docs.size() == 1 && D().pristine() && !E.blank_tab;
    Doc &d = new_doc(false);
    d.title = "Merge: " + base_name(rel);
    d.mv = mv;
    if (replace_first) E.docs.erase(E.docs.begin());
    set_active((int)E.docs.size() - 1);
}

void merge_op(int id) {
    Doc &d = D();
    if (!d.mv) return;
    MergeView &v = *d.mv;
    if (id < 1000) {
        size_t k = (size_t)(id / 8);
        int op = id % 8;
        if (k >= v.conf_seg.size()) return;
        v.segs[(size_t)v.conf_seg[k]].choice = op;
        v.cur = (int)k;
        v.dirty = true;
        return;
    }
    if (id == 1000) merge_goto(v, v.cur - 1);
    else if (id == 1001) merge_goto(v, v.cur + 1);
    else if (id == 1003) open_path(E.git.root + "/" + v.path);
    else if (id == 1004 || id == 1005) { for (MergeSeg &s : v.segs) if (s.conflict && s.choice == 0) s.choice = id == 1004 ? 1 : 2; v.dirty = true; }
    else if (id == 1002) {
        if (v.dirty) merge_layout(v);
        if (v.unresolved) { E.status_msg = sfmt("%d conflict%s still unresolved", v.unresolved, v.unresolved == 1 ? "" : "s"); return; }
        std::string out = merge_result_text(v), abs = v.root + "/" + v.path, rel = v.path;
        FILE *f = fopen(abs.c_str(), "wb");
        if (!f) { E.status_msg = "cannot write " + rel; return; }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        std::string root = v.root;
        close_tab(E.active);
        git_stage(root, rel);
        git_refresh(); reload_open_docs();
        E.status_msg = "merged " + base_name(rel) + " and staged it";
    }
}

void draw_merge(const Layout &L, float ex, float ew) {
    Doc &d = D();
    MergeView &v = *d.mv;
    if (v.dirty) merge_layout(v);
    const MergeGeom g = merge_geom(L);
    merge_clamp(v, g);
    const float rh = (float)E.rowh, half = (ew - 1) / 2;
    const uint32_t c_in = ui_mix(C_EDITOR, C_ACCENT, 0.17), c_cur = ui_mix(C_EDITOR, C_GIT_ADD, 0.15), dim = ui_mix(C_EDITOR, C_FG, 0.55);

    /* toolbar */
    r_rect(E.r, ex, (float)L.text_y, ew, g.tb_h, C_CHROME);
    r_rect(E.r, ex, (float)L.text_y + g.tb_h - 1, ew, 1, C_BORDER);
    const float by = (float)L.text_y + (g.tb_h - (float)S(22)) / 2, bh = (float)S(22);
    float rx = ex + ew - (float)S(10);
    auto button = [&](int id, const char *label, Icon ic, bool has_icon, bool primary, bool enabled) {
        float tw = label ? text_width(E.ui, label) : 0.f;
        float w = (float)S(12) + (has_icon ? (float)S(16) : 0.f) + (label ? tw + (has_icon ? (float)S(4) : 0.f) : 0.f);
        float x = rx - w;
        uint32_t bg = primary ? (enabled ? (hovered(H_MERGE, id) ? ui_mix(C_ACCENT, C_UI, 0.14) : C_ACCENT) : C_FIELD) : (hovered(H_MERGE, id) ? C_HOVER : 0);
        if (bg) r_rect(E.r, x, by, w, bh, bg);
        uint32_t fg = primary ? (enabled ? C_ON_ACCENT : C_UI_FAINT) : C_UI_DIM;
        float px = x + (float)S(6);
        if (has_icon) { draw_icon(ic, px, by + (bh - (float)S(16)) / 2, S(16), fg); px += (float)S(20); }
        if (label) draw_text(E.ui, px, ui_base(by, bh), label, fg, tw + 2);
        hit(x, by, w, bh, H_MERGE, id);
        rx = x - (float)S(4);
    };
    button(1002, "Complete Merge", IC_CHECK, true, true, v.unresolved == 0);
    button(1003, "Open File", IC_OPEN_FILE, true, false, true);
    button(1005, "All Incoming", IC_ARROW_D, false, false, true);
    button(1004, "All Current", IC_ARROW_D, false, false, true);
    button(1001, nullptr, IC_ARROW_D, true, false, true);
    button(1000, nullptr, IC_ARROW_U, true, false, true);
    std::string lab = v.path + sfmt("   %d conflict%s, %d unresolved", v.nconf, v.nconf == 1 ? "" : "s", v.unresolved);
    draw_text(E.ui, ex + (float)S(12), ui_base(by, bh), lab.c_str(), C_UI, std::max(0.f, rx - ex - (float)S(24)), FS_BOLD);

    /* pane titles */
    float y = g.body_y;
    r_rect(E.r, ex, y, ew, rh, ui_mix(C_EDITOR, C_FG, 0.07));
    draw_text(E.ui, ex + (float)S(12), ui_base(y, rh), ("Incoming  " + v.inc_label).c_str(), C_UI_DIM, half - (float)S(20), FS_BOLD);
    draw_text(E.ui, ex + half + 1 + (float)S(12), ui_base(y, rh), ("Current  " + v.cur_label).c_str(), C_UI_DIM, half - (float)S(20), FS_BOLD);
    y += rh;
    const float top_body = (float)(g.top_rows - 1) * rh;
    r_rect(E.r, ex + half, y, 1, top_body, C_BORDER);
    auto code = [&](float x, float yy, const std::string &t, float w, uint32_t col) {
        r_clip(E.r, (int)x, (int)yy, (int)w, E.rowh);
        draw_text(E.font, x, yy + (float)E.rowasc, t.c_str(), col, w);
        r_clip_off(E.r);
    };
    for (int r = 0; r < g.top_rows - 1; r++) {
        long idx = v.top + r;
        if (idx >= (long)v.rows.size()) break;
        const MRow &row = v.rows[(size_t)idx];
        const MergeSeg &s = v.segs[(size_t)row.seg];
        float yy = y + (float)r * rh;
        if (row.kind == 0) {
            code(ex + (float)S(12), yy, s.common[(size_t)row.a], half - (float)S(16), dim);
            code(ex + half + 1 + (float)S(12), yy, s.common[(size_t)row.a], half - (float)S(16), dim);
        } else if (row.kind == 3) {
            r_rect(E.r, ex, yy, ew, rh, C_DIFF_FOLD);
            std::string t = sfmt("%d unchanged lines", row.a);
            draw_text(E.ui, ex + (ew - text_width(E.ui, t.c_str())) / 2, ui_base(yy, rh), t.c_str(), C_UI_DIM, ew);
        } else if (row.kind == 1) {
            int k = 0;
            for (size_t q = 0; q < v.conf_seg.size(); q++) if (v.conf_seg[q] == row.seg) k = (int)q;
            r_rect(E.r, ex, yy, ew, rh, ui_mix(C_EDITOR, C_FG, 0.13));
            if (v.cur == k) r_rect(E.r, ex, yy, (float)S(3), rh, C_ACCENT);
            static const char *st[6] = {"unresolved", "taking current", "taking incoming", "taking both", "", "taking base"};
            std::string head = sfmt("Conflict %d of %d  \xE2\x80\xA2  %s", k + 1, v.nconf, st[s.choice]);
            draw_text(E.ui, ex + (float)S(12), ui_base(yy, rh), head.c_str(), s.choice ? C_GIT_ADD : C_GIT_MOD, ew * 0.4f, FS_BOLD);
            float bx = ex + ew - (float)S(10);
            const int ops[5] = {0, 5, 3, 2, 1};   /* right to left: Reset, Base, Both, Incoming, Current */
            for (int q = 0; q < 5; q++) {
                int op = ops[q];
                if ((op == 5 && !s.has_base) || (op == 0 && !s.choice)) continue;
                float w = text_width(E.ui, MERGE_OPS[op]) + (float)S(16);
                bx -= w;
                int id = k * 8 + op;
                if (hovered(H_MERGE, id)) r_rect(E.r, bx, yy + 1, w, rh - 2, C_HOVER);
                draw_text(E.ui, bx + (float)S(8), ui_base(yy, rh), MERGE_OPS[op], op == 0 ? C_UI_DIM : C_ACCENT, w);
                hit(bx, yy, w, rh, H_MERGE, id);
            }
        } else {
            if (row.b >= 0) { r_rect(E.r, ex, yy, half, rh, c_in); code(ex + (float)S(12), yy, s.theirs[(size_t)row.b], half - (float)S(16), C_FG); }
            if (row.a >= 0) { r_rect(E.r, ex + half + 1, yy, half, rh, c_cur); code(ex + half + 1 + (float)S(12), yy, s.ours[(size_t)row.a], half - (float)S(16), C_FG); }
            if (s.choice == 2 || s.choice == 3 || s.choice == 4) r_rect(E.r, ex, yy, (float)S(2), rh, C_ACCENT);
            if (s.choice == 1 || s.choice == 3 || s.choice == 4) r_rect(E.r, ex + half + 1, yy, (float)S(2), rh, C_ACCENT);
        }
    }

    /* the result */
    float ry = y + top_body;
    r_rect(E.r, ex, ry, ew, rh, ui_mix(C_EDITOR, C_FG, 0.07));
    r_rect(E.r, ex, ry, ew, 1, C_BORDER);
    std::string rt = v.unresolved ? sfmt("Result  \xE2\x80\xA2  %d conflict%s left to resolve", v.unresolved, v.unresolved == 1 ? "" : "s") : std::string("Result  \xE2\x80\xA2  all conflicts resolved, press Complete Merge");
    draw_text(E.ui, ex + (float)S(12), ui_base(ry, rh), rt.c_str(), v.unresolved ? C_GIT_MOD : C_GIT_ADD, ew - (float)S(24), FS_BOLD);
    ry += rh;
    for (int r = 0; r < g.res_rows - 1; r++) {
        long idx = v.rtop + r;
        if (idx >= (long)v.res.size()) break;
        const MRes &m = v.res[(size_t)idx];
        float yy = ry + (float)r * rh;
        if (yy + rh > (float)L.ed_bottom) break;
        if (m.kind == 1) r_rect(E.r, ex, yy, ew, rh, ui_mix(C_EDITOR, C_GIT_MOD, 0.22));
        else if (m.kind == 2) r_rect(E.r, ex, yy, ew, rh, ui_mix(C_EDITOR, C_GIT_ADD, 0.10));
        code(ex + (float)S(12), yy, m.text, ew - (float)S(24), m.kind == 1 ? C_GIT_MOD : C_FG);
    }
}

bool merge_key(const SDL_KeyboardEvent &k, bool ctrl, bool shift) {
    MergeView &v = *D().mv;
    Layout L = layout();
    const MergeGeom g = merge_geom(L);
    if (ctrl || (k.keysym.mod & KMOD_ALT)) return false;
    switch (k.keysym.sym) {
    case SDLK_UP: v.top -= 1; break;
    case SDLK_DOWN: v.top += 1; break;
    case SDLK_PAGEUP: v.top -= g.top_rows - 2; break;
    case SDLK_PAGEDOWN: v.top += g.top_rows - 2; break;
    case SDLK_HOME: v.top = 0; break;
    case SDLK_END: v.top = (long)v.rows.size(); break;
    case SDLK_F7: merge_goto(v, v.cur + (shift ? -1 : 1)); break;
    case SDLK_BACKSPACE: case SDLK_DELETE: case SDLK_TAB: return true;
    default: return false;
    }
    merge_clamp(v, g);
    touch();
    return true;
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
            tw[i] = std::min((float)S(240), std::max((float)S(90), text_width(E.ui, E.docs[i]->name().c_str()) + (float)S(48)));
            total += tw[i];
        }
        E.tab_x.assign(nt, 0); E.tab_w = tw;
        { float acc = 0; for (size_t i = 0; i < nt; i++) { E.tab_x[i] = acc; acc += tw[i]; } }
        if (E.tab_reveal && E.active >= 0 && (size_t)E.active < nt) {   /* a switch by keyboard or click scrolls the strip just enough to show the tab */
            float l = E.tab_x[(size_t)E.active], r = l + tw[(size_t)E.active];
            if (l < E.tab_scroll) E.tab_scroll = l;
            else if (r > E.tab_scroll + ew) E.tab_scroll = r - ew;
        }
        E.tab_reveal = false;
        E.tab_scroll = std::max(0.f, std::min(E.tab_scroll, total - ew));
        r_clip(E.r, (int)ex, (int)ty, (int)ew, (int)th);
        tx = ex - E.tab_scroll;
        for (size_t i = 0; i < nt; i++) {
            bool act = (int)i == E.active;
            bool lift = E.tab_dragging && E.tab_press == (int)i;
            if (tx + tw[i] < ex || tx > ex + ew) { tx += tw[i]; continue; }   /* scrolled out of the strip */
            r_rect(E.r, tx, ty, tw[i], th, act || lift ? C_EDITOR : C_CHROME);
            if (act) r_rect(E.r, tx, ty + th - (float)S(2), tw[i], (float)S(2), C_ACCENT);
            if (lift) r_rect(E.r, tx, ty, tw[i], (float)S(2), C_ACCENT);
            r_rect(E.r, tx + tw[i] - 1, ty, 1, th, C_BORDER);
            float ix = tx + (float)S(14);
            const Doc &dd = *E.docs[i];
            std::string nm = dd.name();
            if (dd.preview && dd.modified()) E.docs[i]->preview = false;   /* typing in a preview keeps the tab */
            draw_text(E.ui, ix, ui_base(ty, th), nm.c_str(), act ? C_UI : C_UI_DIM, tw[i] - (float)S(46), dd.preview ? FS_ITALIC : FS_REGULAR);
            bool mod = dd.modified(), hv = hovered(H_TABCLOSE, (int)i) || hovered(H_TAB, (int)i);
            float cx = tx + tw[i] - (float)S(26), cy = ty + (th - (float)S(16)) / 2;
            if (mod && !hovered(H_TABCLOSE, (int)i)) draw_icon(IC_DOT, cx, cy, S(16), C_UI);
            else if (act || hv) draw_icon(IC_CLOSE, cx, cy, S(16), hovered(H_TABCLOSE, (int)i) ? C_UI : C_UI_DIM);
            auto clipped_hit = [&](float x0, float w0, int kind) {   /* a tab half under the sidebar must not take its clicks */
                float x1 = std::max(x0, ex), x2 = std::min(x0 + w0, ex + ew);
                if (x2 > x1) hit(x1, ty, x2 - x1, th, kind, (int)i);
            };
            clipped_hit(tx, tw[i] - (float)S(30), H_TAB);
            clipped_hit(tx + tw[i] - (float)S(30), (float)S(30), H_TABCLOSE);
            tx += tw[i];
        }
        r_clip_off(E.r);
        if (nt && tx < ex + ew) hit(tx, ty, ex + ew - tx, th, H_TABSPACE, 0);   /* the empty part of the strip: a double click opens a new empty tab */
    }
    double tm1 = E.timing ? now_s() : 0;

    /* ---- editor area ---- */
    if (splash()) {
        /* the owl in its tag as a faint watermark, then the name and a hint, centred in the editor area like the empty VS Code window */
        const char *name = "Hootedit";
        const char *hint = "Double click or Ctrl+N: new tab   Ctrl+P: open   Ctrl+B: sidebar";
        float cxm = ex + ew / 2, areah = (float)(L.ed_bottom - L.text_y);
        int ls = (int)std::max((float)S(96), std::min((float)S(200), std::min(ew, areah) * 0.34f)) / 16 * 16;   /* multiples of 16 px: a resize makes a few sizes, not hundreds */
        float gap1 = (float)S(24), gap2 = (float)S(16), nameh = (float)S(36), hinth = (float)S(18);
        float total = (float)ls + gap1 + nameh + gap2 + hinth;
        float y0 = (float)L.text_y + std::max(0.f, (areah - total) / 2) - (float)S(10);
        uint32_t wm = ui_mix(C_EDITOR, C_UI, 0.09f), wm_eyes = ui_mix(C_EDITOR, C_ACCENT, 0.34f);
        draw_icon(IC_LOGO_BODY, cxm - (float)ls / 2, y0, ls, wm);
        draw_icon(IC_LOGO_EYES, cxm - (float)ls / 2, y0, ls, wm_eyes);
        float w = text_width(E.big, name);
        draw_text(E.big, cxm - w / 2, y0 + (float)ls + gap1 + nameh - (float)S(8), name, wm, (float)E.W);
        draw_text(E.ui, cxm - text_width(E.ui, hint) / 2, y0 + (float)ls + gap1 + nameh + gap2 + (float)S(13), hint, C_UI_FAINT, ew);
    } else if (d.dv) {
        draw_diff(L, ex, ew);
    } else if (d.mv) {
        draw_merge(L, ex, ew);
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
        const std::vector<Conflict> *cfl = conflicts_for(d);
        long bra = -1, brb = -1;
        bracket_pair(d, &bra, &brb);
        long bla = bra >= 0 ? (long)d.buf.line_of((size_t)bra) : -1, blb = brb >= 0 ? (long)d.buf.line_of((size_t)brb) : -1;
        /* pass 2: per row, bottom layer first (cursor line, matches, selection, indent guides, bracket pair), then the text */
        for (int row = 0; row < nrows; row++) {
            size_t line = d.top + (size_t)row;
            const std::string &tt = texts[(size_t)row];
            float ry = (float)(L.text_y + row * E.rowh);
            if (cfl) {
                size_t ci_ = 0;
                int ck = conflict_kind(*cfl, (long)line, &ci_);
                if (ck) {
                    uint32_t bg = ck == 1 ? ui_mix(C_EDITOR, C_GIT_ADD, 0.34) : ck == 2 ? ui_mix(C_EDITOR, C_GIT_ADD, 0.14) : ck == 7 ? ui_mix(C_EDITOR, C_ACCENT, 0.36) : ck == 6 ? ui_mix(C_EDITOR, C_ACCENT, 0.16) : ui_mix(C_EDITOR, C_FG, 0.10);
                    r_rect(E.r, (float)L.text_x, ry, (float)(E.W - L.text_x), (float)E.rowh, bg);
                }
            }
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
            float colw = (float)(L.text_x - L.ed_x - S(18));   /* the number column: the gutter without the space before the text */
            float x = (float)L.ed_x + (colw - (float)(strlen(num) * (size_t)E.cw)) / 2;   /* centered, so 1, 42 and 1337 share one axis */
            draw_text(E.font, x, (float)(L.text_y + row * E.rowh + E.rowasc), num, line == cl ? C_GUTTER_CUR : C_GUTTER, (float)E.W);
        }
        if (GutterCache *gc = gutter_for(d)) {   /* change bars: green added, blue changed, a red wedge where lines were removed */
            const std::vector<DiffHunk> &hs = gc->hunks;
            size_t lo = 0, hi = hs.size();
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (hs[mid].b0 + std::max(hs[mid].bn, 1) <= (int)d.top) lo = mid + 1; else hi = mid; }
            float bx = (float)(L.text_x - S(9)), bw = (float)S(3);
            for (size_t k = lo; k < hs.size() && hs[k].b0 <= (int)(d.top + (size_t)L.rows); k++) {
                const DiffHunk &h = hs[k];
                if (h.bn > 0) {
                    long r0 = std::max<long>(h.b0, (long)d.top), r1 = std::min<long>(h.b0 + h.bn, (long)(d.top + (size_t)L.rows));
                    if (r1 > r0) {
                        float y0 = (float)(L.text_y + (r0 - (long)d.top) * E.rowh);
                        r_rect(E.r, bx, y0, bw, (float)((r1 - r0) * E.rowh), h.an == 0 ? C_GUT_ADD : C_GUT_MOD);
                        hit((float)(L.text_x - S(14)), y0, (float)S(14), (float)((r1 - r0) * E.rowh), H_GUTTER, h.b0);
                    }
                } else {
                    long row = (long)h.b0 - (long)d.top;
                    if (row >= 0 && row <= L.rows) {
                        float y0 = (float)(L.text_y + row * E.rowh);
                        draw_icon(IC_TRI_R, bx - (float)S(1), y0 - (float)S(5), S(10), C_GUT_DEL);
                        hit((float)(L.text_x - S(14)), y0 - (float)S(6), (float)S(14), (float)S(12), H_GUTTER, h.b0);
                    }
                }
            }
        }
        if (E.blink_on && E.mode == M_EDIT && cl >= d.top && cl < d.top + (size_t)L.rows) {
            d.buf.line_text(cl, t);
            int c = col_of(t, cb) - d.left;
            if (c >= 0 && c <= L.cols) r_rect(E.r, (float)(L.text_x + c * E.cw), (float)(L.text_y + (long)(cl - d.top) * E.rowh), vim_active() && g_vim.block_cursor() ? (float)E.cw : std::max(1.f, 1.5f * E.scale), (float)E.rowh, vim_active() && g_vim.block_cursor() ? ui_mix(C_EDITOR, C_CURSOR, 0.75f) : C_CURSOR);
        }
        draw_peek(L, d);
        draw_conflict_actions(L, d);
        draw_blame_tip(L, d);
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
        if (d.mv) { items[4] = sfmt("%d unresolved", d.mv->unresolved); items[1] = "merge"; items[3] = "three-way"; }
        if (d.dv) { items[4] = d.dv->m.hunks.empty() ? "no changes" : sfmt("%zu change%s", d.dv->m.hunks.size(), d.dv->m.hunks.size() == 1 ? "" : "s"); items[1] = "diff"; items[3] = d.dv->vinline ? "inline" : "side by side"; }
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
        if (const GitBlame *bl = blame_for(d, cl)) {
            std::string bt = blame_text(*bl, false);
            float bw2 = std::min(text_width(E.ui, bt.c_str()), std::max(0.f, rx - lx - (float)S(200)));
            if (bw2 > (float)S(60)) {
                if (hovered(H_BLAME, 0)) r_rect(E.r, lx - (float)S(6), by0 + 1, bw2 + (float)S(12), bh - 1, C_HOVER);
                draw_text(E.ui, lx, sy, bt.c_str(), C_UI_DIM, bw2 + 2);
                hit(lx - (float)S(6), by0, bw2 + (float)S(12), bh, H_BLAME, 0);
                lx += bw2 + (float)S(18);
            }
        }
        std::string last = E.ops.empty() ? std::string() : (E.ops.back().brief.empty() ? E.ops.back().name : E.ops.back().brief) + "  " + fmt_ms(E.ops.back().ms);
        std::string left = E.status_msg.empty() ? (last.empty() ? std::string(E.hud) : last + "   |   " + E.hud) : E.status_msg;
        if (vim_active() && !d.dv && !d.mv) {
            std::string vl = g_vim.label();
            float vw = draw_text(E.ui, lx, sy, vl.c_str(), g_vim.mode == Vim::INSERT ? C_GREEN : C_ACCENT, std::max(0.f, rx - lx), FS_BOLD);
            lx = vw + (float)S(14);
        }
        draw_text(E.ui, lx, sy, left.c_str(), E.status_msg.empty() ? C_UI_FAINT : C_UI_DIM, std::max(0.f, rx - lx));
        if (vim_active() && g_vim.cmd_kind) {   /* the vim command line takes over the whole bar, like the last line of the terminal */
            r_rect(E.r, 0, by0, (float)E.W, bh, C_CHROME);
            r_rect(E.r, 0, by0, (float)E.W, 1, C_BORDER);
            std::string cl = std::string(1, g_vim.cmd_kind) + g_vim.cmd;
            float pen = draw_text(E.ui, (float)S(10), sy, cl.c_str(), C_UI, (float)E.W - (float)S(20));
            if (E.blink_on) r_rect(E.r, pen + 1, by0 + (float)S(4), (float)S(8), bh - (float)S(8), C_CURSOR);
        }
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
        if (E.mode == M_FIND && E.repl_on) {   /* the Replace box hangs under the find box */
            float ry2 = mh, rh2 = (float)S(34);
            r_rect(E.r, bx, ry2, bw, rh2, C_BOX_LINE);
            r_rect(E.r, bx + 1, ry2, bw - 2, rh2 - 1, C_BOX);
            r_rect(E.r, bx, ry2 + rh2 - 1, bw, 1, C_ACCENT);
            hit(bx, ry2, bw, rh2, H_REPL, 3);
            const char *bl[2] = {"Replace", "All"};
            float bxr = bx + bw - (float)S(6);
            for (int k2 = 1; k2 >= 0; k2--) {
                float w = text_width(E.ui, bl[k2]) + (float)S(16), hh = rh2 - (float)S(10);
                bxr -= w;
                r_rect(E.r, bxr, ry2 + (float)S(5), w, hh, hovered(H_REPL, k2) ? C_SELROW : C_FIELD);
                draw_text(E.ui, bxr + (float)S(8), ui_base(ry2 + (float)S(5), hh), bl[k2], C_UI, w);
                hit(bxr, ry2 + (float)S(5), w, hh, H_REPL, k2);
                bxr -= (float)S(4);
            }
            float fx = bx + (float)S(10), fw = bxr - fx;
            r_rect(E.r, fx - (float)S(6), ry2 + (float)S(5), fw + (float)S(8), rh2 - (float)S(10), E.repl_focus ? C_ACCENT : C_BOX_LINE);
            r_rect(E.r, fx - (float)S(5), ry2 + (float)S(6), fw + (float)S(6), rh2 - (float)S(12), C_FIELD);
            hit(fx - (float)S(6), ry2 + (float)S(5), fw + (float)S(8), rh2 - (float)S(10), H_REPL, 2);
            float pen2 = draw_text(E.ui, fx, ui_base(ry2, rh2 - (float)S(1)), E.repl_q.c_str(), C_UI, fw - (float)S(4));
            if (E.repl_q.empty()) draw_text(E.ui, fx, ui_base(ry2, rh2 - (float)S(1)), "Replace with", C_UI_FAINT, fw);
            if (E.repl_focus && E.blink_on) r_rect(E.r, pen2, ry2 + (float)S(9), std::max(1.f, E.scale), rh2 - (float)S(19), C_CURSOR);
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
            bool enabled = it.act != A_NONE && !(it.act == A_VIM && !have_vim());
            if (enabled && (int)i == E.menu_sel) r_rect(E.r, x + (float)S(3), y, iw - (float)S(6), ih, C_MENU_SEL);
            uint32_t col = enabled ? C_UI : C_UI_FAINT;
            draw_text(E.ui, x + (float)S(26), ui_base(y, ih), (it.act == A_VIM && !have_vim() ? std::string(it.label) + " (vim not installed)" : (it.act == A_VIM && E.vim_on ? std::string("\xE2\x9C\x93 ") + it.label : (it.act == A_OUTLINE && E.outline_on ? std::string("\xE2\x9C\x93 ") + it.label : std::string(it.label)))).c_str(), col, iw);
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
        const char *ph_txt = E.pick_kind == PICK_LANG ? "Select Language Mode" : E.pick_kind == PICK_THEME ? "Select Color Theme" : E.pick_kind == PICK_CUSTOM ? E.pick_title.c_str() : "Select the Terminal";
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
    if (E.mode == M_DIALOG) draw_dialog();
    if (E.mode == M_CTX) draw_ctx_menu();
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


/* ---- VS Code style editing commands: word moves, line operations, comments, indent ---- */

int char_class(unsigned char c) { return c == ' ' || c == '\t' ? 0 : (isalnum(c) || c == '_' || c >= 0x80) ? 1 : 2; }

size_t word_left(Doc &d, size_t pos) {
    size_t l = d.buf.line_of(pos), ls = d.buf.line_start(l);
    if (pos == ls) return l ? ls - 1 : 0;
    std::string t;
    d.buf.line_text(l, t);
    size_t i = pos - ls;
    while (i > 0 && char_class((unsigned char)t[i - 1]) == 0) i--;
    if (i > 0) { int c = char_class((unsigned char)t[i - 1]); while (i > 0 && char_class((unsigned char)t[i - 1]) == c) i--; }
    return ls + i;
}

size_t word_right(Doc &d, size_t pos) {
    size_t l = d.buf.line_of(pos), ls = d.buf.line_start(l), n = d.buf.line_len(l);
    if (pos - ls >= n) return l + 1 < d.buf.lines() ? d.buf.line_start(l + 1) : pos;
    std::string t;
    d.buf.line_text(l, t);
    size_t i = pos - ls;
    while (i < n && char_class((unsigned char)t[i]) == 0) i++;
    if (i < n) { int c = char_class((unsigned char)t[i]); while (i < n && char_class((unsigned char)t[i]) == c) i++; }
    return ls + i;
}

/* the lines the selection touches: a selection ending at the very start of a line does not include that line */
void sel_lines(Doc &d, size_t *l0, size_t *l1) {
    *l0 = d.buf.line_of(d.sel_lo());
    *l1 = d.buf.line_of(d.sel_hi());
    if (d.has_sel() && *l1 > *l0 && d.sel_hi() == d.buf.line_start(*l1)) (*l1)--;
}

size_t line_end(Doc &d, size_t l) { return d.buf.line_start(l) + d.buf.line_len(l); }

std::string lines_text(Doc &d, size_t l0, size_t l1) {
    std::string out, t;
    for (size_t l = l0; l <= l1; l++) { d.buf.line_text(l, t); if (l > l0) out += '\n'; out += t; }
    return out;
}

void set_sel(Doc &d, size_t anchor, size_t cur) { d.anchor = anchor; d.cur = cur; d.want_col = d.col_of_byte(d.buf.line_of(cur), cur - d.buf.line_start(d.buf.line_of(cur))); }

void line_move(Doc &d, int dir) {   /* Alt+Up / Alt+Down: the selected lines trade places with their neighbour */
    size_t l0, l1;
    sel_lines(d, &l0, &l1);
    if (dir < 0 && l0 == 0) return;
    if (dir > 0 && l1 + 1 >= d.buf.lines()) return;
    size_t a = d.anchor, c = d.cur;
    std::string block = lines_text(d, l0, l1), nb;
    size_t from = d.buf.line_start(dir < 0 ? l0 - 1 : l0), to = line_end(d, dir < 0 ? l1 : l1 + 1);
    d.buf.line_text(dir < 0 ? l0 - 1 : l1 + 1, nb);
    std::string out = dir < 0 ? block + "\n" + nb : nb + "\n" + block;
    d.break_group();
    d.replace(from, to - from, out.data(), out.size());
    d.break_group();
    long delta = dir < 0 ? -(long)(nb.size() + 1) : (long)(nb.size() + 1);
    set_sel(d, (size_t)((long)a + delta), (size_t)((long)c + delta));
}

void line_copy(Doc &d, int dir) {   /* Shift+Alt+Up / Down: duplicate the selected lines above or below */
    size_t l0, l1;
    sel_lines(d, &l0, &l1);
    size_t a = d.anchor, c = d.cur;
    std::string block = lines_text(d, l0, l1);
    d.break_group();
    if (dir > 0) { std::string ins = "\n" + block; d.replace(line_end(d, l1), 0, ins.data(), ins.size()); set_sel(d, a + block.size() + 1, c + block.size() + 1); }
    else { std::string ins = block + "\n"; d.replace(d.buf.line_start(l0), 0, ins.data(), ins.size()); set_sel(d, a, c); }
    d.break_group();
}

void line_delete(Doc &d) {   /* Ctrl+Shift+K */
    size_t l0, l1;
    sel_lines(d, &l0, &l1);
    size_t from = d.buf.line_start(l0), to = l1 + 1 < d.buf.lines() ? d.buf.line_start(l1 + 1) : d.buf.size();
    if (l1 + 1 >= d.buf.lines() && l0 > 0) from--;   /* the last line takes the break before it with it */
    d.break_group();
    d.replace(from, to - from, "", 0);
    d.break_group();
}

void line_open(Doc &d, bool below) {   /* Ctrl+Enter / Ctrl+Shift+Enter: a new line without moving the line break */
    size_t l = d.buf.line_of(d.cur);
    std::string t;
    d.buf.line_text(l, t);
    size_t w = 0;
    while (w < t.size() && (t[w] == ' ' || t[w] == '\t')) w++;
    std::string ind = t.substr(0, w);
    d.break_group();
    if (below) { std::string ins = "\n" + ind; d.replace(line_end(d, l), 0, ins.data(), ins.size()); }
    else { std::string ins = ind + "\n"; size_t ls = d.buf.line_start(l); d.replace(ls, 0, ins.data(), ins.size()); set_sel(d, ls + ind.size(), ls + ind.size()); }
    d.break_group();
}

std::string indent_unit(Doc &d) { return d.indent.compare(0, 3, "Tab") == 0 ? std::string("\t") : std::string((size_t)std::max(1, indent_step(d)), ' '); }

void indent_lines(Doc &d, bool out) {   /* Ctrl+] / Ctrl+[ and Tab / Shift+Tab over several lines */
    size_t l0, l1;
    sel_lines(d, &l0, &l1);
    size_t a = d.anchor, c = d.cur;
    std::string unit = indent_unit(d), res, t;
    long first = 0, last = 0;
    for (size_t l = l0; l <= l1; l++) {
        d.buf.line_text(l, t);
        long delta = 0;
        if (!out) { if (!t.empty()) { t.insert(0, unit); delta = (long)unit.size(); } }
        else if (!t.empty() && t[0] == '\t') { t.erase(0, 1); delta = -1; }
        else { size_t k = 0; while (k < t.size() && k < unit.size() && t[k] == ' ') k++; t.erase(0, k); delta = -(long)k; }
        if (l == l0) first = delta;
        if (l == l1) last = delta;
        if (l > l0) res += '\n';
        res += t;
    }
    size_t from = d.buf.line_start(l0), to = line_end(d, l1);
    d.break_group();
    d.replace(from, to - from, res.data(), res.size());
    d.break_group();
    auto adj = [&](size_t pos) {
        size_t l = d.buf.line_of(std::min(pos, d.buf.size()));
        return (size_t)std::max<long>((long)from, (long)pos + (l == l0 ? first : l == l1 ? last : first));
    };
    size_t na = adj(a), nc = adj(c);
    if (a == c) nc = na;
    set_sel(d, na, nc);
}

const char *comment_prefix(const Lang *l) {
    const char *n = lang_name(l);
    static const char *hash[] = {"Python", "Shell", "YAML", "TOML", "Makefile", "CMake", "Ruby", "Perl", "Dockerfile", "Diff"};
    for (const char *h : hash) if (!strcmp(n, h)) return "#";
    if (!strcmp(n, "Lua") || !strcmp(n, "SQL") || !strcmp(n, "Haskell")) return "--";
    if (!strcmp(n, "INI")) return ";";
    if (!strcmp(n, "Plain Text") || !strcmp(n, "Markdown") || !strcmp(n, "JSON") || !strcmp(n, "HTML") || !strcmp(n, "XML") || !strcmp(n, "CSS")) return nullptr;
    return "//";
}

void toggle_comment(Doc &d) {   /* Ctrl+/ */
    const char *pre = comment_prefix(d.lang);
    if (!pre) { E.status_msg = std::string("no line comment for ") + lang_name(d.lang); return; }
    size_t pl = strlen(pre), l0, l1;
    sel_lines(d, &l0, &l1);
    size_t a = d.anchor, c = d.cur;
    std::vector<std::string> ls;
    bool all = true, any = false;
    size_t minind = (size_t)-1;
    for (size_t l = l0; l <= l1; l++) {
        ls.emplace_back();
        d.buf.line_text(l, ls.back());
        const std::string &t = ls.back();
        size_t w = 0;
        while (w < t.size() && (t[w] == ' ' || t[w] == '\t')) w++;
        if (w == t.size()) continue;   /* blank lines neither count nor change */
        any = true;
        minind = std::min(minind, w);
        if (t.compare(w, pl, pre) != 0) all = false;
    }
    if (!any) return;
    std::string res;
    long first = 0, last = 0;
    for (size_t i = 0; i < ls.size(); i++) {
        std::string &t = ls[i];
        size_t w = 0;
        while (w < t.size() && (t[w] == ' ' || t[w] == '\t')) w++;
        long delta = 0;
        if (w < t.size()) {
            if (all) { size_t k = pl + (t.compare(w + pl, 1, " ") == 0 ? 1 : 0); t.erase(w, k); delta = -(long)k; }
            else { std::string ins = std::string(pre) + " "; t.insert(minind, ins); delta = (long)ins.size(); }
        }
        if (i == 0) first = delta;
        if (i + 1 == ls.size()) last = delta;
        if (i) res += '\n';
        res += t;
    }
    size_t from = d.buf.line_start(l0), to = line_end(d, l1);
    d.break_group();
    d.replace(from, to - from, res.data(), res.size());
    d.break_group();
    auto adj = [&](size_t pos) { size_t l = d.buf.line_of(std::min(pos, d.buf.size())); return (size_t)std::max<long>((long)from, (long)pos + (l == l0 ? first : l == l1 ? last : first)); };
    set_sel(d, a == c ? adj(c) : adj(a), adj(c));
}

void select_next_occurrence(Doc &d) {   /* Ctrl+D: first the word under the cursor, then the next match of the selection */
    if (!d.has_sel()) {
        size_t l = d.buf.line_of(d.cur), ls = d.buf.line_start(l);
        std::string t;
        d.buf.line_text(l, t);
        size_t i = d.cur - ls, a = i, b = i;
        while (a > 0 && char_class((unsigned char)t[a - 1]) == 1) a--;
        while (b < t.size() && char_class((unsigned char)t[b]) == 1) b++;
        if (b > a) set_sel(d, ls + a, ls + b);
        return;
    }
    std::string needle = d.selection();
    if (needle.empty() || needle.size() > 4096) return;
    size_t from = d.sel_hi(), cap = 8u << 20;
    std::string hay;
    d.buf.copy(from, std::min(cap, d.buf.size() - from), hay);
    size_t at = hay.find(needle);
    if (at != std::string::npos) { set_sel(d, from + at, from + at + needle.size()); return; }
    d.buf.copy(0, std::min(d.sel_lo(), cap), hay);   /* wrapped around */
    at = hay.find(needle);
    if (at != std::string::npos) set_sel(d, at, at + needle.size());
}

void goto_line(const std::string &q) {
    long n = atol(q.c_str());
    Doc &d = D();
    if (n < 1) n = 1;
    size_t l = std::min((size_t)n - 1, d.buf.lines() - 1);
    d.break_group();
    set_sel(d, d.buf.line_start(l), d.buf.line_start(l));
    E.follow = true;
    size_t rows = (size_t)std::max(1, layout().rows);   /* centre the line like VS Code does */
    d.top = l > rows / 2 ? l - rows / 2 : 0;
    E.follow = false;
}

void reopen_closed_tab() {
    while (!E.closed_tabs.empty()) {
        std::string p = E.closed_tabs.back();
        E.closed_tabs.pop_back();
        struct stat st;
        if (stat(p.c_str(), &st) == 0) { open_path(p); return; }
    }
    E.status_msg = "no closed tab to reopen";
}

void close_all_tabs() {
    int kept = 0;
    for (int i = (int)E.docs.size() - 1; i >= 0; i--) {
        if (E.docs[(size_t)i]->modified()) { kept++; continue; }
        remove_doc(i);
    }
    if (kept) E.status_msg = sfmt("%d tab%s with unsaved changes kept", kept, kept == 1 ? "" : "s");
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
    {   /* VS Code's own colours (git extension manifest, Dark/Light Modern, diff editor registry); dark or light by the editor background */
        double lum = (0.299 * (tbg & 255) + 0.587 * ((tbg >> 8) & 255) + 0.114 * ((tbg >> 16) & 255)) / 255.0;
        bool light = lum > 0.5;
        C_GIT_ADD = light ? RGBA(0x58, 0x7c, 0x0c, 255) : RGBA(0x81, 0xb8, 0x8b, 255);
        C_GIT_MOD = light ? RGBA(0x89, 0x55, 0x03, 255) : RGBA(0xe2, 0xc0, 0x8d, 255);
        C_GIT_DEL = light ? RGBA(0xad, 0x07, 0x07, 255) : RGBA(0xc7, 0x4e, 0x39, 255);
        C_GIT_UNT = light ? RGBA(0x00, 0x71, 0x00, 255) : RGBA(0x73, 0xc9, 0x91, 255);
        C_GIT_CONF = light ? RGBA(0xad, 0x07, 0x07, 255) : RGBA(0xe4, 0x67, 0x6b, 255);
        C_GUT_ADD = RGBA(0x2e, 0xa0, 0x43, 255);
        C_GUT_MOD = light ? RGBA(0x00, 0x5f, 0xb8, 255) : RGBA(0x00, 0x78, 0xd4, 255);
        C_GUT_DEL = RGBA(0xf8, 0x51, 0x49, 255);
        C_DIFF_ADD = ui_mix(tbg, RGBA(155, 185, 85, 255), light ? 0.30 : 0.20);        /* diffEditor.insertedLineBackground */
        C_DIFF_ADD_TXT = ui_mix(tbg, RGBA(156, 204, 44, 255), light ? 0.42 : 0.34);    /* diffEditor.insertedTextBackground, over the line colour */
        C_DIFF_DEL = ui_mix(tbg, RGBA(255, 0, 0, 255), light ? 0.16 : 0.20);           /* diffEditor.removedLineBackground */
        C_DIFF_DEL_TXT = ui_mix(tbg, RGBA(255, 0, 0, 255), light ? 0.30 : 0.38);
        C_DIFF_FILL = ui_mix(tbg, tfg, 0.14);                                          /* diffEditor.diagonalFill */
        C_DIFF_FOLD = ui_mix(bg1, tbg, 0.5);
    }
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

void pick_custom(const std::string &title, std::vector<Ed::PItem> items, std::function<void(const Ed::PItem &)> cb) {
    E.pick_kind = PICK_CUSTOM; E.pick_title = title; E.pick_items = std::move(items); E.pick_cb = std::move(cb);
    E.pick_filter.clear(); E.pick_sel = E.pick_scroll = 0;
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
    if (E.pick_kind == PICK_CUSTOM) { auto cb = E.pick_cb; if (cb) cb(it); redraw(); return; }
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

void command_palette();
void show_shortcuts();
void do_action(int act) {
    if ((D().dv || D().mv) && (act == A_CUT || act == A_PASTE || act == A_UNDO || act == A_REDO || act == A_SAVE || act == A_SAVEAS)) { E.status_msg = "diff tabs are read only"; redraw(); return; }
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
    case A_FIND: E.mode = M_FIND; E.find_msg.clear(); E.repl_on = false; E.repl_focus = false; break;
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
    case A_PALETTE: command_palette(); break;
    case A_OUTLINE: E.outline_on = !E.outline_on; settings_save(); E.status_msg = E.outline_on ? "outline on (Files tab)" : "outline off"; break;
    case A_SHORTCUTS: show_shortcuts(); break;
    case A_VIM: if (!have_vim()) { E.status_msg = "vim is not installed: Vim Keys is unavailable"; break; } E.vim_on = !E.vim_on; vim_reset(); settings_save(); E.status_msg = E.vim_on ? "vim keys on (Esc for normal mode)" : "vim keys off"; break;
    case A_REPLACE: if (D().dv || D().mv) { E.status_msg = "diff tabs are read only"; break; } E.mode = M_FIND; E.find_msg.clear(); E.repl_on = true; E.repl_focus = false; break;
    case A_GOTO: E.mode = M_QUICK; E.quick_q = ":"; E.quick_sel = 0; quick_update(); break;
    case A_REOPEN: reopen_closed_tab(); break;
    case A_CLOSE_ALL: close_all_tabs(); break;
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

/* Ctrl+Shift+P: every menu command and the git and remote actions in one filterable list */
void command_palette() {
    struct Cmd { std::string label; std::function<void()> run; };
    auto cmds = std::make_shared<std::vector<Cmd>>();
    for (const Menu &m : MENUS)
        for (const MenuItem &it : m.items)
            if (it.label[0] && it.act != A_NONE && it.act != A_PALETTE) { int act = it.act; cmds->push_back({std::string(m.name) + ": " + it.label + (it.keys[0] ? std::string("    ") + it.keys : std::string()), [act] { do_action(act); }}); }
    auto add = [&](const char *l, std::function<void()> f) { cmds->push_back({l, std::move(f)}); };
    add("Git: Pull", [] { git_pull_flow(); });
    add("Git: Pull (Rebase)", [] { git_pull_rebase(); });
    add("Git: Pull from...", [] { git_pull_from(); });
    add("Git: Push", [] { git_push_flow(); });
    add("Git: Push to...", [] { git_push_to(); });
    add("Git: Fetch", [] { git_fetch_flow(); });
    add("Git: Sync", [] { git_sync_flow(); });
    add("Git: Checkout to...", [] { git_branch_picker(); });
    add("Git: Create Branch...", [] { git_create_branch(""); });
    add("Git: Merge Branch...", [] { git_branch_ops(2); });
    add("Git: Rebase Branch...", [] { git_rebase_onto(); });
    add("Git: Stage All Changes", [] { git_stage_all(E.git.root); git_refresh(); });
    add("Git: Unstage All Changes", [] { git_unstage(E.git.root, "."); git_refresh(); });
    add("Git: Commit", [] { git_commit_flow(0); });
    add("Git: Commit (Amend)", [] { git_commit_flow(1); });
    add("Git: Stash", [] { git_stash_push(false); });
    add("Git: Pop Latest Stash", [] { git_stash_op("pop", true); });
    add("Git: Create Tag...", [] { git_tag_create(""); });
    add("Git: Add Remote...", [] { git_remote_add(); });
    add("Git: Remove Remote...", [] { git_remote_remove(); });
    add("Remote: New Connection...", [] { remote_new_connection(); });
    add("Remote: Open SSH Terminal", [] { if (!g_rem.profiles.empty()) { for (size_t i = 0; i < g_rem.conns.size(); i++) if (g_rem.conns[i].connected) { remote_open_terminal((int)i); return; } } E.status_msg = "connect to an ssh server in the Remote tab first"; });
    std::vector<Ed::PItem> items;
    for (size_t i = 0; i < cmds->size(); i++) items.push_back({(*cmds)[i].label, (int)i, false});
    pick_custom("Command Palette", items, [cmds](const Ed::PItem &it) { if ((size_t)it.id < cmds->size()) (*cmds)[(size_t)it.id].run(); });
}

/* Help > Keyboard Shortcuts: every shortcut in one filterable list; picking an entry that is a menu command runs it */
void show_shortcuts() {
    std::vector<Ed::PItem> items;
    for (const Menu &m : MENUS)
        for (const MenuItem &it : m.items)
            if (it.label[0] && it.keys[0] && it.act != A_NONE) items.push_back({std::string(it.label) + "    " + it.keys, it.act + 1, false});
    static const char *extra[] = {
        "Select Next Occurrence    Ctrl+D", "Move Line Up / Down    Alt+Up / Alt+Down", "Copy Line Up / Down    Shift+Alt+Up / Down", "Delete Line    Ctrl+Shift+K",
        "Insert Line Below / Above    Ctrl+Enter / Ctrl+Shift+Enter", "Select Line    Ctrl+L", "Toggle Line Comment    Ctrl+/", "Indent / Outdent    Ctrl+] / Ctrl+[",
        "Go to Tab 1-9    Alt+1 ... Alt+9", "Recent Tabs    Ctrl+Tab", "Next Change in a Diff    F7", "Save All    Ctrl+K Ctrl+S", "Close All Tabs    Ctrl+K Ctrl+W",
        "Keyboard Shortcuts    Ctrl+K Ctrl+H",
        "Vim: normal mode    Esc", "Vim: insert    i a I A o O", "Vim: move    h j k l w b e 0 ^ $ gg G f t % { }", "Vim: edit    x dd D cc C yy p P u Ctrl+R . J r ~",
        "Vim: operators    d y c > <  with motions and objects (diw ci\" da( yap)", "Vim: select    v V, then d y c > <", "Vim: command line    :w :q :wq :e file :42 /text n"};
    for (const char *e : extra) items.push_back({e, 0, false});
    pick_custom("Keyboard Shortcuts", items, [](const Ed::PItem &it) { if (it.id > 0) do_action(it.id - 1); });
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
    if (E.mode == M_DIALOG) {
        Dialog &d = E.dlg;
        int n = (int)d.btns.size();
        if (sym == SDLK_ESCAPE) dlg_finish(-1);
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) dlg_finish(d.focus);
        else if (sym == SDLK_TAB) d.focus = (d.focus + (shift ? n - 1 : 1)) % std::max(1, n);
        else if (sym == SDLK_LEFT && !d.input) d.focus = (d.focus + n - 1) % std::max(1, n);
        else if (sym == SDLK_RIGHT && !d.input) d.focus = (d.focus + 1) % std::max(1, n);
        else if (d.input && sym == SDLK_BACKSPACE) { if (d.fresh) { d.text.clear(); d.fresh = false; } else pop_utf8(d.text); }
        else if (d.input && sym == SDLK_v && ctrl) { char *c = SDL_GetClipboardText(); if (c) { if (d.fresh) { d.text.clear(); d.fresh = false; } d.text += c; SDL_free(c); } }
        else if (d.input && sym == SDLK_u && ctrl) d.text.clear();
        touch();
        return;
    }
    if (E.mode == M_CTX) {
        if (sym == SDLK_ESCAPE) { E.mode = M_EDIT; E.ctx.items.clear(); E.ctx.open.clear(); }
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
    if (E.mode == M_GITMSG) {
        if (sym == SDLK_ESCAPE) E.mode = M_EDIT;
        else if (sym == SDLK_BACKSPACE) pop_utf8(E.g.msg);
        else if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && ctrl) { E.mode = M_EDIT; git_commit_now(); }
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) E.g.msg += '\n';
        else if (sym == SDLK_v && ctrl) { char *t = SDL_GetClipboardText(); if (t) { E.g.msg += t; SDL_free(t); } }
        else if (sym == SDLK_u && ctrl) E.g.msg.clear();
        else return;
        E.g.err.clear();
        touch();
        return;
    }
    if (E.mode == M_QUICK) {
        int n = (int)E.quick_hits.size();
        if (sym == SDLK_ESCAPE) E.mode = M_EDIT;
        else if (sym == SDLK_UP) E.quick_sel = std::max(0, E.quick_sel - 1);
        else if (sym == SDLK_DOWN) E.quick_sel = std::min(n - 1, E.quick_sel + 1);
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { E.mode = M_EDIT; if (!E.quick_q.empty() && E.quick_q[0] == ':') goto_line(E.quick_q.substr(1)); else if (n) open_path(E.quick_hits[(size_t)E.quick_sel]); }
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
        bool alt2 = k.keysym.mod & KMOD_ALT;
        if (sym == SDLK_ESCAPE) { E.mode = M_EDIT; E.repl_on = false; E.repl_focus = false; }
        else if (sym == SDLK_h && ctrl) { E.repl_on = true; E.repl_focus = !E.repl_focus; }
        else if (sym == SDLK_TAB && E.repl_on) E.repl_focus = !E.repl_focus;
        else if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && E.repl_on && ctrl && alt2) replace_all();
        else if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && E.repl_on && E.repl_focus) replace_one();
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) find_from(d.sel_hi());
        else if (sym == SDLK_BACKSPACE && E.repl_focus) pop_utf8(E.repl_q);
        else if (sym == SDLK_v && ctrl && E.repl_focus) { char *c = SDL_GetClipboardText(); if (c) { E.repl_q += c; SDL_free(c); } }
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
    bool alt = k.keysym.mod & KMOD_ALT;
    if (E.mode == M_EDIT && vim_active() && !d.dv && !d.mv && !E.chord_k) {
        if (g_vim.cmd_kind) {
            if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) g_vim.cmd_enter(d);
            else if (sym == SDLK_ESCAPE) g_vim.cmd_cancel();
            else if (sym == SDLK_BACKSPACE) g_vim.cmd_back();
            else if (ctrl && sym == SDLK_u) g_vim.cmd.clear();
            else if (ctrl && sym == SDLK_v) { char *c = SDL_GetClipboardText(); if (c) { g_vim.cmd_text(c); SDL_free(c); } }
            E.follow = true; touch(); return;
        }
        if (sym == SDLK_ESCAPE || (ctrl && sym == SDLK_LEFTBRACKET)) { g_vim.escape(d); E.follow = true; touch(); return; }
        if (g_vim.mode == Vim::INSERT) { if (sym == SDLK_BACKSPACE && !ctrl) g_vim.erased(); }
        else if (!alt) {
            if (ctrl && !shift && sym == SDLK_r) { d.redo(); g_vim.after_history(d); E.follow = true; touch(); return; }
            if (ctrl && !shift && (sym == SDLK_d || sym == SDLK_u)) { g_vim.half_page(d, sym == SDLK_d ? 1 : -1); E.follow = true; touch(); return; }
            if (!ctrl) {
                const char *km = sym == SDLK_BACKSPACE ? "h" : sym == SDLK_DELETE ? "x" : (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) ? "+" : nullptr;
                if (km) { g_vim.text(d, km); E.follow = true; touch(); return; }
                if (sym == SDLK_TAB) return;
            }
        }
    }
    if (E.side_open && !E.pinned && sym == SDLK_ESCAPE) { open_side(false); touch(); return; }
    if (E.chord_k) {   /* the second key of Ctrl+K */
        E.chord_k = false;
        E.status_msg.clear();
        if (sym == SDLK_LSHIFT || sym == SDLK_RSHIFT || sym == SDLK_LCTRL || sym == SDLK_RCTRL) { E.chord_k = true; return; }
        if (sym == SDLK_t && ctrl) do_action(A_THEME);
        else if (sym == SDLK_o && ctrl) do_action(A_OPEN_FOLDER);
        else if (sym == SDLK_w && ctrl) do_action(A_CLOSE_ALL);
        else if (sym == SDLK_m) do_action(A_LANG);
        else if (sym == SDLK_h && ctrl) do_action(A_SHORTCUTS);
        else if (sym == SDLK_s && ctrl) { for (auto &dp : E.docs) if (dp->modified() && !dp->path.empty()) dp->save(nullptr); after_save(); }
        touch();
        return;
    }
    if (ctrl && !shift && !alt && sym == SDLK_k) { E.chord_k = true; E.status_msg = "(Ctrl+K) was pressed. Waiting for the second key of the chord..."; touch(); return; }
    if (alt && !ctrl && (sym == SDLK_UP || sym == SDLK_DOWN)) { begin_op(shift ? "copy line" : "move line"); if (shift) line_copy(d, sym == SDLK_UP ? -1 : 1); else line_move(d, sym == SDLK_UP ? -1 : 1); E.follow = true; touch(); return; }
    if (alt && !ctrl && !shift && sym >= SDLK_1 && sym <= SDLK_9) { if ((size_t)(sym - SDLK_1) < E.docs.size()) set_active((int)(sym - SDLK_1)); touch(); return; }
    if (alt && !ctrl && !shift && sym == SDLK_0) { set_active((int)E.docs.size() - 1); touch(); return; }
    if (ctrl && (sym == SDLK_PAGEUP || sym == SDLK_PAGEDOWN) && shift && E.docs.size() > 1) {   /* move the tab itself */
        int to = E.active + (sym == SDLK_PAGEUP ? -1 : 1);
        if (to >= 0 && to < (int)E.docs.size()) { std::swap(E.docs[(size_t)E.active], E.docs[(size_t)to]); E.active = to; E.tab_reveal = true; }
        touch();
        return;
    }
    if (g_peek.doc == &d && sym == SDLK_ESCAPE) { g_peek.doc = nullptr; touch(); return; }
    if (d.dv && diff_key(k, ctrl, shift)) return;
    if (d.mv && merge_key(k, ctrl, shift)) return;
    if (ctrl && !alt && (sym == SDLK_LEFT || sym == SDLK_RIGHT)) { d.break_group(); d.move_to(sym == SDLK_LEFT ? word_left(d, d.cur) : word_right(d, d.cur), shift, false); E.follow = true; touch(); return; }
    if (ctrl && !shift && (sym == SDLK_BACKSPACE || sym == SDLK_DELETE)) {
        if (!d.has_sel()) { size_t o = sym == SDLK_BACKSPACE ? word_left(d, d.cur) : word_right(d, d.cur); d.anchor = o; }
        d.erase_back();   /* with a selection set, Backspace removes exactly it */
        E.follow = true; touch(); return;
    }
    if (ctrl && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)) { line_open(d, !shift); E.follow = true; touch(); return; }
    if (ctrl && !shift && sym == SDLK_g) { do_action(A_GOTO); touch(); return; }
    if (ctrl && !shift && sym == SDLK_l) { size_t l = d.buf.line_of(d.cur); size_t e = l + 1 < d.buf.lines() ? d.buf.line_start(l + 1) : d.buf.size(); set_sel(d, d.buf.line_start(l), e); touch(); return; }
    if (ctrl && shift && sym == SDLK_k) { line_delete(d); E.follow = true; touch(); return; }
    if (ctrl && !shift && sym == SDLK_SLASH) { toggle_comment(d); E.follow = true; touch(); return; }
    if (ctrl && !shift && (sym == SDLK_RIGHTBRACKET || sym == SDLK_LEFTBRACKET)) { indent_lines(d, sym == SDLK_LEFTBRACKET); E.follow = true; touch(); return; }
    if (ctrl && !shift && sym == SDLK_d) { select_next_occurrence(d); E.follow = true; touch(); return; }
    if (ctrl && shift && sym == SDLK_t) { do_action(A_REOPEN); touch(); return; }
    if (sym == SDLK_TAB && !ctrl && (shift || (d.has_sel() && d.buf.line_of(d.sel_lo()) != d.buf.line_of(d.sel_hi())))) { indent_lines(d, shift); E.follow = true; touch(); return; }
    if (sym == SDLK_F3 && !E.find_q.empty()) { find_from(d.sel_hi()); E.follow = true; touch(); return; }
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
        if (ctrl) { mru_cycle(shift ? -1 : 1); touch(); return; }
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
    case SDLK_h: if (ctrl && !shift) { do_action(A_REPLACE); touch(); } else return; return;
    case SDLK_p: if (ctrl && shift) do_action(A_PALETTE); else if (ctrl) do_action(A_QUICK); else return; return;
    case SDLK_F1: do_action(A_PALETTE); return;
    case SDLK_b: if (ctrl) do_action(A_TOGGLE_SIDEBAR); else return; return;
    case SDLK_e: if (ctrl && shift) do_action(A_VIEW_EXPLORER); else return; return;
    case SDLK_g: if (ctrl && shift) do_action(A_VIEW_GIT); else return; return;
    case SDLK_BACKQUOTE: if (ctrl) do_action(shift ? A_TERM_NEW : A_TERM_TOGGLE); else return; return;
    case SDLK_i: if (ctrl && shift) do_action(A_STATS); else return; return;
    default: return;
    }
    E.follow = true;
    touch();
}

void copy_text(const std::string &t) { SDL_SetClipboardText(t.c_str()); E.status_msg = "copied"; }

void context_menu_at(const Hit &h, float mx, float my) {
    if (h.kind == H_GITROW || h.kind == H_GITOPEN || h.kind == H_GITSTG || h.kind == H_GITDISC) {
        size_t idx = (size_t)(h.id / 2);
        bool staged = (h.id & 1) != 0;
        if (h.id < 0 || idx >= E.git.changes.size()) return;
        GitChange c = E.git.changes[idx];
        bool unt = c.x == '?' && c.y == '?';
        std::vector<CtxItem> v = {ci("Open Changes", [c, staged] { if (!E.pinned) open_side(false); open_change_diff(c, staged); }),
                                  ci("Open File", [c] { if (!E.pinned) open_side(false); open_path(E.git.root + "/" + c.path); }, "", c.y != 'D' || unt), ci_sep()};
        if (!staged) v.push_back(ci("Discard Changes", [idx] { discard_files({(int)idx * 1}); }));
        v.push_back(ci(staged ? "Unstage Changes" : "Stage Changes", [c, staged] { begin_op(staged ? "git unstage" : "git stage"); git_do(staged ? git_unstage(E.git.root, c.path) : git_stage(E.git.root, c.path), staged ? "unstage" : "stage"); }));
        if (unt) { v.push_back(ci_sep()); v.push_back(ci("Add to .gitignore", [c] { git_add_ignore(c.path); })); }
        v.push_back(ci_sep());
        v.push_back(ci("Copy Path", [c] { copy_text(E.git.root + "/" + c.path); }));
        v.push_back(ci("Copy Relative Path", [c] { copy_text(c.path); }));
        ctx_show(mx, my, std::move(v));
    } else if (h.kind == H_GITSEC && h.id < 2) {
        if (h.id == 0) ctx_show(mx, my, {ci("Unstage All Changes", [] { git_unstage(E.git.root, "."); git_refresh(); }), ci_sep(), ci(E.g.tree_view ? "View as List" : "View as Tree", [] { E.g.tree_view = !E.g.tree_view; settings_save(); })});
        else ctx_show(mx, my, {ci("Stage All Changes", [] { git_stage_all(E.git.root); git_refresh(); }), ci("Discard All Changes", [] { discard_all_changes(); }), ci_sep(), ci(E.g.tree_view ? "View as List" : "View as Tree", [] { E.g.tree_view = !E.g.tree_view; settings_save(); })});
    } else if ((h.kind == H_GITLOG || h.kind == H_GITTL) && h.id >= 0) {
        const std::vector<GitCommit> &src = h.kind == H_GITLOG ? E.g.log : E.g.tl;
        if ((size_t)h.id >= src.size()) return;
        GitCommit c = src[(size_t)h.id];
        ctx_show(mx, my, {ci("Copy Commit Hash", [c] { copy_text(c.hash); }), ci("Copy Commit Message", [c] { copy_text(c.subject); }), ci_sep(),
                          ci("Select for Compare", [c] { E.g.mark_hash = c.hash; E.g.mark_label = c.hash.substr(0, 7); }),
                          ci_sep(), ci("Checkout (Detached)", [c] { git_checkout(c.hash, false); }), ci("Create Branch...", [c] { git_create_branch(c.hash); }), ci("Create Tag...", [c] { git_tag_create(c.hash); }),
                          ci("Cherry Pick", [c] { std::string o; if (git_sync_run("cherry-pick", "cherry-pick " + git_quote(c.hash), &o, false)) { git_refresh(); reload_open_docs(); } else { git_error("cherry-pick", o); git_refresh(); } })});
    } else if (h.kind == H_RROW && h.id >= 0 && (size_t)h.id < g_rrows.size()) {
        RRow r = g_rrows[(size_t)h.id];
        remote_menu(r.prof, r.path, r.kind, mx, my);
    } else if (h.kind == H_TAB && h.id >= 0 && (size_t)h.id < E.docs.size()) {
        int i = h.id;
        bool has_path = !E.docs[(size_t)i]->path.empty();
        auto close_range = [](int from, int to, int keep) {   /* closes the clean tabs in [from, to), the dirty ones stay */
            for (int k = to - 1; k >= from; k--) if (k != keep && k < (int)E.docs.size() && !E.docs[(size_t)k]->modified()) { E.confirm_idx = k; remove_doc(k); }
        };
        ctx_show(mx, my, {ci("Close", [i] { close_tab(i); }, "Ctrl+W"), ci("Close Others", [i, close_range] { close_range(0, (int)E.docs.size(), i); }), ci("Close to the Right", [i, close_range] { close_range(i + 1, (int)E.docs.size(), -1); }),
                          ci("Close All", [] { close_all_tabs(); }, "Ctrl+K W"), ci_sep(), ci("Copy Path", [i] { copy_text(E.docs[(size_t)i]->path); }, "", has_path),
                          ci("Copy Relative Path", [i] { copy_text(rel_to_root(E.docs[(size_t)i]->path)); }, "", has_path)});
    }
}

/* Stage, unstage or revert the current change of a diff tab by rebuilding the file from the two sides: the lines outside the change are kept. */
std::vector<std::string> split_keep(const std::string &t) {
    std::vector<std::string> v;
    size_t p0 = 0;
    while (p0 < t.size()) { size_t e = t.find('\n', p0); if (e == std::string::npos) e = t.size() - 1; v.push_back(t.substr(p0, e - p0 + 1)); p0 = e + 1; }
    return v;
}

void diff_hunk_apply(DiffView &v, int op) {   /* 4 stage, 5 revert, 6 unstage */
    if (v.cur_change < 0 || (size_t)v.cur_change >= v.m.hunks.size()) { E.status_msg = "Select a change first (F7)"; return; }
    DiffHunk h = v.m.hunks[(size_t)v.cur_change];
    std::string root = v.root, path = v.path;
    if (op == 4 && v.left_rev.empty()) { git_do(git_stage(root, path), "stage"); return; }
    if (op == 4 && v.right_rev == "WORK") { struct stat st0; if (stat((root + "/" + path).c_str(), &st0) != 0) { git_do(git_stage(root, path), "stage"); return; } }   /* the file is gone: staging the change stages the deletion */
    std::string lt = v.left_rev.empty() ? std::string() : git_show(root, v.left_rev, path);
    std::string rt;
    if (v.right_rev == "WORK") { FILE *f = fopen((root + "/" + path).c_str(), "rb"); if (f) { char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) rt.append(buf, n); fclose(f); } }
    else rt = git_show(root, v.right_rev, path);
    std::vector<std::string> L = split_keep(lt), R = split_keep(rt);
    if (L.size() != v.m.a.size() || R.size() != v.m.b.size()) { E.status_msg = "the file changed since the diff was made; refresh and try again"; git_refresh(); return; }
    auto splice = [&](const std::vector<std::string> &base, int b0, int bn, const std::vector<std::string> &with, int w0, int wn) {
        std::string o;
        for (int i = 0; i < b0; i++) o += base[(size_t)i];
        for (int i = 0; i < wn; i++) o += with[(size_t)(w0 + i)];
        for (size_t i = (size_t)(b0 + bn); i < base.size(); i++) o += base[i];
        return o;
    };
    std::string out;
    if (op == 4) out = splice(L, h.a0, h.an, R, h.b0, h.bn);          /* new index: the index with this change taken from the file */
    else if (op == 6) out = splice(R, h.b0, h.bn, L, h.a0, h.an);     /* new index: the index with this change put back to HEAD */
    else out = splice(R, h.b0, h.bn, L, h.a0, h.an);                  /* new file: the file with this change put back to the index */
    if (op == 5) {
        FILE *f = fopen((root + "/" + path).c_str(), "wb");
        if (!f) { E.status_msg = "cannot write " + path; return; }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        git_refresh(); reload_open_docs();
        E.status_msg = "change reverted";
        return;
    }
    char tmpl[] = "/tmp/hootedit-hunk-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return;
    if (write(fd, out.data(), out.size()) != (ssize_t)out.size()) { close(fd); unlink(tmpl); return; }
    close(fd);
    std::string sha, ls;
    git_exec(root, "hash-object -w " + git_quote(tmpl), &sha);
    unlink(tmpl);
    sha = trim_nl(sha);
    git_exec(root, "ls-files -s -- " + git_quote(path), &ls);
    std::string mode = ls.size() > 6 ? ls.substr(0, 6) : "100644";
    std::string o;
    if (sha.size() >= 40 && git_exec(root, "update-index --add --cacheinfo " + mode + "," + sha.substr(0, 40) + "," + git_quote(path), &o) == 0) { git_refresh(); E.status_msg = op == 4 ? "change staged" : "change unstaged"; }
    else git_error(op == 4 ? "stage change" : "unstage change", o);
}

void diff_change_op(const std::shared_ptr<DiffView> &dv, int op) {   /* 4 stage, 5 revert (asks first), 6 unstage: the current change */
    DiffView &v = *dv;
    if (op != 5) { diff_hunk_apply(v, op); return; }
    if (v.cur_change < 0) { E.status_msg = "Select a change first (F7)"; return; }
    std::shared_ptr<DiffView> keep = dv;
    dlg_show("Are you sure you want to revert this change in '" + base_name(v.path) + "'?", "The change is removed from the file on disk. This action is irreversible!", {"Revert Change", "Cancel"}, [keep](int i, const std::string &) { if (i == 0) diff_hunk_apply(*keep, 5); });
}

void click_hit(const Hit &h) {
    bool term_hit = h.kind == H_TERMTAB || h.kind == H_SPLIT || (h.kind == H_ACT && (h.id == A_TERM_NEW || h.id == A_TERM_KILL || h.id == A_TERM_TOGGLE));
    E.term_focus = term_hit && E.term_open;
    if (term_hit && h.kind == H_ACT) E.term_focus = true;
    switch (h.kind) {
    case H_TAB: if (g_clicks >= 2 && h.id < (int)E.docs.size()) E.docs[(size_t)h.id]->preview = false; set_active(h.id); E.mode = M_EDIT; E.tab_press = h.id; E.tab_press_x = E.mx; E.tab_dragging = false; break;
    case H_TABCLOSE: E.mode = M_EDIT; close_tab(h.id >= 1000 ? h.id - 1000 : h.id); break;
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
    case H_SECT: if (h.id == 0) E.opened_open = !E.opened_open; else E.outline_open = !E.outline_open; break;
    case H_OPENED: if (h.id < (int)E.docs.size()) { if (g_clicks >= 2) E.docs[(size_t)h.id]->preview = false; set_active(h.id); E.mode = M_EDIT; } break;
    case H_OUTLINE: { const std::vector<OItem> &oi = outline_for(D()); if ((size_t)h.id < oi.size()) { E.mode = M_EDIT; goto_line(std::to_string(oi[(size_t)h.id].line + 1)); } break; }
    case H_TREE: {
        const std::vector<Tree::Row> &rows = E.tree.rows();
        if ((size_t)h.id >= rows.size()) break;
        if (rows[(size_t)h.id].dir) E.tree.toggle((size_t)h.id);
        else if (rows[(size_t)h.id].ghost) {   /* a deleted file: show what was removed */
            std::string p = rows[(size_t)h.id].path;
            for (const GitChange &c : E.git.changes) if (E.git.root + "/" + c.path == p) { if (!E.pinned) open_side(false); open_change_diff(c, c.y == ' '); break; }
        }
        else { std::string p = rows[(size_t)h.id].path; if (!E.pinned) open_side(false); open_path(p, g_clicks < 2); }
        break;
    }
    case H_ACTIVITY: show_view((View)h.id); break;
    case H_PIN: do_action(A_PIN); break;
    case H_GITROW: case H_GITOPEN: case H_GITSTG: {
        size_t idx = (size_t)(h.id / 2);
        bool staged = (h.id & 1) != 0;
        if (idx >= E.git.changes.size()) break;
        GitChange c = E.git.changes[idx];
        E.mode = M_EDIT;
        if (h.kind == H_GITSTG) { begin_op(staged ? "git unstage" : "git stage"); git_do(staged ? git_unstage(E.git.root, c.path) : git_stage(E.git.root, c.path), staged ? "unstage" : "stage"); break; }
        if (!E.pinned) open_side(false);
        bool conflict = c.x == 'U' || c.y == 'U' || (c.x == 'A' && c.y == 'A') || (c.x == 'D' && c.y == 'D');
        if (conflict && h.kind != H_GITOPEN && !(SDL_GetModState() & KMOD_CTRL)) merge_open(c.path);
        else if (h.kind == H_GITOPEN || conflict || (SDL_GetModState() & KMOD_CTRL)) open_path(E.git.root + "/" + c.path); else open_change_diff(c, staged);
        break;
    }
    case H_GITSEC: {
        bool *f[5] = {&E.g.open_staged, &E.g.open_changes, &E.g.open_graph, &E.g.open_tl, &E.g.open_merge};
        if (h.id >= 0 && h.id < 5) *f[h.id] = !*f[h.id];
        break;
    }
    case H_BLAME: {
        Doc &d = D();
        const GitBlame *bl = blame_for(d, d.buf.line_of(d.cur));
        if (bl) { GitBlame b = *bl; open_blame_commit(b, rel_to_root(d.path)); }
        break;
    }
    case H_GUTTER: {   /* a click on a change bar opens or closes the inline peek of that change */
        Doc &d = D();
        if (g_peek.doc == &d && g_peek.b0 == h.id) g_peek.doc = nullptr; else { g_peek.doc = &d; g_peek.b0 = h.id; }
        break;
    }
    case H_PEEK: {
        Doc &d = D();
        if (g_peek.doc != &d || h.id == 0) break;
        GutterCache *gc = gutter_for(d);
        if (h.id == 3) g_peek.doc = nullptr;
        else if (h.id == 1) peek_revert(d);
        else if (h.id == 2) { int b0 = g_peek.b0; g_peek.doc = nullptr; peek_open_diff(d, b0); }
        else if ((h.id == 4 || h.id == 5) && gc) {
            for (size_t k = 0; k < gc->hunks.size(); k++) if (gc->hunks[k].b0 == g_peek.b0) {
                size_t to = h.id == 5 ? k + 1 : k;
                if (h.id == 4) { if (k == 0) break; to = k - 1; }
                if (to >= gc->hunks.size()) break;
                g_peek.b0 = gc->hunks[to].b0;
                size_t ln = std::min((size_t)gc->hunks[to].b0, d.buf.lines() - 1);
                d.break_group(); d.move_to(d.buf.line_start(ln), false, false);
                E.follow = true;
                break;
            }
        }
        break;
    }
    case H_DIFFFOLD: {
        Doc &d = D();
        if (!d.dv || (size_t)h.id >= d.dv->vrows.size()) break;
        d.dv->open_folds.push_back(d.dv->vrows[(size_t)h.id].row);
        d.dv->vdirty = true;
        break;
    }
    case H_DIFFHUNK: {   /* the buttons that appear on the change under the mouse: id = change * 8 + operation */
        Doc &d = D();
        if (!d.dv || (size_t)(h.id / 8) >= d.dv->m.hunks.size()) break;
        d.dv->cur_change = h.id / 8;
        diff_change_op(d.dv, h.id % 8);
        break;
    }
    case H_DIFFBTN: {
        Doc &d = D();
        if (!d.dv) break;
        DiffView &v = *d.dv;
        Layout L = layout();
        if (h.id == 0) diff_step(v, -1, L);
        else if (h.id == 1) diff_step(v, 1, L);
        else if (h.id == 2) { v.mode = v.vinline ? 1 : 2; v.vdirty = true; }
        else if (h.id == 4 || h.id == 5 || h.id == 6) diff_change_op(d.dv, h.id);
        else if (h.id == 3) { std::string p = v.root + "/" + v.path; struct stat st; if (stat(p.c_str(), &st) == 0) open_path(p); else E.status_msg = "the file is not on disk any more"; }
        break;
    }
    case H_GITOP: git_op_do(h.id); break;
    case H_CONFLICT: if (h.id % 4 == 3) { merge_open(rel_to_root(D().path)); break; } conflict_accept(D(), (size_t)(h.id / 4), h.id % 4); break;
    case H_MERGE: merge_op(h.id); break;
    case H_REPL: if (h.id == 0) replace_one(); else if (h.id == 1) replace_all(); else if (h.id == 2) E.repl_focus = true; else if (h.id == 3) E.repl_focus = false; break;
    case H_GITREFRESH: begin_op("git refresh"); git_refresh(); break;
    case H_GITALL: {
        begin_op(h.id == 0 ? "git stage all" : "git unstage all");
        git_do(h.id == 0 ? git_stage_all(E.git.root) : git_unstage(E.git.root, "."), h.id == 0 ? "stage all" : "unstage all");
        break;
    }
    case H_GITMSG: E.mode = M_GITMSG; break;
    case H_GITCOMMIT: E.mode = M_EDIT; if (h.id == 1) git_sync_flow(); else if (h.id == 2) git_push_flow(); else git_commit_now(); break;
    case H_GITDD: git_commit_menu(E.mx - (float)S(150), E.my + (float)S(14)); break;
    case H_GITMORE: git_more_menu(E.mx - (float)S(8), E.my + (float)S(14)); break;
    case H_GITBRANCH: git_branch_picker(); break;
    case H_GITDISC: if (h.id < 0) discard_all_changes(); else discard_files({h.id / 2}); break;
    case H_GITDIR: if ((size_t)h.id < g_gitdirs.size()) { const std::string k = g_gitdirs[(size_t)h.id]; if (!E.g.collapsed.erase(k)) E.g.collapsed.insert(k); } break;
    case H_GITLOG: {
        GitUi &g = E.g;
        if ((size_t)h.id >= g.log.size()) break;
        const GitCommit &c = g.log[(size_t)h.id];
        Uint16 mod = SDL_GetModState();
        E.mode = M_EDIT;
        if (mod & KMOD_CTRL) { g.mark_hash = c.hash; g.mark_label = c.hash.substr(0, 7); break; }
        if ((mod & KMOD_SHIFT) && !g.mark_hash.empty() && g.mark_hash != c.hash) {   /* the marked commit is the left side, this one the right */
            g.log_sel = h.id; g.cmp_a = g.mark_hash; g.cmp_b = c.hash;
            g.sel_files = git_compare_files(E.git.root, g.cmp_a, g.cmp_b);
            break;
        }
        if (g.log_sel == h.id) { g.log_sel = -1; g.sel_files.clear(); g.cmp_a.clear(); g.cmp_b.clear(); break; }
        g.log_sel = h.id; g.cmp_a.clear(); g.cmp_b.clear();
        g.sel_files = git_commit_files(E.git.root, c.hash);
        break;
    }
    case H_GITCFILE: {
        GitUi &g = E.g;
        if (g.log_sel < 0 || (size_t)g.log_sel >= g.log.size() || (size_t)h.id >= g.sel_files.size()) break;
        E.mode = M_EDIT;
        if (!E.pinned) open_side(false);
        const GitFile &f = g.sel_files[(size_t)h.id];
        if (!g.cmp_a.empty()) open_compare_diff(f.path, g.cmp_a, g.cmp_b);
        else open_commit_diff(g.log[(size_t)g.log_sel], f);
        break;
    }
    case H_GITTL: {
        GitUi &g = E.g;
        if ((size_t)h.id >= g.tl.size()) break;
        const GitCommit &c = g.tl[(size_t)h.id];
        Uint16 mod = SDL_GetModState();
        E.mode = M_EDIT;
        if (mod & KMOD_CTRL) { g.mark_hash = c.hash; g.mark_label = c.hash.substr(0, 7); break; }
        if (!E.pinned) open_side(false);
        if ((mod & KMOD_SHIFT) && !g.mark_hash.empty() && g.mark_hash != c.hash) open_compare_diff(g.tl_path, g.mark_hash, c.hash);
        else open_commit_diff(c, GitFile{'M', g.tl_path});
        break;
    }
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
    case H_RROW: remote_row_click(h.id); break;
    case H_RNEW: remote_new_connection(); break;
    case H_DLGBTN: dlg_finish(h.id); return;
    case H_DLGBG: break;
    case H_CTXITEM: ctx_click(h.id); break;
    case H_CTXBG: E.mode = M_EDIT; E.ctx.items.clear(); E.ctx.open.clear(); break;
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
    if (E.mode == M_DIALOG) { if (E.dlg.input) { if (E.dlg.fresh) { E.dlg.text.clear(); E.dlg.fresh = false; } E.dlg.text += s; touch(); } return; }
    if (E.mode == M_CTX) return;
    if (E.mode == M_EDIT && (D().dv || D().mv)) return;   /* diff and merge tabs are read only */
    if (E.mode == M_EDIT) begin_op("keystroke");
    else if (E.mode == M_QUICK) begin_op("quick-open filter", std::to_string(E.quick_files.size()) + " files");
    else if (E.mode == M_SIDE && E.view == V_EXPLORER) begin_op("file filter", std::to_string(E.quick_files.size()) + " files");
    if (E.mode == M_FIND) { if (E.repl_focus) { E.repl_q += s; touch(); return; } E.find_q += s; find_from(D().sel_lo()); touch(); return; }
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
    if (E.mode == M_GITMSG) { E.g.msg += s; E.g.err.clear(); touch(); return; }
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
    if (E.mode == M_EDIT && vim_active()) {
        if (g_vim.cmd_kind) { g_vim.cmd_text(s); touch(); return; }
        if (g_vim.mode != Vim::INSERT) { g_vim.text(D(), s); E.follow = true; touch(); return; }
        g_vim.typed(s);
    }
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
    ctx_hover();
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
        if (E.mx >= (float)L.ed_x && E.my >= (float)L.tab_y && E.my < (float)L.text_y && !splash()) {   /* over the tab strip: slide the tabs, keep the active one */
            E.tab_scroll += (float)(e.wheel.x - e.wheel.y) * (float)S(70);
            redraw();
            break;
        }
        if (E.side_open && E.mx < (float)L.side_x1 && E.my >= (float)L.menu_h && E.my < (float)L.bar_y) {
            E.side_scroll = std::max(0.f, E.side_scroll - (float)e.wheel.y * (float)S(66));
            redraw();
            break;
        }
        Doc &d = D();
        if (d.dv) {
            d.dv->top -= (long)e.wheel.y * 3;
            if (e.wheel.x) d.dv->left = std::max(0, d.dv->left + e.wheel.x * 4);
            diff_clamp(*d.dv, L);
            redraw();
            break;
        }
        if (d.mv) {
            Layout L2 = layout();
            MergeGeom g2 = merge_geom(L2);
            if (E.my >= g2.body_y + (float)g2.top_rows * (float)E.rowh) d.mv->rtop -= (long)e.wheel.y * 3; else d.mv->top -= (long)e.wheel.y * 3;
            merge_clamp(*d.mv, g2);
            redraw();
            break;
        }
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
        if (e.button.button == SDL_BUTTON_RIGHT && (E.mode == M_EDIT || E.mode == M_CTX)) {
            float mx = (float)e.button.x * E.scale, my = (float)e.button.y * E.scale;
            E.mx = mx; E.my = my;
            if (E.mode == M_CTX) { E.mode = M_EDIT; E.ctx.items.clear(); E.ctx.open.clear(); redraw(); break; }
            for (size_t i = E.hits.size(); i-- > 0;) {
                const Hit &h = E.hits[i];
                if (mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h) { Hit c = h; context_menu_at(c, mx, my); break; }
            }
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
            bool was_overlay = E.mode == M_PICK || E.mode == M_FIND || E.mode == M_MENU || E.mode == M_QUICK || E.mode == M_SIDE || E.mode == M_GITMSG;
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
    case SDL_KEYUP:
        if (e.key.keysym.sym == SDLK_LCTRL || e.key.keysym.sym == SDLK_RCTRL) mru_end();
        break;
    case SDL_MOUSEMOTION:
        if (g_scripted && e.motion.which != SCRIPT_MOUSE) break;   /* --script drives the pointer; the real one (xvfb parks it mid-window) is ignored */
        E.mx = (float)e.motion.x * E.scale; E.my = (float)e.motion.y * E.scale;
        if (E.tip_on) { E.tip_on = false; redraw(); }
        E.tip_at = now_s() + 0.5;
        if (E.tab_press >= 0 && (e.motion.state & SDL_BUTTON_LMASK)) {
            Layout L = layout();
            if (!E.tab_dragging && std::fabs(E.mx - E.tab_press_x) > (float)S(6)) E.tab_dragging = true;
            if (E.tab_dragging) {
                if (E.mx < (float)L.ed_x + (float)S(30)) E.tab_scroll -= (float)S(16);   /* near an edge the strip follows the pointer */
                else if (E.mx > (float)E.W - (float)S(30)) E.tab_scroll += (float)S(16);
                float px = E.mx - (float)L.ed_x + E.tab_scroll;   /* pointer in strip coordinates */
                int to = E.tab_press;
                for (size_t i = 0; i < E.tab_x.size(); i++) if (px >= E.tab_x[i] && px < E.tab_x[i] + E.tab_w[i]) to = (int)i;
                if (px < 0) to = 0;
                if (!E.tab_x.empty() && px >= E.tab_x.back() + E.tab_w.back()) to = (int)E.tab_x.size() - 1;
                if (to != E.tab_press && to >= 0 && to < (int)E.docs.size()) {
                    auto &v = E.docs;
                    if (to > E.tab_press) std::rotate(v.begin() + E.tab_press, v.begin() + E.tab_press + 1, v.begin() + to + 1);
                    else std::rotate(v.begin() + to, v.begin() + E.tab_press, v.begin() + E.tab_press + 1);
                    if (E.active == E.tab_press) E.active = to;
                    else if (E.tab_press < E.active && to >= E.active) E.active--;
                    else if (E.tab_press > E.active && to <= E.active) E.active++;
                    if (E.confirm_idx == E.tab_press) E.confirm_idx = to;
                    E.tab_press = to;
                }
                redraw();
            }
        } else if (E.sb_drag) sb_to_pointer();
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
        E.tab_press = -1;
        if (E.tab_dragging) { E.tab_dragging = false; redraw(); }
        break;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) { SDL_GL_GetDrawableSize(E.win, &E.W, &E.H); redraw(); }
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            E.focused = true;
            static double last_git = 0;
            if (E.git.repo && now_s() - last_git > 1.5) { last_git = now_s(); git_refresh(); }   /* the files may have changed in another program */
            touch();
        }
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
            double until = now_s() + atof(step.c_str() + (step[5] == '~' ? 6 : 5)) / 1000.0;
            while (now_s() < until && (E.s_active && !E.sstat.done)) { search_poll(); SDL_Delay(1); }
            while (now_s() < until && (g_job.running || g_rjob.running || step.compare(0, 6, "wait:~") == 0)) { git_job_poll(); job_poll(g_rjob); askpass_poll(); tip_poll(); SDL_Delay(5); }   /* wait:~MS also keeps polling for password prompts */
            search_poll(); git_job_poll(); job_poll(g_rjob); askpass_poll(); E.ev_t0 = 0; frame_if_timed();
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
        if (step.compare(0, 4, "mod:") == 0) {   /* mod:ctrl, mod:shift or mod:none holds a modifier for the following clicks (test hook) */
            SDL_SetModState(step == "mod:ctrl" ? KMOD_LCTRL : step == "mod:shift" ? KMOD_LSHIFT : KMOD_NONE);
            continue;
        }
        if (step.compare(0, 6, "wheel:") == 0) {   /* wheel:x,y,dy over a point (test hook) */
            int x = 0, y = 0, dy = 0;
            sscanf(step.c_str() + 6, "%d,%d,%d", &x, &y, &dy);
            E.mx = (float)x; E.my = (float)y;
            SDL_Event ev = {};
            ev.type = SDL_MOUSEWHEEL; ev.wheel.y = dy;
            handle_event(ev);
            build_frame();
            frame_if_timed();
            continue;
        }
        if (step.compare(0, 5, "drag:") == 0) {   /* press at x0,y0, move to x1,y1 in steps with the button held, release */
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            sscanf(step.c_str() + 5, "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
            build_frame();   /* the hit boxes must be those of the current state */
            SDL_Event dn = {};
            dn.type = SDL_MOUSEBUTTONDOWN; dn.button.button = SDL_BUTTON_LEFT; dn.button.clicks = 1; dn.button.x = (int)((float)x0 / E.scale); dn.button.y = (int)((float)y0 / E.scale);
            handle_event(dn);
            build_frame();
            for (int i = 1; i <= 8; i++) {
                SDL_Event mv = {};
                mv.type = SDL_MOUSEMOTION; mv.motion.which = SCRIPT_MOUSE; mv.motion.state = SDL_BUTTON_LMASK;
                mv.motion.x = (int)((float)(x0 + (x1 - x0) * i / 8) / E.scale); mv.motion.y = (int)((float)(y0 + (y1 - y0) * i / 8) / E.scale);
                handle_event(mv);
                build_frame();
            }
            SDL_Event up = {};
            up.type = SDL_MOUSEBUTTONUP;
            handle_event(up);
            build_frame();
            frame_if_timed();
            continue;
        }
        if (step.compare(0, 7, "rclick:") == 0) {
            int x = 0, y = 0;
            sscanf(step.c_str() + 7, "%d,%d", &x, &y);
            SDL_Event ev = {};
            ev.type = SDL_MOUSEBUTTONDOWN; ev.button.button = SDL_BUTTON_RIGHT; ev.button.clicks = 1; ev.button.x = (int)((float)x / E.scale); ev.button.y = (int)((float)y / E.scale);
            handle_event(ev);
            build_frame();
            frame_if_timed();
            continue;
        }
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

/* vim mode: the engine lives in vim.cpp; this wires it to the clipboard, the find box, tabs and the ":" commands */
void vim_search_next() {
    Doc &d = D();
    if (E.find_q.empty()) return;
    if (find_from(d.next_char(d.cur))) { size_t p = d.sel_lo(); d.anchor = d.cur = p; E.follow = true; }
}

void vim_exec(char kind, const std::string &raw) {
    Doc &d = D();
    std::string c = raw;
    while (!c.empty() && c.back() == ' ') c.pop_back();
    size_t b = c.find_first_not_of(' ');
    c = b == std::string::npos ? std::string() : c.substr(b);
    if (kind == '/') {
        if (c.empty()) return;
        E.find_q = c;
        E.find_msg.clear();
        if (find_from(d.cur)) { size_t p = d.sel_lo(); d.anchor = d.cur = p; E.follow = true; }
        else E.status_msg = "E486: Pattern not found: " + c;
        return;
    }
    if (c.empty()) return;
    bool digits = c.find_first_not_of("0123456789") == std::string::npos;
    if (digits) { goto_line(c); return; }
    if (c == "$") { goto_line(std::to_string(d.buf.lines())); return; }
    std::string cmd = c, arg;
    size_t sp = c.find(' ');
    if (sp != std::string::npos) { cmd = c.substr(0, sp); arg = c.substr(sp + 1); }
    bool bang = !cmd.empty() && cmd.back() == '!';
    if (bang) cmd.pop_back();
    if (cmd == "w" || cmd == "write") { if (d.path.empty() && arg.empty()) do_action(A_SAVEAS); else if (!arg.empty()) { std::string p = arg[0] == '/' ? arg : (E.tree.root().empty() ? arg : E.tree.root() + "/" + arg); E.status_msg = d.save(p.c_str()) ? "saved " + p : "SAVE FAILED: " + p; } else save_active(); }
    else if (cmd == "q" || cmd == "quit" || cmd == "close") { if (bang) { E.confirm_idx = E.active; remove_doc(E.active); } else close_tab(E.active); }
    else if (cmd == "wq" || cmd == "x" || cmd == "xit" || cmd == "exit") { save_active(); if (!D().modified()) close_tab(E.active); }
    else if (cmd == "wa" || cmd == "wall") { for (auto &dp : E.docs) if (dp->modified() && !dp->path.empty()) dp->save(nullptr); after_save(); E.status_msg = "saved all"; }
    else if (cmd == "qa" || cmd == "qall" || cmd == "wqa" || cmd == "xa") { if (cmd == "wqa" || cmd == "xa") { for (auto &dp : E.docs) if (dp->modified() && !dp->path.empty()) dp->save(nullptr); } if (bang) E.running = false; else request_quit(); }
    else if (cmd == "e" || cmd == "edit" || cmd == "o" || cmd == "open") { if (arg.empty()) do_action(A_OPEN_FILE); else open_path(arg[0] == '/' ? arg : (E.tree.root().empty() ? arg : E.tree.root() + "/" + arg)); }
    else if (cmd == "bn" || cmd == "bnext" || cmd == "tabn") do_action(A_NEXT_TAB);
    else if (cmd == "bp" || cmd == "bprev" || cmd == "tabp") do_action(A_PREV_TAB);
    else if (cmd == "noh" || cmd == "nohlsearch" || cmd == "set" || cmd == "syntax") {}
    else if (cmd == "help" || cmd == "h") show_shortcuts();
    else E.status_msg = "E492: Not an editor command: " + c;
}

void vim_init() {
    g_vim.set_clip = [](const std::string &t) { SDL_SetClipboardText(t.c_str()); };
    g_vim.get_clip = []() { std::string r; char *c = SDL_GetClipboardText(); if (c) { r = c; SDL_free(c); } return r; };
    g_vim.exec = vim_exec;
    g_vim.search_next = vim_search_next;
    g_vim.page_rows = []() { return std::max(2, layout().rows); };
    vim_reset();
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--askpass")) return askpass_client(argc >= 3 ? argv[2] : "");   /* git and ssh start us as their password helper */
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
    vim_init();
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
    askpass_start();
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
        SDL_GL_SetSwapInterval(0);   /* with vsync on, the wait for the display lands in build_frame and hides the real cost */
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
                uint8_t sync[4];
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sync);   /* waits for the GPU, so the frame time is the time to finished pixels */
                double c2 = now_s();
                sum_build += c1 - c0; sum_full += c2 - c0;
                if (c1 - c0 > worst) worst = c1 - c0;
            }
            printf("bench pass %d (%s): build_frame %.1f us avg, worst %.1f us; with GL submit+swap+finish %.1f us (%s)\n", pass + 1,
                   pass == 0 ? "cold caches" : "warm row cache", sum_build / bench * 1e6, worst * 1e6, sum_full / bench * 1e6, E.gl_desc);
            printf("  per frame: tab row %.1f us, text rows (hash+lookup+push, no GL) %.1f us, clip flush (GL) %.1f us, editor area incl. rows %.1f us, status bar %.1f us\n",
                   E.t_top / bench * 1e6, E.t_rows / bench * 1e6, E.t_flush / bench * 1e6, E.t_gutter / bench * 1e6, E.t_status / bench * 1e6);
        }
        printf("%s\n", cache_stats_text().c_str());
        if (g_job.th.joinable()) g_job.th.join();   /* a joinable thread at exit would abort and lose the numbers */
        if (g_rjob.th.joinable()) g_rjob.th.join();
        askpass_stop();
        return 0;
    }
    SDL_Event ev;
    while (E.running) {
        double due = E.next_hud ? E.next_hud : E.now;
        if (g_job.running || g_rjob.running) due = std::min(due, E.now + 0.2);
        if (E.focused && E.now - E.last_active < BLINK_FOR) due = std::min(due, E.last_blink + 0.53);
        if (E.side_close_at > 0) due = std::min(due, E.side_close_at);
        if (E.tip_at > 0) due = std::min(due, E.tip_at);
        if (E.follow_fleet && !E.fleet_watch) due = std::min(due, E.next_fleet ? E.next_fleet : E.now + 1.0);
        double left = due - E.now;
        int wait = left < 0.001 ? 1 : left > 5.0 ? 5000 : (int)(left * 1000.0) + 1;
        if (E.dirty) wait = 0;
        if (SDL_WaitEventTimeout(&ev, wait)) { do handle_event(ev); while (SDL_PollEvent(&ev)); }
        E.now = now_s();
        if (g_print_stats) { g_print_stats = 0; fprintf(stderr, "%s\n", cache_stats_text().c_str()); }
        g_wake_pending = 0;
        search_poll();
        git_job_poll();
        job_poll(g_rjob);
        askpass_poll();
        tip_poll();
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
    if (g_job.th.joinable()) g_job.th.join();
    if (g_rjob.th.joinable()) g_rjob.th.join();
    askpass_stop();
    return 0;
}
