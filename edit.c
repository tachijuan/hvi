/*
 * edit.c - VI command processing for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Implements normal mode, insert mode, and command-line mode.
 * Operator-motion model: d/c/y + motion applies to a range.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hvi.h"

extern Editor ed;

/* ------------------------------------------------------------------ */
/*  Static state                                                        */
/* ------------------------------------------------------------------ */
static int g_op      = 0;  /* pending operator: 'd','c','y', 0=none */
static int g_count   = 0;  /* digit-prefix accumulator              */
static int g_hcnt    = 0;  /* non-zero when a count digit was seen  */
static int g_g       = 0;  /* 'g' prefix pending                    */
static int g_g_count = 1;  /* count saved when 'g' prefix was typed */
static int g_find_char = 0; /* last char target for f/F             */
static int g_find_dir  = 1; /* 1 = forward (f), -1 = backward (F)  */
static int g_ins_cmd   = 0; /* command that entered insert mode     */

/* Return effective count (at least 1), then clear. */
static int get_count()
{
    int n = (g_hcnt) ? g_count : 1;
    g_count = 0; g_hcnt = 0;
    return n;
}

/*
 * Move cursor to the next (dir>0) or previous (dir<0) occurrence of ch
 * on the current logical line.  Stays put if not found.
 */
static void do_find(ch, dir)
int ch, dir;
{
    int p, sz;
    if (!ch) return;
    sz = gb_content_len();
    if (dir > 0) {
        p = ed.cur_pos + 1;
        while (p < sz && gb_char_at(p) != '\n') {
            if (gb_char_at(p) == ch) { ed.cur_pos = p; return; }
            p++;
        }
    } else {
        p = ed.cur_pos - 1;
        while (p >= 0 && gb_char_at(p) != '\n') {
            if (gb_char_at(p) == ch) { ed.cur_pos = p; return; }
            p--;
        }
    }
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
void mv_word_fwd();
void mv_word_back();
void mv_word_end();
int  motion_endpoint();
void apply_op();
int  read_pattern();
int  do_search_from();

/* ------------------------------------------------------------------ */
/*  Insert mode                                                         */
/* ------------------------------------------------------------------ */

/*
 * Handle one insert-mode keypress.
 * Returns 0 to stay in insert mode, 1 to return to normal mode.
 */
static int insert_key(c)
int c;
{
    char tmp[2];
    int del_ch, cur_col, new_col, sz;

    if (c == KEY_ESC) {
        int old_top, i, ilen;
        ed.mode = MODE_NORMAL;
        /* Capture inserted text for dot-repeat before adjusting cursor. */
        if (g_ins_cmd && ed.undo.type == UNDO_INSERT && ed.undo.len > 0) {
            ilen = ed.undo.len;
            if (ilen > DOT_TEXT_MAX) ilen = DOT_TEXT_MAX;
            for (i = 0; i < ilen; i++)
                ed.dot_text[i] = (char)gb_char_at(ed.undo.pos + i);
            ed.dot_len   = ilen;
            ed.dot_cmd   = g_ins_cmd;
            ed.dot_count = 1;
            ed.dot_arg   = 0;
            if (g_ins_cmd != 'c') ed.dot_motion = 0;
        }
        g_ins_cmd = 0;
        if (ed.cur_pos > 0) {
            int prev = gb_char_at(ed.cur_pos - 1);
            if (prev != '\n') ed.cur_pos--;
        }
        ed.want_col = scr_pos_col(ed.cur_pos);
        ed.status[0] = '\0';
        old_top = ed.top_pos;
        scr_scroll_to_cursor();
        if (ed.top_pos != old_top) {
            /* Viewport shifted (cursor was below screen): full refresh. */
            scr_refresh();
        } else {
            /* Common case: only redraw the line that was edited. */
            scr_redraw_cur_line();
            scr_show_status(ed.status);
        }
        return 1;
    }

    if (c == KEY_BS || c == KEY_DEL || c == KEY_CTRL_H) {
        if (ed.cur_pos > 0) {
            del_ch = gb_char_at(ed.cur_pos - 1);
            ed.cur_pos--;
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
            if (ed.undo.type == UNDO_INSERT && ed.undo.len > 0)
                ed.undo.len--;
            if (del_ch == '\n') {
                scr_scroll_to_cursor();
                scr_refresh();
                scr_show_status("-- INSERT --");
            } else if (del_ch == '\t') {
                /* tab width varies — must redraw to recompute columns */
                scr_redraw_cur_line();
            } else {
                sz = gb_content_len();
                if (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n') {
                    /*
                     * At end of line: BS moves terminal cursor left one
                     * column (ANSI BS crosses visual-row boundaries correctly),
                     * then clreol erases the deleted char.  No term_goto.
                     */
                    term_putch(KEY_BS);
                    term_clreol();
                } else {
                    /* Middle of line: remaining chars must shift left. */
                    scr_redraw_cur_line();
                }
            }
        }
        return 0;
    }

    if (c == KEY_CTRL_W) {
        /* delete previous word */
        int start = ed.cur_pos;
        while (ed.cur_pos > 0 && isspacech(gb_char_at(ed.cur_pos - 1)))
            ed.cur_pos--;
        while (ed.cur_pos > 0 && !isspacech(gb_char_at(ed.cur_pos - 1)))
            ed.cur_pos--;
        if (ed.cur_pos < start) {
            int del = start - ed.cur_pos;
            gb_delete(ed.cur_pos, del);
            ed.modified = 1;
            if (ed.undo.type == UNDO_INSERT) {
                ed.undo.len -= del;
                if (ed.undo.len < 0) ed.undo.len = 0;
            }
        }
        scr_scroll_to_cursor();
        scr_refresh();
        scr_show_status("-- INSERT --");
        return 0;
    }

    if (c == KEY_CTRL_U) {
        /* delete to start of line */
        int sol = ed.cur_pos;
        while (sol > 0 && gb_char_at(sol - 1) != '\n') sol--;
        if (sol < ed.cur_pos) {
            int del = ed.cur_pos - sol;
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

    /* Regular character or Enter */
    if (c == KEY_CR) c = '\n';

    if (c == '\n') {
        tmp[0] = '\n';
        if (!gb_insert(ed.cur_pos, tmp, 1)) {
            strcpy(ed.status, "Buffer full");
            return 0;
        }
        ed.cur_pos++;
        ed.modified = 1;
        if (ed.undo.type == UNDO_INSERT) ed.undo.len++;
        scr_scroll_to_cursor();
        scr_refresh();
        scr_show_status("-- INSERT --");
        return 0;
    }

    /*
     * Compute the visual-row column BEFORE inserting so we can decide
     * whether the new character will overflow the right edge.
     * scr_vrow_col() is O(chars-from-line-start) — far cheaper than a
     * screen redraw at 9600 baud.
     */
    cur_col = scr_vrow_col(ed.cur_pos);
    new_col = (c == '\t') ? (cur_col / TAB_STOP + 1) * TAB_STOP
                          : cur_col + 1;

    tmp[0] = (char)c;
    if (!gb_insert(ed.cur_pos, tmp, 1)) {
        strcpy(ed.status, "Buffer full");
        return 0;
    }
    ed.cur_pos++;
    ed.modified = 1;
    if (ed.undo.type == UNDO_INSERT) ed.undo.len++;

    if (new_col > ed.scr_cols) {
        /* Char caused a visual-row wrap: all rows below must shift down. */
        scr_redraw_cur_line();
    } else if (c == '\t') {
        /* Expand tab as spaces; terminal cursor already in the right place. */
        while (cur_col < new_col) { term_putch(' '); cur_col++; }
    } else {
        /*
         * Common case: char fits on the current visual row.  Output it
         * directly — the terminal cursor auto-advances one column,
         * matching the new cur_pos.  Zero escape sequences emitted.
         */
        term_putch(c);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Command-line (ex) mode                                              */
/* ------------------------------------------------------------------ */

static void cmdline_mode()
{
    int c;

    ed.cmdline[0] = '\0';
    ed.cmdlen = 0;
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    term_putch(':');

    for (;;) {
        c = term_getch();
        if (c == KEY_ESC) {
            scr_clear_status();
            return;
        }
        if (c == KEY_CR || c == KEY_LF) {
            ed.cmdline[ed.cmdlen] = '\0';
            if (ed.cmdlen > 0)
                ex_execute(ed.cmdline);
            if (!ed.quit) scr_refresh();
            return;
        }
        if ((c == KEY_BS || c == KEY_DEL) && ed.cmdlen > 0) {
            ed.cmdlen--;
            term_putch(KEY_BS); term_putch(' '); term_putch(KEY_BS);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && ed.cmdlen < CMD_MAX - 1) {
            ed.cmdline[ed.cmdlen++] = (char)c;
            term_putch(c);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Normal mode dispatcher                                              */
/* ------------------------------------------------------------------ */

/* Character search on current line: f, F, ;, ,, .
 * Defined before normal_misc_cmd so no forward declaration is needed. */
static void normal_find_cmd(c, count)
int c, count;
{
    int n;
    switch (c) {
    case 'f':
    case 'F':
        {
            char prompt[3];
            int ch;
            prompt[0] = (char)c;
            prompt[1] = '_';
            prompt[2] = '\0';
            scr_show_status(prompt);   /* show "f_" or "F_" while waiting */
            ch = term_getch();
            scr_clear_status();
            g_find_char = ch;
            g_find_dir  = (c == 'f') ? 1 : -1;
            for (n = 0; n < count; n++)
                do_find(g_find_char, g_find_dir);
            ed.want_col = scr_pos_col(ed.cur_pos);
            scr_update_cursor();
        }
        break;

    case ';':   /* repeat last f/F in same direction */
        for (n = 0; n < count; n++)
            do_find(g_find_char, g_find_dir);
        ed.want_col = scr_pos_col(ed.cur_pos);
        scr_update_cursor();
        break;

    case ',':   /* repeat last f/F in opposite direction */
        for (n = 0; n < count; n++)
            do_find(g_find_char, -g_find_dir);
        ed.want_col = scr_pos_col(ed.cur_pos);
        scr_update_cursor();
        break;

    default:
        break;
    }
}

/* Adjust cursor to the insertion point for the original entry command. */
static void dot_ins_position()
{
    int sz, eol;
    switch (ed.dot_cmd) {
    case 'a':
        sz = gb_content_len();
        if (sz > 0 && ed.cur_pos < sz && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        break;
    case 'A':
        mv_eol();
        sz = gb_content_len();
        if (sz > 0 && ed.cur_pos < sz && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        break;
    case 'I':
        mv_bnb();
        break;
    case 'o':
        sz = gb_content_len();
        eol = ed.cur_pos;
        while (eol < sz && gb_char_at(eol) != '\n') eol++;
        if (eol < sz) eol++;
        ed.cur_pos = eol;
        break;
    case 'O':
        while (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;
        break;
    default: break;
    }
}

/* Replay c/C: delete the range then re-insert stored text. */
static void dot_replay_c(n)
int n;
{
    int from, to, ins_pos, sz, linewise, endpoint;
    linewise = 0;
    if (ed.dot_cmd == 'C' || ed.dot_motion == 'c') {
        from = ed.cur_pos;
        while (from > 0 && gb_char_at(from - 1) != '\n') from--;
        to = from;
        sz = gb_content_len();
        while (to < sz && gb_char_at(to) != '\n') to++;
    } else {
        endpoint = motion_endpoint(ed.dot_motion, n, &linewise);
        if (endpoint < 0) return;
        from = (ed.cur_pos < endpoint) ? ed.cur_pos : endpoint;
        to   = (ed.cur_pos < endpoint) ? endpoint   : ed.cur_pos;
    }
    ins_pos = from;
    if (to > from) {
        undo_save_delete(from, to - from);
        gb_delete(from, to - from);
        ed.cur_pos = from;
        ed.modified = 1;
    }
    if (ed.dot_len > 0) {
        undo_save_insert(ins_pos, ed.dot_len);
        gb_insert(ins_pos, ed.dot_text, ed.dot_len);
        ed.cur_pos = ins_pos + ed.dot_len;
        if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;
        if (ed.cur_pos < 0) ed.cur_pos = 0;
    }
    scr_scroll_to_cursor();
    scr_refresh();
}

/*
 * Replay the last change command at the current cursor position.
 * count == 0 means use the stored dot_count; otherwise use count.
 */
static void dot_replay(count)
int count;
{
    int  n, sz, k, linewise, endpoint;
    int  from, to, ins_pos, ch, eol;
    int  start_line, end_line, total;
    char tmp_c[1];
    char sp;

    if (!ed.dot_cmd) return;
    n  = (count > 0) ? count : ed.dot_count;
    sp = ' ';

    switch (ed.dot_cmd) {

    case 'x':
        k = n;
        while (k-- > 0 && ed.cur_pos < gb_content_len() &&
               gb_char_at(ed.cur_pos) != '\n') {
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
        }
        sz = gb_content_len();
        if (ed.cur_pos >= sz) ed.cur_pos = (sz > 0) ? sz - 1 : 0;
        if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos) == '\n' &&
            gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;
        scr_redraw_cur_line();
        break;

    case 'X':
        k = n;
        while (k-- > 0 && ed.cur_pos > 0 &&
               gb_char_at(ed.cur_pos - 1) != '\n') {
            ed.cur_pos--;
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
        }
        scr_redraw_cur_line();
        break;

    case 'r':
        sz = gb_content_len();
        if (ed.cur_pos < sz) {
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            tmp_c[0] = (char)ed.dot_arg;
            gb_insert(ed.cur_pos, tmp_c, 1);
            ed.modified = 1;
            if (ed.dot_arg == '\n') scr_refresh();
            else scr_redraw_cur_line();
        }
        break;

    case 'D':
        to = ed.cur_pos;
        sz = gb_content_len();
        while (to < sz && gb_char_at(to) != '\n') to++;
        if (to > ed.cur_pos) {
            undo_save_delete(ed.cur_pos, to - ed.cur_pos);
            gb_delete(ed.cur_pos, to - ed.cur_pos);
            ed.modified = 1;
            sz = gb_content_len();
            if (ed.cur_pos > 0 && (ed.cur_pos >= sz ||
                gb_char_at(ed.cur_pos) == '\n'))
                if (gb_char_at(ed.cur_pos - 1) != '\n')
                    ed.cur_pos--;
        }
        scr_redraw_cur_line();
        break;

    case '~':
        sz = gb_content_len();
        for (k = 0; k < n; k++) {
            if (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n') break;
            ch = gb_char_at(ed.cur_pos);
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
            tmp_c[0] = (char)ch;
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            gb_insert(ed.cur_pos, tmp_c, 1);
            ed.cur_pos++;
            ed.modified = 1;
        }
        scr_redraw_cur_line();
        break;

    case 'J':
        k = (n > 1) ? n - 1 : 1;
        while (k-- > 0) {
            sz = gb_content_len();
            eol = ed.cur_pos;
            while (eol < sz && gb_char_at(eol) != '\n') eol++;
            if (eol >= sz) break;
            undo_save_delete(eol, 1);
            gb_delete(eol, 1);
            sz = gb_content_len();
            if (eol < sz && gb_char_at(eol) != ' ')
                gb_insert(eol, &sp, 1);
            ed.modified = 1;
        }
        scr_refresh();
        break;

    case 'd':
        if (ed.dot_motion == 'd') {
            start_line = scr_pos_line(ed.cur_pos);
            end_line = start_line + ed.dot_count - 1;
            total = scr_line_count();
            if (end_line >= total) end_line = total - 1;
            from = scr_line_start(start_line);
            to = (end_line + 1 < total)
                 ? scr_line_start(end_line + 1) : gb_content_len();
            apply_op('d', from, to, 1);
        } else {
            linewise = 0;
            endpoint = motion_endpoint(ed.dot_motion, ed.dot_count, &linewise);
            if (endpoint >= 0)
                apply_op('d', ed.cur_pos, endpoint, linewise);
        }
        break;

    case 'c':
    case 'C':
        dot_replay_c(n);
        break;

    default:
        /* Insert-mode commands: position then re-insert stored text. */
        if (ed.dot_len > 0) {
            dot_ins_position();
            ins_pos = ed.cur_pos;
            undo_save_insert(ins_pos, ed.dot_len);
            gb_insert(ins_pos, ed.dot_text, ed.dot_len);
            ed.cur_pos = ins_pos + ed.dot_len;
            if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
                ed.cur_pos--;
            ed.modified = 1;
            scr_scroll_to_cursor();
            scr_refresh();
        }
        break;
    }
}

/* Handle insert/edit/search commands (split from normal_cmd for optimizer). */
/* Handle yank/put/search/undo/ex commands. */
static void normal_misc_cmd(c, count, size)
int c, count, size;
{
    int save_len;
    switch (c) {

    /* --- Yank/Put --- */
    case 'Y':   /* yank current line (alias for yy) */
        {
            int ln    = scr_pos_line(ed.cur_pos);
            int from  = scr_line_start(ln);
            int to    = (ln + 1 < scr_line_count()) ? scr_line_start(ln + 1) : size;
            int save  = to - from;
            int i;
            if (save >= YANK_MAX) save = YANK_MAX - 1;
            for (i = 0; i < save; i++) {
                int ch = gb_char_at(from + i);
                ed.yank[i] = (ch < 0) ? 0 : (char)ch;
            }
            ed.yank[save] = '\0';
            ed.yank_len   = save;
            ed.yank_line  = 1;
            sprintf(ed.status, "1 line yanked");
            scr_show_status(ed.status);
        }
        break;

    case 'p':   /* put after cursor */
        if (ed.yank_len > 0) {
            int ins_pos;
            undo_save_insert(ed.cur_pos, ed.yank_len);
            if (ed.yank_line) {
                /* linewise: insert below current line */
                ins_pos = ed.cur_pos;
                while (ins_pos < size && gb_char_at(ins_pos) != '\n') ins_pos++;
                if (ins_pos < size) ins_pos++;
                gb_insert(ins_pos, ed.yank, ed.yank_len);
                /* ensure trailing newline */
                if (ed.yank[ed.yank_len - 1] != '\n') {
                    char nl = '\n';
                    gb_insert(ins_pos + ed.yank_len, &nl, 1);
                }
                ed.cur_pos = ins_pos;
            } else {
                ins_pos = ed.cur_pos;
                if (ins_pos < gb_content_len() && gb_char_at(ins_pos) != '\n')
                    ins_pos++;
                gb_insert(ins_pos, ed.yank, ed.yank_len);
                ed.cur_pos = ins_pos;
            }
            ed.modified = 1;
            scr_scroll_to_cursor();
            scr_refresh();
        }
        break;

    case 'P':   /* put before cursor */
        if (ed.yank_len > 0) {
            int ins_pos;
            undo_save_insert(ed.cur_pos, ed.yank_len);
            if (ed.yank_line) {
                ins_pos = scr_line_start(scr_pos_line(ed.cur_pos));
                gb_insert(ins_pos, ed.yank, ed.yank_len);
                if (ed.yank[ed.yank_len - 1] != '\n') {
                    char nl = '\n';
                    gb_insert(ins_pos + ed.yank_len, &nl, 1);
                }
                ed.cur_pos = ins_pos;
            } else {
                gb_insert(ed.cur_pos, ed.yank, ed.yank_len);
            }
            ed.modified = 1;
            scr_scroll_to_cursor();
            scr_refresh();
        }
        break;

    /* --- Search --- */
    case '/':   /* search forward */
        ed.search_dir = SEARCH_FWD;
        if (read_pattern('/')) {
            int pos = do_search_from(ed.cur_pos);
            if (pos >= 0) {
                ed.cur_pos = pos;
                scr_scroll_to_cursor();
                scr_refresh();
            } else {
                strcpy(ed.status, "Pattern not found");
                scr_show_status(ed.status);
            }
        } else {
            scr_clear_status();
        }
        break;

    case '?':   /* search backward */
        ed.search_dir = SEARCH_BWD;
        if (read_pattern('?')) {
            int pos = do_search_from(ed.cur_pos);
            if (pos >= 0) {
                ed.cur_pos = pos;
                scr_scroll_to_cursor();
                scr_refresh();
            } else {
                strcpy(ed.status, "Pattern not found");
                scr_show_status(ed.status);
            }
        } else {
            scr_clear_status();
        }
        break;

    case 'n':   /* repeat last search */
        {
            int pos = do_search_from(ed.cur_pos);
            if (pos >= 0) {
                ed.cur_pos = pos;
                scr_scroll_to_cursor();
                scr_refresh();
            } else {
                strcpy(ed.status, "Pattern not found");
                scr_show_status(ed.status);
            }
        }
        break;

    case 'N':   /* repeat last search in opposite direction */
        {
            int old_dir = ed.search_dir;
            int pos;
            ed.search_dir = -old_dir;
            pos = do_search_from(ed.cur_pos);
            ed.search_dir = old_dir;
            if (pos >= 0) {
                ed.cur_pos = pos;
                scr_scroll_to_cursor();
                scr_refresh();
            } else {
                strcpy(ed.status, "Pattern not found");
                scr_show_status(ed.status);
            }
        }
        break;

    /* --- Undo --- */
    case 'u':
        if (ed.undo.type == UNDO_DELETE) {
            /* re-insert the deleted text */
            save_len = ed.undo.len;
            if (save_len > UNDO_MAX) save_len = UNDO_MAX;
            gb_insert(ed.undo.pos, ed.undo.text, save_len);
            ed.cur_pos   = ed.undo.pos;
            ed.modified  = ed.undo.was_clean ? 0 : 1;
            ed.undo.type = UNDO_NONE;
            scr_scroll_to_cursor();
            scr_refresh();
        } else if (ed.undo.type == UNDO_INSERT) {
            /* delete the previously inserted text */
            gb_delete(ed.undo.pos, ed.undo.len);
            ed.cur_pos   = ed.undo.pos;
            ed.modified  = ed.undo.was_clean ? 0 : 1;
            ed.undo.type = UNDO_NONE;
            scr_scroll_to_cursor();
            scr_refresh();
        } else {
            strcpy(ed.status, "Nothing to undo");
            scr_show_status(ed.status);
        }
        break;

    /* --- Screen control --- */
    case KEY_CTRL_L:   /* redraw */
        term_clear();
        scr_refresh();
        break;

    /* --- Ex command line --- */
    case ':':   cmdline_mode(); break;

    /* --- Join lines --- */
    case 'J':
        ed.dot_cmd = 'J'; ed.dot_count = count;
        ed.dot_motion = 0; ed.dot_arg = 0;
        {
            int n = (count > 1) ? count - 1 : 1;
            while (n-- > 0) {
                int sz = gb_content_len();
                int end_of_line = ed.cur_pos;
                char sp = ' ';
                while (end_of_line < sz && gb_char_at(end_of_line) != '\n')
                    end_of_line++;
                if (end_of_line >= sz) break;
                undo_save_delete(end_of_line, 1);
                gb_delete(end_of_line, 1); /* remove newline */
                /* insert space unless next char is space */
                sz = gb_content_len();
                if (end_of_line < sz && gb_char_at(end_of_line) != ' ')
                    gb_insert(end_of_line, &sp, 1);
                ed.modified = 1;
            }
            scr_refresh();
        }
        break;

    /* --- Tilde: toggle case --- */
    case '~':
        ed.dot_cmd = '~'; ed.dot_count = count;
        ed.dot_motion = 0; ed.dot_arg = 0;
        {
            int sz = gb_content_len();
            if (ed.cur_pos < sz && gb_char_at(ed.cur_pos) != '\n') {
                int ch = gb_char_at(ed.cur_pos);
                char repl_ch[1];
                if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
                else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
                repl_ch[0] = (char)ch;
                undo_save_delete(ed.cur_pos, 1);
                gb_delete(ed.cur_pos, 1);
                gb_insert(ed.cur_pos, repl_ch, 1);
                ed.modified = 1;
                mv_right(1);
                scr_redraw_cur_line();
            }
        }
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

    case 'D':   /* delete to end of line */
        {
            int to = ed.cur_pos;
            while (to < size && gb_char_at(to) != '\n') to++;
            undo_save_delete(ed.cur_pos, to - ed.cur_pos);
            gb_delete(ed.cur_pos, to - ed.cur_pos);
            ed.modified = 1;
            if (ed.cur_pos > 0 && (ed.cur_pos >= gb_content_len() ||
                gb_char_at(ed.cur_pos) == '\n')) {
                if (gb_char_at(ed.cur_pos - 1) != '\n')
                    ed.cur_pos--;
            }
            ed.dot_cmd = 'D'; ed.dot_count = 1;
            ed.dot_motion = 0; ed.dot_arg = 0;
            scr_redraw_cur_line();
        }
        break;

    case 'C':   /* change to end of line */
        {
            int to = ed.cur_pos;
            while (to < size && gb_char_at(to) != '\n') to++;
            undo_save_delete(ed.cur_pos, to - ed.cur_pos);
            gb_delete(ed.cur_pos, to - ed.cur_pos);
            ed.modified = 1;
            ed.dot_cmd = 'C'; ed.dot_count = 1;
            ed.dot_motion = 0; ed.dot_arg = 0; ed.dot_len = 0;
            g_ins_cmd = 'C';
            undo_save_insert(ed.cur_pos, 0);
            scr_redraw_cur_line();
            ed.mode = MODE_INSERT;
            scr_show_status("-- INSERT --");
        }
        break;

    case 'x':   /* delete char under cursor */
        {
            int n = count;
            while (n-- > 0 && ed.cur_pos < gb_content_len() &&
                   gb_char_at(ed.cur_pos) != '\n') {
                undo_save_delete(ed.cur_pos, 1);
                gb_delete(ed.cur_pos, 1);
                ed.modified = 1;
            }
            {
                int sz = gb_content_len();
                if (ed.cur_pos >= sz) ed.cur_pos = sz > 0 ? sz - 1 : 0;
                if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos) == '\n'
                    && gb_char_at(ed.cur_pos-1) != '\n')
                    ed.cur_pos--;
            }
            ed.dot_cmd = 'x'; ed.dot_count = count;
            ed.dot_motion = 0; ed.dot_arg = 0;
            scr_redraw_cur_line();
        }
        break;

    case 'X':   /* delete char before cursor */
        {
            int n = count;
            while (n-- > 0 && ed.cur_pos > 0 &&
                   gb_char_at(ed.cur_pos - 1) != '\n') {
                ed.cur_pos--;
                undo_save_delete(ed.cur_pos, 1);
                gb_delete(ed.cur_pos, 1);
                ed.modified = 1;
            }
            ed.dot_cmd = 'X'; ed.dot_count = count;
            ed.dot_motion = 0; ed.dot_arg = 0;
            scr_redraw_cur_line();
        }
        break;

    case 'r':   /* replace single character */
        {
            int repl = term_getch();
            if (repl != KEY_ESC && ed.cur_pos < size) {
                char tmp_c[1];
                if (repl == KEY_CR) repl = '\n';
                undo_save_delete(ed.cur_pos, 1);
                gb_delete(ed.cur_pos, 1);
                tmp_c[0] = (char)repl;
                gb_insert(ed.cur_pos, tmp_c, 1);
                ed.modified = 1;
                ed.dot_cmd = 'r'; ed.dot_count = 1;
                ed.dot_motion = 0; ed.dot_arg = repl;
                if (repl == '\n') scr_refresh();
                else scr_redraw_cur_line();
            }
        }
        break;

    default:
        normal_misc_cmd(c, count, size);
        break;
    }
}

static void normal_edit_cmd(c, count, size)
int c, count, size;
{
    switch (c) {

    /* --- Insert mode entry --- */
    case 'i':   /* insert before cursor */
        g_ins_cmd = 'i';
        undo_save_insert(ed.cur_pos, 0);
        ed.mode = MODE_INSERT;
        scr_show_status("-- INSERT --");
        break;

    case 'a':   /* append after cursor */
        if (size > 0 && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        g_ins_cmd = 'a';
        undo_save_insert(ed.cur_pos, 0);
        ed.mode = MODE_INSERT;
        scr_show_status("-- INSERT --");
        break;

    case 'I':   /* insert at beginning of line */
        mv_bnb();
        g_ins_cmd = 'I';
        undo_save_insert(ed.cur_pos, 0);
        ed.mode = MODE_INSERT;
        scr_show_status("-- INSERT --");
        break;

    case 'A':   /* append at end of line */
        mv_eol();
        if (size > 0 && ed.cur_pos < size && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        g_ins_cmd = 'A';
        undo_save_insert(ed.cur_pos, 0);
        ed.mode = MODE_INSERT;
        scr_show_status("-- INSERT --");
        break;

    case 'o':   /* open line below */
        g_ins_cmd = 'o';
        {
            char nl = '\n';
            int  eol = ed.cur_pos;
            int  sz  = gb_content_len();
            /* find position just after the '\n' at end of current line */
            while (eol < sz && gb_char_at(eol) != '\n') eol++;
            if (eol < sz) eol++;  /* move past existing '\n' */
            /* For last line with no trailing newline, insert one first */
            if (eol >= sz && sz > 0 && gb_char_at(sz - 1) != '\n') {
                gb_insert(sz, &nl, 1);
                eol = sz + 1;
            }
            undo_save_insert(eol, 0);
            gb_insert(eol, &nl, 1);  /* the actual new empty line */
            ed.cur_pos = eol;        /* cursor on the new '\n' line */
            ed.modified = 1;
            ed.undo.len++;
            scr_scroll_to_cursor();
            scr_refresh();
            ed.mode = MODE_INSERT;
            scr_show_status("-- INSERT --");
        }
        break;

    case 'O':   /* open line above */
        g_ins_cmd = 'O';
        {
            char nl = '\n';
            mv_bol();
            undo_save_insert(ed.cur_pos, 0);
            gb_insert(ed.cur_pos, &nl, 1);
            ed.modified = 1;
            ed.undo.len++;
            scr_scroll_to_cursor();
            scr_refresh();
            ed.mode = MODE_INSERT;
            scr_show_status("-- INSERT --");
        }
        break;

    case 's':   /* substitute character (delete + insert) */
        g_ins_cmd = 's';
        {
            int del = (count > 1) ? count : 1;
            int from = ed.cur_pos;
            int to = from;
            while (del-- > 0 && to < size && gb_char_at(to) != '\n') to++;
            undo_save_delete(from, to - from);
            gb_delete(from, to - from);
            ed.modified = 1;
            undo_save_insert(from, 0);
            scr_refresh();
            ed.mode = MODE_INSERT;
            scr_show_status("-- INSERT --");
        }
        break;

    case 'S':   /* substitute line */
        g_ins_cmd = 'S';
        {
            int from = scr_line_start(scr_pos_line(ed.cur_pos));
            int to;
            to = from;
            while (to < size && gb_char_at(to) != '\n') to++;
            undo_save_delete(from, to - from);
            gb_delete(from, to - from);
            ed.cur_pos = from;
            ed.modified = 1;
            undo_save_insert(from, 0);
            scr_refresh();
            ed.mode = MODE_INSERT;
            scr_show_status("-- INSERT --");
        }
        break;

    default:
        normal_delchg_cmd(c, count, size);
        break;
    }
}

static void normal_cmd(c)
int c;
{
    int  count, size, linewise, endpoint;
    int  had_count, old_top;

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
        int op = g_op;
        g_op = 0;

        /* doubled operator (dd, cc, yy) = operate on count lines */
        if (c == op) {
            int start_line = scr_pos_line(ed.cur_pos);
            int end_line   = start_line + count - 1;
            int total      = scr_line_count();
            int from, to;
            if (end_line >= total) end_line = total - 1;
            from = scr_line_start(start_line);
            to   = (end_line + 1 < total)
                   ? scr_line_start(end_line + 1)
                   : size;
            if (op != 'y') {
                ed.dot_cmd = op; ed.dot_motion = op;
                ed.dot_count = count; ed.dot_arg = 0; ed.dot_len = 0;
            }
            if (op == 'c') g_ins_cmd = 'c';
            apply_op(op, from, to, 1);
            return;
        }

        /* motion character */
        linewise = 0;
        endpoint = motion_endpoint(c, count, &linewise);
        if (endpoint < 0) {
            sprintf(ed.status, "Unknown motion: %c", c);
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
            /* gg: go to first line (or line count if given before 'g') */
            int line = (g_g_count > 0) ? g_g_count - 1 : 0;
            if (line >= scr_line_count()) line = scr_line_count() - 1;
            ed.cur_pos = scr_line_start(line);
            mv_bnb();
            ed.want_col = 0;
            scr_scroll_to_cursor();
            scr_refresh();
        }
        return;
    }

    /* ---- normal commands ---- */
    switch (c) {

    /* --- Movement --- */
    case 'h':  mv_left(count);  scr_update_cursor(); break;
    case 'l':  mv_right(count); scr_update_cursor(); break;

    case 'j':
        old_top = ed.top_pos;
        /* Load more from tail when cursor nears the end of loaded content */
        if (ed.tail_offset > 0L &&
            ed.cur_pos >= gb_content_len() - ed.scr_cols)
            gb_load_more(LOAD_CHUNK);
        mv_down(count); scr_scroll_to_cursor();
        if (ed.top_pos != old_top) scr_refresh(); else scr_update_cursor();
        break;
    case 'k':
        old_top = ed.top_pos;
        mv_up(count); scr_scroll_to_cursor();
        if (ed.top_pos != old_top) scr_refresh(); else scr_update_cursor();
        break;

    case 'w':
        old_top = ed.top_pos;
        mv_word_fwd(count); scr_scroll_to_cursor();
        if (ed.top_pos != old_top) scr_refresh(); else scr_update_cursor();
        break;
    case 'b':
        old_top = ed.top_pos;
        mv_word_back(count); scr_scroll_to_cursor();
        if (ed.top_pos != old_top) scr_refresh(); else scr_update_cursor();
        break;
    case 'e':
        old_top = ed.top_pos;
        mv_word_end(count); scr_scroll_to_cursor();
        if (ed.top_pos != old_top) scr_refresh(); else scr_update_cursor();
        break;

    case '0':  mv_bol();  ed.want_col = 0; scr_update_cursor(); break;
    case '^':  mv_bnb();  ed.want_col = scr_pos_col(ed.cur_pos); scr_update_cursor(); break;
    case '$':  mv_eol();  ed.want_col = 9999; scr_update_cursor(); break;

    case '.':   /* repeat last change */
        dot_replay(had_count ? count : 0);
        break;

    case 'G':  /* go to last line, or line count if explicit */
        {
            int line, clen;
            /* Load as much tail as possible; advance cur_pos to near the end
             * each iteration so gb_load_more's "don't discard past cursor"
             * cap doesn't block progress when cur_pos starts at 0. */
            while (ed.tail_offset > 0L) {
                clen = gb_content_len();
                if (clen > 0) ed.cur_pos = clen - 1;
                if (gb_load_more(LOAD_CHUNK) == 0) break;
            }
            line = had_count ? count - 1 : scr_line_count() - 1;
            if (line < 0) line = 0;
            if (line >= scr_line_count()) line = scr_line_count() - 1;
            ed.cur_pos = scr_line_start(line);
            mv_bnb();
            ed.want_col = scr_pos_col(ed.cur_pos);
            scr_scroll_to_cursor();
            scr_refresh();
        }
        break;

    case 'g':  g_g = 1; g_g_count = had_count ? count : 0; return;

    case KEY_CTRL_F:   /* page forward */
        {
            int text_rows = ed.scr_rows - 1;
            int n = count * (text_rows - 2);
            int top_line = scr_pos_line(ed.top_pos) + n;
            int total    = scr_line_count();
            if (ed.tail_offset > 0L && top_line >= total - 1)
                gb_load_more(LOAD_CHUNK);
            total = scr_line_count();
            if (top_line >= total) top_line = total - 1;
            ed.top_pos  = scr_line_start(top_line);
            ed.cur_pos  = ed.top_pos;
            scr_refresh();
        }
        break;

    case KEY_CTRL_B:   /* page backward */
        {
            int text_rows = ed.scr_rows - 1;
            int n = count * (text_rows - 2);
            int top_line = scr_pos_line(ed.top_pos) - n;
            if (top_line < 0) top_line = 0;
            ed.top_pos  = scr_line_start(top_line);
            ed.cur_pos  = ed.top_pos;
            scr_refresh();
        }
        break;

    case KEY_CTRL_D:   /* half page down */
        {
            int n = (ed.scr_rows - 1) / 2;
            if (ed.tail_offset > 0L &&
                ed.cur_pos >= gb_content_len() - n * ed.scr_cols)
                gb_load_more(LOAD_CHUNK);
            mv_down(n * count);
            scr_scroll_to_cursor();
            scr_refresh();
        }
        break;

    case KEY_CTRL_U:   /* half page up */
        {
            int n = (ed.scr_rows - 1) / 2;
            mv_up(n * count);
            scr_scroll_to_cursor();
            scr_refresh();
        }
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

void edit_run()
{
    int c;

    ed.mode    = MODE_NORMAL;
    ed.quit    = 0;
    ed.want_col = 0;

    scr_refresh();

    while (!ed.quit) {
        c = term_getch();

        if (ed.debug) {
            fprintf(stderr, "DEBUG key=0x%02X mode=%d pos=%d\n",
                    c, ed.mode, ed.cur_pos);
        }

        if (ed.mode == MODE_INSERT) {
            /*
             * If a transient message (e.g. "Buffer full") was shown by the
             * previous keypress, clear it and restore the mode indicator
             * before processing the next key.  The mode indicator is shown
             * once on mode entry and must not be refreshed every keypress.
             */
            if (ed.status[0]) {
                ed.status[0] = '\0';
                scr_show_status("-- INSERT --");
            }
            insert_key(c);
            /* Display any transient message set by insert_key. */
            if (ed.mode == MODE_INSERT && ed.status[0])
                scr_show_status(ed.status);
        } else {
            /* Normal mode: clear any transient message then process key. */
            if (ed.status[0]) {
                ed.status[0] = '\0';
                scr_clear_status();
            }
            normal_cmd(c);
        }
    }
}
