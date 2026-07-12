/*
 * edit.c - VI command processing for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Implements normal mode, insert mode, and command-line mode.
 * Operator-motion model: d/c/y + motion applies to a range.
 *
 * Division by 2 for half-page scrolling is replaced with right-shift (>> 1)
 * so the Z80 software-division library routines are not linked.
 * No standard library headers are included.
 */

#include "hvi.h"

extern Editor ed;

/* ------------------------------------------------------------------ */
/*  Static state                                                        */
/* ------------------------------------------------------------------ */
static int g_op;           /* pending operator: 'd','c','y', 0=none */
static int g_count;        /* digit-prefix accumulator              */
static int g_hcnt;         /* non-zero when a count digit was seen  */
static int g_g;            /* 'g' prefix pending                    */
static int g_g_count;      /* count saved when 'g' prefix was typed */
static int g_find_char;    /* last char target for f/F             */
static int g_find_dir;     /* 1 = forward (f), -1 = backward (F)  */
static int g_ins_cmd;      /* command that entered insert mode     */
static int g_ins_multi;    /* insert session touched a wrapped line:
                              the incremental updates may have left a
                              stale row, so ESC must redraw the line  */

static void normal_cmd();
/* Return effective count (at least 1), then clear. */
static int get_count()
{
    static int n;
    n = (g_hcnt) ? g_count : 1;
    g_count = 0; g_hcnt = 0;
    return n;
}

/* Alias commands (x, X, s, S, D, C, Y) expand to operator + motion. */
static void op_motion(op, count, motion)
int op, count, motion;
{
    g_op = op; g_count = count; g_hcnt = 1;
    normal_cmd(motion);
}



/* Functions defined in emove.c */
int  iswordch();
int  isspacech();
void undo_save_delete();
void undo_save_insert();
void mv_bol();
void mv_bnb();
void mv_eol();
void mv_left();
void mv_right();
void mv_up();
void mv_down();
void mv_word();
void mv_find();
int  motion_endpoint();
extern int me_cw;
extern int me_mkc;
void apply_op();
int  read_line();
int  read_pattern();
int  do_search_from();
int  do_search_full();
void status_show();
void status_msg();
void set_wcol();
int  line_span();
extern int ls_from;

/* Defined in erepeat.c */
void cur_back_nl();

/* Scroll the viewport to the cursor, then repaint only the rows that
 * came into view (old_top = ed.top_pos captured before the motion). */
static void move_upd(old_top)
int old_top;
{
    scr_scroll_to_cursor();
    scr_update_after_move(old_top);
}

/* Record the dot-repeat state for a change command. */
static void set_dot(cmd, motion, cnt, arg)
int cmd, motion, cnt, arg;
{
    ed.dot_cmd    = cmd;
    ed.dot_motion = motion;
    ed.dot_count  = cnt;
    ed.dot_arg    = arg;
    ed.dot_len    = 0;
}

/* ------------------------------------------------------------------ */
/*  Insert mode                                                         */
/* ------------------------------------------------------------------ */

/*
 * Buffer full during insert: swap out via gb_make_room(), retry the
 * one-character insert, and repaint from scratch (the room-making
 * reloaded the window).  Shared by the newline and character paths.
 */
/* Insert session touched a multi-row line: make ESC redraw the line. */
static void chk_multi()
{
    if (!scr_line_is_1row(ed.cur_pos)) g_ins_multi = 1;
}

/* Shared status literals (HI-TECH C stores repeated literals per use;
 * s_full and s_nopat are also used by the :s substitute in ex.c). */
char s_full[]  = "Buffer full";
char s_nopat[] = "Pattern not found";

static void ins_full_retry(tmp)
char *tmp;
{
    if (!gb_make_room() || !gb_insert(ed.cur_pos, tmp, 1)) {
        status_msg(s_full);
        return;
    }
    ed.cur_pos++;
    ed.modified = 1;
    scr_scroll_to_cursor();
    scr_refresh();
    scr_show_status(msg_insert);
    g_ins_multi = 0;            /* screen is fresh */
}

/*
 * Non-zero when a tab lies between cur_pos and the end of the line.
 * The terminal's insert/delete-char escapes shift the on-screen tail
 * by exactly one column, but a tab's rendered width absorbs (or, at
 * a stop boundary, amplifies) the shift -- the fast paths below may
 * only be used on a tab-free tail.
 */
static int tht_p, tht_sz, tht_c;

static int tail_has_tab()
{
    tht_sz = gb_content_len();
    for (tht_p = ed.cur_pos; tht_p < tht_sz; tht_p++) {
        tht_c = gb_char_at(tht_p);
        if (tht_c == '\n') break;
        if (tht_c == '\t') return 1;
    }
    return 0;
}

/*
 * Handle one insert-mode keypress.
 * Returns 0 to stay in insert mode, 1 to return to normal mode.
 */
static int insert_key(c0)
int c0;
{
    static int c;       /* param copy: absolute beats IX (never nests) */
    static char tmp[2];
    static int del_ch, cur_col, new_col, sz;
    static int old_top, i, ilen;
    static int start, del, had_nl, sol;

    c = c0;
    if (c == KEY_ESC) {
        ed.mode = MODE_NORMAL;
        if (g_ins_cmd && ed.undo.type == UNDO_INSERT && ed.undo.len != 0) {
            ilen = ed.undo.len;
            if (ilen > DOT_TEXT_MAX) ilen = DOT_TEXT_MAX;
            gb_copy_out(ed.dot_text, ed.undo.pos, ilen);
            ed.dot_len   = ilen;
            ed.dot_cmd   = g_ins_cmd;
            ed.dot_count = 1;
            ed.dot_arg   = 0;
            if (g_ins_cmd != 'c') ed.dot_motion = 0;
        }
        g_ins_cmd = 0;
        cur_back_nl();
        set_wcol();
        ed.status[0] = '\0';
        old_top = ed.top_pos;
        scr_scroll_to_cursor();
        if (ed.top_pos != old_top) {
            scr_refresh();
        } else if (!g_ins_multi && scr_line_is_1row(ed.cur_pos)) {
            /* Insert mode kept the row current -- status update only. */
            status_show();
        } else {
            ed.cur_vrow = -1;   /* row cache unreliable after wrap ops */
            scr_redraw_cur_line();
            status_show();
        }
        g_ins_multi = 0;
        return 1;
    }

    if (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_UP || c == KEY_DOWN) {
        if (c == KEY_LEFT)  mv_left(1);
        if (c == KEY_RIGHT) mv_right(1);
        if (c == KEY_UP)    mv_up(1);
        if (c == KEY_DOWN)  mv_down(1);
        scr_scroll_to_cursor();
        scr_update_cursor();
        return 0;
    }

    if (c == KEY_BS || c == KEY_DEL || c == KEY_CTRL_H) {
        if (ed.cur_pos != 0) {  /* cur_pos >= 0: cheap equality */
            chk_multi();
            del_ch = gb_char_at(ed.cur_pos - 1);
            ed.cur_pos--;
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
            if (ed.undo.type == UNDO_INSERT && ed.undo.len != 0)
                ed.undo.len--;
            if (del_ch == '\n') {
                scr_adj();
                scr_show_status(msg_insert);
            } else if (del_ch == '\t' || tail_has_tab()) {
                scr_redraw_cur_line();
            } else {
                sz = gb_content_len();
                term_putch(KEY_BS);
                if (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n')
                    term_clreol();
                else
                    term_del_char();
            }
        }
        return 0;
    }

    if (c == KEY_CTRL_W) {
        chk_multi();
        start = ed.cur_pos;
        while (ed.cur_pos != 0 && isspacech(gb_char_at(ed.cur_pos - 1)))
            ed.cur_pos--;
        while (ed.cur_pos != 0 && !isspacech(gb_char_at(ed.cur_pos - 1)))
            ed.cur_pos--;
        del = start - ed.cur_pos;
        if (del > 0) {
            had_nl = 0;
            for (i = ed.cur_pos; i < start; i++)
                if (gb_char_at(i) == '\n') { had_nl = 1; break; }
            gb_delete(ed.cur_pos, del);
            ed.modified = 1;
            if (ed.undo.type == UNDO_INSERT) {
                ed.undo.len -= del;
                if (ed.undo.len < 0) ed.undo.len = 0;
            }
            if (had_nl) scr_adj();
            else        scr_redraw_cur_line();
        }
        scr_show_status(msg_insert);
        return 0;
    }

    if (c == KEY_CTRL_U) {
        chk_multi();
        sol = find_bol(ed.cur_pos);
        if (sol < ed.cur_pos) {
            del = ed.cur_pos - sol;
            gb_delete(sol, del);
            ed.cur_pos = sol;
            ed.modified = 1;
            if (ed.undo.type == UNDO_INSERT) {
                ed.undo.len -= del;
                if (ed.undo.len < 0) ed.undo.len = 0;
            }
        }
        scr_redraw_cur_line();
        return 0;
    }

    if (c == KEY_CR) c = '\n';

    if (c == '\n') {
        tmp[0] = '\n';
        if (!gb_insert(ed.cur_pos, tmp, 1)) {
            ins_full_retry(tmp);
            return 0;
        }
        ed.cur_pos++;
        ed.modified = 1;
        if (ed.undo.type == UNDO_INSERT) ed.undo.len++;
        scr_adj();
        /* Mid-line split: the row above the cursor kept the tail that
         * moved down with the new line -- repaint it (tail exists only
         * when the cursor's new line is non-empty). */
        if (ed.cur_vrow > 0 && ed.cur_pos < gb_content_len() &&
            gb_char_at(ed.cur_pos) != '\n')
            scr_redraw_line(ed.cur_vrow - 1);
        scr_show_status(msg_insert);
        g_ins_multi = 0;        /* cursor is on a freshly drawn line */
        return 0;
    }

    cur_col = scr_vrow_col(ed.cur_pos);
    new_col = (c == '\t') ? (cur_col | (TAB_STOP - 1)) + 1 : cur_col + 1;

    tmp[0] = (char)c;
    if (!gb_insert(ed.cur_pos, tmp, 1)) {
        ins_full_retry(tmp);
        return 0;
    }
    ed.cur_pos++;
    ed.modified = 1;
    if (ed.undo.type == UNDO_INSERT) ed.undo.len++;

    if (new_col > ed.scr_cols) {
        g_ins_multi = 1;        /* line wraps now */
        ed.cur_vrow = -1;       /* cursor crossed onto the next vrow */
        scr_redraw_cur_line();
    } else {
        sz = gb_content_len();
        /* If appending at the end of the line, emit the byte directly to save output.
           Otherwise, shift the trailing text right. */
        if (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n') {
            if (c == '\t') {
                while (cur_col < new_col) { term_putch(' '); cur_col++; }
            } else {
                term_putch(c);
            }
        } else {
            if (c == '\t' || tail_has_tab()) {
                scr_redraw_cur_line();
            } else {
                term_ins_char();
                term_putch(c);
            }
            /* Mid-line insert into a line that (now) wraps can push a
             * character off the row end; make ESC repaint the line. */
            chk_multi();
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Command-line (ex) mode                                              */
/* ------------------------------------------------------------------ */

static void cmdline_mode()
{
    /* read_line (emove.c) shares the prompt/edit loop with / and ?. */
    ed.cmdlen = read_line(':', ed.cmdline, CMD_MAX, 0);
    if (ed.cmdlen <= 0) {           /* ESC (-1) or empty command */
        scr_clear_status();
        return;
    }
    if (ex_execute(ed.cmdline)) {
        if (!ed.quit) scr_refresh();
    } else {
        if (!ed.quit) status_show();
    }
}

/* ------------------------------------------------------------------ */
/*  Normal mode dispatcher                                              */
/* ------------------------------------------------------------------ */

/* Shared tail for f/F/;/, -- move to the char and update the screen. */
static void find_move(dir, count)
int dir, count;
{
    mv_find(g_find_char, dir, count);   /* does not move top_pos */
    move_upd(ed.top_pos);
}

/* Character search on current line: f, F, ;, ,, . */
static void normal_find_cmd(c, count)
int c, count;
{
    static int ch;
    switch (c) {
    case 'f':
    case 'F':
        ch = term_getch();
        if (ch == KEY_ESC) break;
        g_find_char = ch;
        g_find_dir  = (c == 'f') ? 1 : -1;
        find_move(g_find_dir, count);
        break;

    case ';':
        find_move(g_find_dir, count);
        break;

    case ',':
        find_move(-g_find_dir, count);
        break;

    default:
        break;
    }
}

/* Adjust cursor to the insertion point for the original entry command. */
void ins_position();
void dot_replay_c();
void dot_replay();

static int  sm_pos;
static long sm_win;   /* win_start before search -- detects buffer reload */

/* HI-TECH C stores repeated literals once per use -- share these. */
static char s_top[] = "TOP", s_bot[] = "BOTTOM";

/* Shared search-result handler: move to match or report not found. */
static void do_search_move()
{
    sm_win = ed.win_start;
    sm_pos = do_search_full(ed.cur_pos);
    if (sm_pos >= 0) {
        ed.marks[MARK_PREV] = ed.cur_pos;   /* `` returns here */
        ed.cur_pos  = sm_pos;
        ed.cur_vrow = -1;
        /* A buffer reload changes win_start; need a full screen redraw
         * (scr_refresh scrolls to the cursor itself). */
        if (ed.win_start != sm_win) {
            scr_refresh();
        } else {
            move_upd(ed.top_pos);
        }
        if (ed.search_wrapped) {
            hvi_sprintf(ed.status, "search hit %s, continuing at %s",
                (int)(ed.search_dir == SEARCH_FWD ? s_bot : s_top),
                (int)(ed.search_dir == SEARCH_FWD ? s_top : s_bot));
            status_show();
        }
    } else {
        status_msg(s_nopat);
    }
}

/* CPIR newline counter over raw memory (cstart.as). */
extern int gb_cntnl();

/* Non-zero when the charwise yank buffer contains a newline. */
static int yank_has_nl()
{
    return gb_cntnl(ed.yank, ed.yank_len) != 0;
}

/*
 * Shared put: after != 0 is 'p' (below the line / after the cursor),
 * 0 is 'P' (above the line / at the cursor).
 * The undo record covers exactly the bytes inserted, including any
 * newline added around the yank (py_start / py_total).
 *
 * Inserts go through gb_insert_room: with a window-slid large file the
 * gap can be smaller than the yank (as little as GAP_MIN bytes), and a
 * bare gb_insert would silently drop the put.  When room-making swapped
 * the window (gb_roomed), the reload invalidated marks and the undo
 * record, so the undo save is skipped and the screen fully repainted.
 */
extern char  gb_roomed;
extern int   gir_pos, gir_len;
extern char *gir_text;
static int   py_pos, py_light, py_total, py_end, py_sz;
static char  py_nl[1] = { '\n' };  /* 1 data byte beats a store per call */

/* Room-making failed (disk full/IO error) mid-insert: part of the text
 * may already be in the buffer -- repaint and report.  Shared by put
 * and the dot-replay insert (erepeat.c). */
void full_fail()
{
    ed.modified = 1;
    scr_refresh();
    status_msg(s_full);
}

/* Insert one byte (gir_text[0], at gir_pos -- both preset by the
 * caller) making room when the gap is exhausted: a window packed to a
 * zero-byte gap otherwise drops it silently.  Does not clear
 * gb_roomed, so a command's calls accumulate the flag.  Returns the
 * position just past the byte (== the updated gir_pos, so consecutive
 * calls chain), or -1 on failure (reported; this call inserted
 * nothing).  Shared by the newline inserts here (put, o/O) and the
 * shift operators' tab insert (apply_shift, emove.c). */
int room1()
{
    gir_len = 1;
    if (gb_insert_room() < 0) {
        status_msg(s_full);
        return -1;
    }
    return gir_pos;
}

static void put_yank(after)
int after;
{
    if (ed.yank_len == 0) return;   /* never negative: cheap equality */
    py_light = 0;
    py_total = ed.yank_len;
    py_sz    = gb_content_len();   /* all uses are pre-insert */
    gb_roomed = 0;
    /* gb_insert_room chains through gir_pos and passes a failure (-1)
     * through, so one py_end check at the bottom covers every insert. */
    if (ed.yank_line) {
        if (after) {
            gir_pos = find_eol(ed.cur_pos);
            if (gir_pos < py_sz) {
                gir_pos++;          /* past the newline */
            } else if (gir_pos > 0 && gb_char_at(gir_pos - 1) != '\n') {
                /* Pasting below a last line that lacks its newline:
                 * add one first so the yank starts on a fresh line
                 * instead of being glued onto this one.  (Only reachable
                 * when find_eol hit buffer end: after the ++ above the
                 * previous char is always '\n'.) */
                gir_text = py_nl;
                if (room1() < 0) return;
                py_total++;
            }
        } else {
            gir_pos = find_bol(ed.cur_pos);
        }
    } else {
        py_light = scr_line_is_1row(ed.cur_pos) && !yank_has_nl();
        gir_pos = ed.cur_pos;
        if (after && gir_pos < py_sz && gb_char_at(gir_pos) != '\n')
            gir_pos++;
    }
    gir_text = ed.yank; gir_len = ed.yank_len;
    py_end = gb_insert_room();
    py_pos = py_end - ed.yank_len;   /* window may have shifted */
    if (ed.yank_line && ed.yank[ed.yank_len - 1] != '\n') {
        gir_text = py_nl;
        py_end = room1();
        py_total++;
    }
    if (py_end < 0) { full_fail(); return; }
    ed.modified = 1;
    ed.cur_pos = py_pos;
    if (gb_roomed) {
        scr_refresh();      /* window moved: repaint from scratch */
    } else {
        /* The undo record covers exactly the py_total inserted bytes,
         * which end at py_end -- no need to have tracked their start. */
        undo_save_insert(py_end - py_total, py_total);
        scr_edit_end(py_light);
    }
}

/* Handle yank/put/search/undo/ex commands. */
static void normal_misc_cmd(c, count, size)
int c, count, size;
{
    static int save_len, old_dir, light;
    static int mk_c, mk_pos;
    switch (c) {

    /* --- Marks --- */
    case 'm':
        mk_c = term_getch();
        if (mk_c >= 'a' && mk_c <= 'z')
            ed.marks[mk_c - '`'] = ed.cur_pos;  /* slot = char - 0x60 */
        break;

    case '`':
        /* motion_endpoint('`') resolves and validates the mark -- the
         * same code the d/c/y operators use (mk_c is a dummy linewise
         * out-param). */
        me_mkc = term_getch();
        mk_pos = motion_endpoint('`', 1, &mk_c);
        if (mk_pos < 0) break;
        ed.marks[MARK_PREV] = ed.cur_pos;   /* `` returns here */
        ed.cur_pos  = mk_pos;
        ed.cur_vrow = -1;
        set_wcol();
        move_upd(ed.top_pos);
        break;

    /* --- Yank/Put --- */
    case 'Y':
        op_motion('y', count, 'y');
        break;

    case 'p':  put_yank(1); break;
    case 'P':  put_yank(0); break;

    /* --- Search --- */
    case '/':
        ed.search_dir = SEARCH_FWD;
        if (read_pattern('/')) do_search_move();
        else scr_clear_status();
        break;

    case '?':
        ed.search_dir = SEARCH_BWD;
        if (read_pattern('?')) do_search_move();
        else scr_clear_status();
        break;

    case 'n':
        do_search_move();
        break;

    case 'N':
        old_dir = ed.search_dir;
        ed.search_dir = -old_dir;
        do_search_move();
        ed.search_dir = old_dir;
        break;

    /* --- Undo --- */
    case 'u':
        if (ed.undo.type == UNDO_DELETE) {
            save_len = ed.undo.len;
            if (save_len > UNDO_MAX) save_len = UNDO_MAX;
            light = scr_line_is_1row(ed.undo.pos) &&
                    gb_cntnl(ed.undo.text, save_len) == 0;
            gb_insert(ed.undo.pos, ed.undo.text, save_len);
            ed.cur_pos   = ed.undo.pos;
            ed.modified  = ed.undo.was_clean ? 0 : 1;
            ed.undo.type = UNDO_NONE;
            scr_edit_end(light);
        } else if (ed.undo.type == UNDO_INSERT) {
            light = scr_line_is_1row(ed.undo.pos) &&
                    gb_count_nl(ed.undo.pos, ed.undo.len) == 0;
            gb_delete(ed.undo.pos, ed.undo.len);
            ed.cur_pos   = ed.undo.pos;
            ed.modified  = ed.undo.was_clean ? 0 : 1;
            ed.undo.type = UNDO_NONE;
            scr_edit_end(light);
        } else {
            status_msg("Nothing to undo");
        }
        break;

    /* --- Screen control --- */
    case KEY_CTRL_L:
        term_clear();
        scr_status_invalidate();    /* status row was just erased */
        /* Re-establish scroll region after term_clear() homes the cursor. */
        term_scroll_region();
        scr_refresh();
        break;

    /* --- Ex command line --- */
    case ':':   cmdline_mode(); break;

    /* --- Join lines / toggle case ---
     * Same implementation as their '.' replay: record the dot state and
     * run the shared handler in erepeat.c (one copy of the logic). */
    case 'J':
    case '~':
        set_dot(c, 0, count, 0);
        dot_replay(count);
        break;

    /* Ignore unknown commands silently */
    default:
        normal_find_cmd(c, count);
        break;
    }
}

/* Handle delete/change/replace commands. */
static void normal_delchg_cmd(c, count, size)
int c, count, size;
{
    switch (c) {

    /* --- Operators (>/< shift one TAB_STOP, always linewise) --- */
    case 'd':
    case 'c':
    case 'y':
    case '>':
    case '<':
        g_op = c; g_count = count; g_hcnt = 1; return;

    case 'D':  op_motion('d', count, '$'); break;
    case 'C':  op_motion('c', count, '$'); break;
    case 'x':  op_motion('d', count, 'l'); break;
    case 'X':  op_motion('d', count, 'h'); break;

    case 'r':
        /* Same implementation as its '.' replay (one copy of the logic,
         * like J and ~): record the dot state and run the replay. */
        {
            static int repl;
            repl = term_getch();
            if (repl != KEY_ESC && ed.cur_pos < size) {
                if (repl == KEY_CR) repl = '\n';
                set_dot('r', 0, 1, repl);
                dot_replay(1);
            }
        }
        break;

    default:
        normal_misc_cmd(c, count, size);
        break;
    }
}

/* Set up insert mode and show the mode indicator (shared tail of the
 * insert-entry commands). */
static void ins_start(c)
int c;
{
    g_ins_cmd = c;
    ed.mode = MODE_INSERT;
    scr_show_status(msg_insert);
}

/* Enter insert mode at the current position for entry command c.
 * Exported: apply_op (emove.c) shares it for the 'c' operator. */
void enter_insert(c)
int c;
{
    undo_save_insert(ed.cur_pos, 0);
    ins_start(c);
}

/* Open a new line and enter insert mode: 'O' above the cursor's line,
 * 'o' below it (first adding the last line's missing newline when the
 * buffer doesn't end in one). */
static void open_line(cmd)
int cmd;
{
    static int pos, sz;
    gb_roomed = 0;
    gir_text  = py_nl;          /* both room1 calls insert a newline */
    if (cmd == 'O') {
        mv_bol();
        gir_pos = ed.cur_pos;
    } else {
        gir_pos = find_eol(ed.cur_pos);
        sz = gb_content_len();
        if (gir_pos < sz) {
            gir_pos++;
        } else if (sz > 0 && gb_char_at(sz - 1) != '\n') {
            /* last line lacks its newline: add it first (room1
             * leaves gir_pos just past it -- the calls chain) */
            if (room1() < 0) return;
        }
    }
    pos = room1();
    if (pos < 0) return;        /* no room: don't enter insert mode */
    pos--;                      /* cursor onto the fresh line */
    ed.cur_pos = pos;
    if (gb_roomed) {
        /* Window swapped: the reload invalidated the undo record (as
         * after any room-making during insert) -- full repaint. */
        ed.modified = 1;
        scr_refresh();
    } else {
        undo_save_insert(pos, 1);   /* the newline; typing extends it */
        ed.modified = 1;
        scr_adj();
    }
    ins_start(cmd);
}

static void normal_edit_cmd(c, count, size)
int c, count, size;
{
    switch (c) {

    case 'i':
        enter_insert('i');
        break;

    /* a/I/A position via ins_position (erepeat.c) -- the same code
     * their '.' replay uses. */
    case 'a':
    case 'I':
    case 'A':
        ins_position(c);
        enter_insert(c);
        break;

    case 'o':
    case 'O':
        open_line(c);
        break;

    case 's':  op_motion('c', count, 'l'); break;
    case 'S':  op_motion('c', count, 'c'); break;

    default:
        normal_delchg_cmd(c, count, size);
        break;
    }
}

/*
 * Shared tail for line-jump commands (G, gg, page moves, window
 * reloads): normalise the cursor to first non-blank and repaint.
 * scr_refresh() runs scr_scroll_to_cursor() itself.
 */
static void nav_refresh()
{
    ed.cur_vrow = -1;
    mv_bnb();
    ed.want_col = 0;
    scr_refresh();
}

/* Set the new top line, place the cursor on the middle line of the new
 * page, and repaint. */
static void pg_mid(new_top, total)
int new_top, total;
{
    static int mid;
    ed.top_pos = scr_line_start(new_top);
    mid = new_top + ((ed.scr_rows - 2) >> 1);   /* >> 1 replaces / 2 */
    if (mid >= total) mid = total - 1;
    if (mid < 0) mid = 0;
    ed.cur_pos = scr_line_start(mid);
    nav_refresh();
}

/*
 * Handle G, Ctrl+F/B/D/U -- page and large-motion commands.
 * Half-page sizes use >> 1 instead of / 2 to avoid the division library.
 */
static void normal_page_cmd(c0, count0, had_count)
int c0, count0, had_count;
{
    static int c, count;    /* param copies (absolute beats IX) */
    static int n, top_line, total, text_rows, new_top;

    c = c0; count = count0;

    switch (c) {

    case 'G':
        ed.marks[MARK_PREV] = ed.cur_pos;   /* `` returns here */
        if (had_count) {
            /* nG: navigate to line n, scanning the file if needed. */
            gb_goto_line(count);
        } else if (ed.tail_offset > 0L && ed.tail_file[0]) {
            gb_load_last();   /* large file: jump to the last window */
        } else {
            ed.cur_pos = scr_last_line_start();
        }
        nav_refresh();
        set_wcol();
        break;

    case KEY_CTRL_F:
    case KEY_CTRL_B:
        text_rows = ed.scr_rows - 1;
        n        = count * (text_rows - 2);
        if (c == KEY_CTRL_F) {
            top_line = gb_count_nl(0, ed.top_pos);
            total    = scr_line_count();
            /* Load when new screen bottom would exceed buffer: need a full
             * screen of lines above new_top plus text_rows more for the new
             * viewport. */
            if (ed.tail_offset > 0L && top_line + n + text_rows > total) {
                gb_load_more(LOAD_CHUNK);
                /* Recompute: gb_discard_head in gb_load_more shifts top_pos. */
                top_line = gb_count_nl(0, ed.top_pos);
                total    = scr_line_count();
            }
            new_top = top_line + n;
            if (new_top >= total) new_top = total - 1;
            if (new_top <= top_line) break;
        } else if (ed.top_pos == 0 && ed.win_start > 0L) {
            gb_load_prev();
            total   = scr_line_count();
            new_top = total - 1 - n;
            if (new_top < 0) new_top = 0;
        } else {
            if (ed.top_pos == 0) break;
            new_top = gb_count_nl(0, ed.top_pos) - n;
            if (new_top < 0) new_top = 0;
            total = scr_line_count();
        }
        pg_mid(new_top, total);
        break;

    case KEY_CTRL_D:
    case KEY_CTRL_U:
        n = (ed.scr_rows - 1) >> 1;  /* >> 1 replaces / 2 */
        if (c == KEY_CTRL_D && ed.tail_offset > 0L &&
            ed.cur_pos >= gb_content_len() - n * ed.scr_cols)
            gb_load_more(LOAD_CHUNK);
        if (c == KEY_CTRL_D) mv_down(n * count);
        else                 mv_up(n * count);
        move_upd(ed.top_pos);        /* scroll + paint new rows only */
        break;
    }
}

static void normal_cmd(c0)
int c0;
{
    /* c copied to a static (absolute beats IX).  normal_cmd recurses
     * through op_motion, but the outer invocation never reads c after
     * its switch dispatch, so the clobber is harmless. */
    static int  c;
    static int  count, size, linewise, endpoint;
    static int  had_count, old_top;
    static int  op, nc_from, nc_to;
    static int  gg_line;

    c = c0;
    /* ---- digit prefix ---- */
    if (c >= '1' && c <= '9' && !g_op) {
        g_count = g_hcnt ? g_count * 10 + (c - '0') : (c - '0');
        g_hcnt  = 1;
        return;
    }
    if (c == '0' && g_hcnt && !g_op) {
        g_count = g_count * 10;
        return;
    }

    had_count = g_hcnt;
    count = get_count();
    size  = gb_content_len();

    /* ---- pending operator: expect motion ---- */
    if (g_op) {
        op = g_op;
        g_op = 0;

        /* doubled operator (dd, cc, yy) = operate on count lines.
         * line_span (emove.c) walks with find_bol/find_eol -- O(range). */
        if (c == op) {
            nc_to   = line_span(count);
            nc_from = ls_from;
            /* cc/S replace the line's text but keep its newline */
            if (op == 'c' && nc_to > nc_from && gb_char_at(nc_to - 1) == '\n')
                nc_to--;
            if (op != 'y')
                set_dot(op, op, count, 0);
            apply_op(op, nc_from, nc_to, 1);
            return;
        }

        /* motion character */
        linewise = 0;
        if (op == 'c' && c == 'w') me_cw = 1;
        me_mkc = (c == '`') ? term_getch() : 0;
        endpoint = motion_endpoint(c, count, &linewise);
        if (endpoint < 0)       /* motion_endpoint showed any message */
            return;
        if (op != 'y')
            set_dot(op, c, count, me_mkc);
        apply_op(op, ed.cur_pos, endpoint, linewise);
        return;
    }

    /* ---- 'g' prefix (gg) ---- */
    if (g_g) {
        g_g = 0;
        if (c == 'g') {
            gg_line = (g_g_count > 0) ? g_g_count : 1;
            ed.marks[MARK_PREV] = ed.cur_pos;   /* `` returns here */
            gb_goto_line(gg_line);
            nav_refresh();
        }
        return;
    }

    /* Translate ANSI arrow keys to their hjkl equivalents */
    if (c == KEY_UP)    c = 'k';
    if (c == KEY_DOWN)  c = 'j';
    if (c == KEY_LEFT)  c = 'h';
    if (c == KEY_RIGHT) c = 'l';

    /* ---- normal commands ---- */
    switch (c) {

    /* --- Movement --- */
    case 'h':  mv_left(count);  scr_update_cursor(); break;
    case 'l':  mv_right(count); scr_update_cursor(); break;

    case KEY_CR:
        mv_down(count); mv_bnb();       /* neither moves top_pos */
        move_upd(ed.top_pos);
        break;

    case 'j':
        if (ed.tail_offset > 0L &&
            ed.cur_pos >= gb_content_len() - ed.scr_cols) {
            gb_load_more(LOAD_CHUNK);
            mv_down(count); scr_scroll_to_cursor();
            scr_refresh();
        } else {
            mv_down(count);
            move_upd(ed.top_pos);
        }
        break;
    case 'k':
        old_top = ed.top_pos;
        mv_up(count); scr_scroll_to_cursor();
        if (ed.cur_pos == 0 && ed.win_start > 0L) {
            gb_load_prev();
            ed.cur_pos = scr_last_line_start();
            nav_refresh();
            break;
        }
        scr_update_after_move(old_top);
        break;

    case 'w':
    case 'b':
    case 'e':
        mv_word(c, count);              /* does not move top_pos */
        move_upd(ed.top_pos);
        break;

    case '0':  mv_bol();  ed.want_col = 0; scr_update_cursor(); break;
    case '^':  mv_bnb();  set_wcol(); scr_update_cursor(); break;
    case '$':  mv_eol();  ed.want_col = 9999; scr_update_cursor(); break;

    case '.':
        dot_replay(had_count ? count : 0);
        break;

    case 'g':  g_g = 1; g_g_count = had_count ? count : 0; return;

    case 'G':
    case KEY_CTRL_F:
    case KEY_CTRL_B:
    case KEY_CTRL_D:
    case KEY_CTRL_U:
        normal_page_cmd(c, count, had_count);
        break;

    default:
        normal_edit_cmd(c, count, size);
        break;
    }

    /* Clear g-prefix if not used */
    g_g = 0;
}

/* ------------------------------------------------------------------ */
/*  Main edit loop                                                      */
/* ------------------------------------------------------------------ */

static int s_er_c;  /* static: bios in term_getch may corrupt IX-relative locals */

void edit_run()
{
    ed.mode    = MODE_NORMAL;
    ed.quit    = 0;
    ed.want_col = 0;

    scr_refresh();

    while (!ed.quit) {
        s_er_c = term_getch();

        if (ed.mode == MODE_INSERT) {
            if (ed.status[0]) {
                ed.status[0] = '\0';
                scr_show_status(msg_insert);
            }
            insert_key(s_er_c);
            if (ed.mode == MODE_INSERT && ed.status[0])
                status_show();
        } else {
            if (ed.status[0]) {
                ed.status[0] = '\0';
                scr_clear_status();
            }
            normal_cmd(s_er_c);
        }
    }
}
