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
void apply_op();
int  read_pattern();
int  do_search_from();
int  do_search_full();

/* ------------------------------------------------------------------ */
/*  Insert mode                                                         */
/* ------------------------------------------------------------------ */

/*
 * Buffer full during insert: swap out via gb_make_room(), retry the
 * one-character insert, and repaint from scratch (the room-making
 * reloaded the window).  Shared by the newline and character paths.
 */
static void ins_full_retry(tmp)
char *tmp;
{
    if (!gb_make_room() || !gb_insert(ed.cur_pos, tmp, 1)) {
        hvi_strcpy(ed.status, "Buffer full");
        scr_show_status(ed.status);
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
 * Handle one insert-mode keypress.
 * Returns 0 to stay in insert mode, 1 to return to normal mode.
 */
static int insert_key(c)
int c;
{
    static char tmp[2];
    static int del_ch, cur_col, new_col, sz;
    static int old_top, i, ilen, prev;
    static int start, del, had_nl, sol;

    if (c == KEY_ESC) {
        ed.mode = MODE_NORMAL;
        if (g_ins_cmd && ed.undo.type == UNDO_INSERT && ed.undo.len > 0) {
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
        if (ed.cur_pos > 0) {
            prev = gb_char_at(ed.cur_pos - 1);
            if (prev != '\n') ed.cur_pos--;
        }
        ed.want_col = scr_pos_col(ed.cur_pos);
        ed.status[0] = '\0';
        old_top = ed.top_pos;
        scr_scroll_to_cursor();
        if (ed.top_pos != old_top) {
            scr_refresh();
        } else if (!g_ins_multi && scr_line_is_1row(ed.cur_pos)) {
            /* Insert mode kept the row current -- status update only. */
            scr_show_status(ed.status);
        } else {
            ed.cur_vrow = -1;   /* row cache unreliable after wrap ops */
            scr_redraw_cur_line();
            scr_show_status(ed.status);
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
        if (ed.cur_pos > 0) {
            if (!scr_line_is_1row(ed.cur_pos)) g_ins_multi = 1;
            del_ch = gb_char_at(ed.cur_pos - 1);
            ed.cur_pos--;
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
            if (ed.undo.type == UNDO_INSERT && ed.undo.len > 0)
                ed.undo.len--;
            if (del_ch == '\n') {
                scr_adj();
                scr_show_status(msg_insert);
            } else if (del_ch == '\t') {
                scr_redraw_cur_line();
            } else {
                sz = gb_content_len();
                if (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n') {
                    term_putch(KEY_BS);
                    term_clreol();
                } else {
                    term_putch(KEY_BS);
                    term_del_char();
                }
            }
        }
        return 0;
    }

    if (c == KEY_CTRL_W) {
        if (!scr_line_is_1row(ed.cur_pos)) g_ins_multi = 1;
        start = ed.cur_pos;
        while (ed.cur_pos > 0 && isspacech(gb_char_at(ed.cur_pos - 1)))
            ed.cur_pos--;
        while (ed.cur_pos > 0 && !isspacech(gb_char_at(ed.cur_pos - 1)))
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
        if (!scr_line_is_1row(ed.cur_pos)) g_ins_multi = 1;
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
            if (c == '\t') {
                scr_redraw_cur_line();
            } else {
                term_ins_char();
                term_putch(c);
            }
            /* Mid-line insert into a line that (now) wraps can push a
             * character off the row end; make ESC repaint the line. */
            if (!scr_line_is_1row(ed.cur_pos)) g_ins_multi = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Command-line (ex) mode                                              */
/* ------------------------------------------------------------------ */

static int s_clm_c;  /* static: bios in term_getch may corrupt IX-relative locals */

static void cmdline_mode()
{
    ed.cmdline[0] = '\0';
    ed.cmdlen = 0;
    scr_status_invalidate();
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    term_putch(':');

    for (;;) {
        s_clm_c = term_getch();
        if (s_clm_c == KEY_ESC) {
            scr_clear_status();
            return;
        }
        if (s_clm_c == KEY_CR || s_clm_c == KEY_LF) {
            ed.cmdline[ed.cmdlen] = '\0';
            if (ed.cmdlen > 0) {
                if (ex_execute(ed.cmdline)) {
                    if (!ed.quit) scr_refresh();
                } else {
                    if (!ed.quit) scr_show_status(ed.status);
                }
            } else {
                if (!ed.quit) scr_clear_status();
            }
            return;
        }
        if ((s_clm_c == KEY_BS || s_clm_c == KEY_DEL) && ed.cmdlen > 0) {
            ed.cmdlen--;
            term_putch(KEY_BS); term_putch(' '); term_putch(KEY_BS);
            continue;
        }
        if (s_clm_c >= 0x20 && s_clm_c < 0x7F && ed.cmdlen < CMD_MAX - 1) {
            ed.cmdline[ed.cmdlen++] = (char)s_clm_c;
            term_putch(s_clm_c);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Normal mode dispatcher                                              */
/* ------------------------------------------------------------------ */

/* Shared tail for f/F/;/, -- move to the char and update the screen. */
static void find_move(dir, count)
int dir, count;
{
    static int fm_top;
    fm_top = ed.top_pos;
    mv_find(g_find_char, dir, count);
    scr_scroll_to_cursor();
    scr_update_after_move(fm_top);
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
void dot_ins_position();
void dot_replay_c();
void dot_replay();

static int  sm_pos;
static int  sm_top;
static long sm_win;   /* win_start before search -- detects buffer reload */

/* Shared search-result handler: move to match or report not found. */
static void do_search_move()
{
    sm_win = ed.win_start;
    sm_pos = do_search_full(ed.cur_pos);
    if (sm_pos >= 0) {
        ed.cur_pos  = sm_pos;
        ed.cur_vrow = -1;
        sm_top = ed.top_pos;
        scr_scroll_to_cursor();
        /* A buffer reload changes win_start; need a full screen redraw. */
        if (ed.win_start != sm_win) {
            scr_refresh();
        } else {
            scr_update_after_move(sm_top);
        }
        if (ed.search_wrapped) {
            hvi_sprintf(ed.status, "search hit %s, continuing at %s",
                (int)(ed.search_dir == SEARCH_FWD ? "BOTTOM" : "TOP"),
                (int)(ed.search_dir == SEARCH_FWD ? "TOP" : "BOTTOM"));
            scr_show_status(ed.status);
        }
    } else {
        hvi_strcpy(ed.status, "Pattern not found");
        scr_show_status(ed.status);
    }
}

/* Non-zero when the charwise yank buffer contains a newline. */
static int yhn_i;

static int yank_has_nl()
{
    for (yhn_i = 0; yhn_i < ed.yank_len; yhn_i++)
        if (ed.yank[yhn_i] == '\n') return 1;
    return 0;
}

/*
 * Shared put: after != 0 is 'p' (below the line / after the cursor),
 * 0 is 'P' (above the line / at the cursor).
 * The undo record covers exactly the bytes inserted, including any
 * newline added around the yank (py_start / py_total).
 */
static int  py_pos, py_light, py_start, py_total;
static char py_nl[1];

static void put_yank(after)
int after;
{
    if (ed.yank_len <= 0) return;
    py_light = 0;
    py_nl[0] = '\n';
    py_total = ed.yank_len;
    if (ed.yank_line) {
        if (after) {
            py_pos = find_eol(ed.cur_pos);
            if (py_pos < gb_content_len()) py_pos++;
            py_start = py_pos;
            if (py_pos >= gb_content_len() && py_pos > 0 &&
                gb_char_at(py_pos - 1) != '\n') {
                /* Pasting below a last line that lacks its newline:
                 * add one first so the yank starts on a fresh line
                 * instead of being glued onto this one. */
                gb_insert(py_pos, py_nl, 1);
                py_pos++;
                py_total++;
            }
        } else {
            py_pos = find_bol(ed.cur_pos);
            py_start = py_pos;
        }
        gb_insert(py_pos, ed.yank, ed.yank_len);
        if (ed.yank[ed.yank_len - 1] != '\n') {
            gb_insert(py_pos + ed.yank_len, py_nl, 1);
            py_total++;
        }
    } else {
        py_light = scr_line_is_1row(ed.cur_pos) && !yank_has_nl();
        py_pos = ed.cur_pos;
        if (after && py_pos < gb_content_len() && gb_char_at(py_pos) != '\n')
            py_pos++;
        py_start = py_pos;
        gb_insert(py_pos, ed.yank, ed.yank_len);
    }
    undo_save_insert(py_start, py_total);
    ed.cur_pos = py_pos;
    ed.modified = 1;
    scr_edit_end(py_light);
}

/* Handle yank/put/search/undo/ex commands. */
static void normal_misc_cmd(c, count, size)
int c, count, size;
{
    static int save_len, old_dir, light, u_i;
    switch (c) {

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
            light = scr_line_is_1row(ed.undo.pos);
            for (u_i = 0; u_i < save_len && light; u_i++)
                if (ed.undo.text[u_i] == '\n') light = 0;
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
            hvi_strcpy(ed.status, "Nothing to undo");
            scr_show_status(ed.status);
        }
        break;

    /* --- Screen control --- */
    case KEY_CTRL_L:
        {
            char buf[16];
            term_clear();
            scr_status_invalidate();    /* status row was just erased */
            /* Re-establish scroll region after term_clear() homes the cursor. */
            hvi_sprintf(buf, "\033[1;%dr", ed.scr_rows - 1, 0);
            term_puts(buf);
            scr_refresh();
        }
        break;

    /* --- Ex command line --- */
    case ':':   cmdline_mode(); break;

    /* --- Join lines / toggle case ---
     * Same implementation as their '.' replay: record the dot state and
     * run the shared handler in erepeat.c (one copy of the logic). */
    case 'J':
    case '~':
        ed.dot_cmd = c; ed.dot_count = count;
        ed.dot_motion = 0; ed.dot_arg = 0;
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

    /* --- Operators --- */
    case 'd':  g_op = 'd'; g_count = count; g_hcnt = 1; return;
    case 'c':  g_op = 'c'; g_count = count; g_hcnt = 1; return;
    case 'y':  g_op = 'y'; g_count = count; g_hcnt = 1; return;

    case 'D':  op_motion('d', count, '$'); break;
    case 'C':  op_motion('c', count, '$'); break;
    case 'x':  op_motion('d', count, 'l'); break;
    case 'X':  op_motion('d', count, 'h'); break;

    case 'r':
        {
            int repl = term_getch();
            int oldc;
            if (repl != KEY_ESC && ed.cur_pos < size) {
                char tmp_c[1];
                if (repl == KEY_CR) repl = '\n';
                oldc = gb_char_at(ed.cur_pos);
                undo_save_delete(ed.cur_pos, 1);
                gb_delete(ed.cur_pos, 1);
                tmp_c[0] = (char)repl;
                gb_insert(ed.cur_pos, tmp_c, 1);
                ed.modified = 1;
                ed.dot_cmd = 'r'; ed.dot_count = 1;
                ed.dot_motion = 0; ed.dot_arg = repl;
                if (repl == '\n') scr_refresh();
                else scr_fix_char(oldc, repl);
            }
        }
        break;

    default:
        normal_misc_cmd(c, count, size);
        break;
    }
}

/* Enter insert mode at the current position for entry command c. */
static void enter_insert(c)
int c;
{
    g_ins_cmd = c;
    undo_save_insert(ed.cur_pos, 0);
    ed.mode = MODE_INSERT;
    scr_show_status(msg_insert);
}

static void normal_edit_cmd(c, count, size)
int c, count, size;
{
    switch (c) {

    case 'i':
        enter_insert('i');
        break;

    case 'a':
        if (size > 0 && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        enter_insert('a');
        break;

    case 'I':
        mv_bnb();
        enter_insert('I');
        break;

    case 'A':
        mv_eol();
        if (size > 0 && ed.cur_pos < size && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        enter_insert('A');
        break;

    case 'o':
        g_ins_cmd = 'o';
        {
            char nl = '\n';
            int  eol = ed.cur_pos;
            int  sz  = gb_content_len();
            while (eol < sz && gb_char_at(eol) != '\n') eol++;
            if (eol < sz) eol++;
            if (eol >= sz && sz > 0 && gb_char_at(sz - 1) != '\n') {
                gb_insert(sz, &nl, 1);
                eol = sz + 1;
            }
            undo_save_insert(eol, 0);
            gb_insert(eol, &nl, 1);
            ed.cur_pos = eol;
            ed.modified = 1;
            ed.undo.len++;
            scr_adj();
            ed.mode = MODE_INSERT;
            scr_show_status(msg_insert);
        }
        break;

    case 'O':
        g_ins_cmd = 'O';
        {
            char nl = '\n';
            mv_bol();
            undo_save_insert(ed.cur_pos, 0);
            gb_insert(ed.cur_pos, &nl, 1);
            ed.modified = 1;
            ed.undo.len++;
            scr_adj();
            ed.mode = MODE_INSERT;
            scr_show_status(msg_insert);
        }
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

/* Place the cursor on the middle line of the new page and repaint. */
static void pg_mid(new_top, total)
int new_top, total;
{
    static int mid;
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
static void normal_page_cmd(c, count, had_count)
int c, count, had_count;
{
    static int n, top_line, total, text_rows, new_top, pg_top;
    static long new_off;

    switch (c) {

    case 'G':
        if (had_count) {
            /* nG: navigate to line n, scanning the file if needed. */
            gb_goto_line(count);
        } else {
            if (ed.tail_offset > 0L && ed.tail_file[0]) {
                /* Large file: jump directly to the last window. */
                new_off = hvi_fsize(ed.tail_file);
                if (new_off > (long)LOAD_CHUNK)
                    new_off -= (long)LOAD_CHUNK;
                else
                    new_off = 0L;
                gb_reload_from(new_off);
            }
            ed.cur_pos = scr_last_line_start();
        }
        nav_refresh();
        ed.want_col = scr_pos_col(ed.cur_pos);
        break;

    case KEY_CTRL_F:
        text_rows = ed.scr_rows - 1;
        n        = count * (text_rows - 2);
        top_line = scr_pos_line(ed.top_pos);
        total    = scr_line_count();
        /* Load when new screen bottom would exceed buffer: need a full screen
         * of lines above new_top plus text_rows more for the new viewport. */
        if (ed.tail_offset > 0L && top_line + n + text_rows > total) {
            gb_load_more(LOAD_CHUNK);
            /* Recompute: gb_discard_head inside gb_load_more shifts ed.top_pos. */
            top_line = scr_pos_line(ed.top_pos);
            total    = scr_line_count();
        }
        new_top = top_line + n;
        if (new_top >= total) new_top = total - 1;
        if (new_top <= top_line) break;
        ed.top_pos = scr_line_start(new_top);
        pg_mid(new_top, total);
        break;

    case KEY_CTRL_B:
        text_rows = ed.scr_rows - 1;
        n        = count * (text_rows - 2);
        if (ed.top_pos == 0 && ed.win_start > 0L) {
            new_off = ed.win_start - (long)LOAD_CHUNK;
            if (new_off < 0L) new_off = 0L;
            gb_reload_from(new_off);
            total   = scr_line_count();
            new_top = total - 1 - n;
            if (new_top < 0) new_top = 0;
        } else {
            if (ed.top_pos == 0) break;
            top_line = scr_pos_line(ed.top_pos);
            new_top  = top_line - n;
            if (new_top < 0) new_top = 0;
            total = scr_line_count();
        }
        ed.top_pos = scr_line_start(new_top);
        pg_mid(new_top, total);
        break;

    case KEY_CTRL_D:
        n = (ed.scr_rows - 1) >> 1;  /* >> 1 replaces / 2 */
        if (ed.tail_offset > 0L &&
            ed.cur_pos >= gb_content_len() - n * ed.scr_cols)
            gb_load_more(LOAD_CHUNK);
        pg_top = ed.top_pos;         /* after load: positions rebased */
        mv_down(n * count);
        scr_scroll_to_cursor();
        scr_update_after_move(pg_top);   /* scroll + paint new rows only */
        break;

    case KEY_CTRL_U:
        n = (ed.scr_rows - 1) >> 1;  /* >> 1 replaces / 2 */
        pg_top = ed.top_pos;
        mv_up(n * count);
        scr_scroll_to_cursor();
        scr_update_after_move(pg_top);
        break;
    }
}

static void normal_cmd(c)
int c;
{
    static int  count, size, linewise, endpoint;
    static int  had_count, old_top;
    static int  op, nc_from, nc_to, nc_k;
    static int  gg_line;
    static long k_new_off;

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
         * Walk with find_bol/find_eol -- O(range), not O(buffer). */
        if (c == op) {
            nc_from = find_bol(ed.cur_pos);
            nc_to   = nc_from;
            for (nc_k = count; nc_k > 0; nc_k--) {
                nc_to = find_eol(nc_to);
                if (nc_to >= size) break;
                nc_to++;    /* include the newline */
            }
            /* cc/S replace the line's text but keep its newline */
            if (op == 'c' && nc_to > nc_from && gb_char_at(nc_to - 1) == '\n')
                nc_to--;
            if (op != 'y') {
                ed.dot_cmd = op; ed.dot_motion = op;
                ed.dot_count = count; ed.dot_arg = 0; ed.dot_len = 0;
            }
            if (op == 'c') g_ins_cmd = 'c';
            apply_op(op, nc_from, nc_to, 1);
            return;
        }

        /* motion character */
        linewise = 0;
        if (op == 'c' && c == 'w') me_cw = 1;
        endpoint = motion_endpoint(c, count, &linewise);
        if (endpoint < 0) {
            hvi_sprintf(ed.status, "Unknown motion: %c", c, 0);
            scr_show_status(ed.status);
            return;
        }
        if (op != 'y') {
            ed.dot_cmd = op; ed.dot_motion = c;
            ed.dot_count = count; ed.dot_arg = 0; ed.dot_len = 0;
        }
        if (op == 'c') g_ins_cmd = 'c';
        apply_op(op, ed.cur_pos, endpoint, linewise);
        return;
    }

    /* ---- 'g' prefix (gg) ---- */
    if (g_g) {
        g_g = 0;
        if (c == 'g') {
            gg_line = (g_g_count > 0) ? g_g_count : 1;
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
        old_top = ed.top_pos;
        mv_down(count); mv_bnb(); scr_scroll_to_cursor();
        scr_update_after_move(old_top);
        break;

    case 'j':
        if (ed.tail_offset > 0L &&
            ed.cur_pos >= gb_content_len() - ed.scr_cols) {
            gb_load_more(LOAD_CHUNK);
            mv_down(count); scr_scroll_to_cursor();
            scr_refresh();
        } else {
            old_top = ed.top_pos;
            mv_down(count); scr_scroll_to_cursor();
            scr_update_after_move(old_top);
        }
        break;
    case 'k':
        old_top = ed.top_pos;
        mv_up(count); scr_scroll_to_cursor();
        if (ed.cur_pos == 0 && ed.win_start > 0L) {
            k_new_off = ed.win_start - (long)LOAD_CHUNK;
            if (k_new_off < 0L) k_new_off = 0L;
            gb_reload_from(k_new_off);
            ed.cur_pos = scr_last_line_start();
            nav_refresh();
            break;
        }
        scr_update_after_move(old_top);
        break;

    case 'w':
    case 'b':
    case 'e':
        old_top = ed.top_pos;
        mv_word(c, count); scr_scroll_to_cursor();
        scr_update_after_move(old_top);
        break;

    case '0':  mv_bol();  ed.want_col = 0; scr_update_cursor(); break;
    case '^':  mv_bnb();  ed.want_col = scr_pos_col(ed.cur_pos); scr_update_cursor(); break;
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
                scr_show_status(ed.status);
        } else {
            if (ed.status[0]) {
                ed.status[0] = '\0';
                scr_clear_status();
            }
            normal_cmd(s_er_c);
        }
    }
}
