/*
 * emove.c - Movement, operator application, and search helpers for HVI
 * Author: Juan Orlandini
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hvi.h"

extern Editor ed;

/* ------------------------------------------------------------------ */
/*  Character classification                                            */
/* ------------------------------------------------------------------ */

int iswordch(c)
int c;
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

int isspacech(c)
int c;
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* ------------------------------------------------------------------ */
/*  Undo helpers                                                        */
/* ------------------------------------------------------------------ */

void undo_save_delete(pos, len)
int pos, len;
{
    static int i, save, c;
    save = (len > UNDO_MAX) ? UNDO_MAX : len;
    ed.undo.type      = UNDO_DELETE;
    ed.undo.pos       = pos;
    ed.undo.len       = len;
    ed.undo.was_clean = !ed.modified;
    for (i = 0; i < save; i++) {
        c = gb_char_at(pos + i);
        ed.undo.text[i] = (c < 0) ? 0 : (char)c;
    }
    if (save < UNDO_MAX)
        ed.undo.text[save] = '\0';
}

void undo_save_insert(pos, len)
int pos, len;
{
    ed.undo.type      = UNDO_INSERT;
    ed.undo.pos       = pos;
    ed.undo.len       = len;
    ed.undo.was_clean = !ed.modified;
    ed.undo.text[0]   = '\0';
}

/* ------------------------------------------------------------------ */
/*  Movement helpers (do NOT update want_col unless it makes sense)    */
/* ------------------------------------------------------------------ */

static int h_vstart;
static void begin_hmove()
{
    h_vstart = (ed.cur_vrow >= 0) ? vrow_start_of(ed.cur_pos) : -1;
}

static void end_hmove()
{
    if (h_vstart >= 0 && vrow_start_of(ed.cur_pos) != h_vstart)
        ed.cur_vrow = -1;
}

/* Place cursor at column wantcol on line starting at lstart. */
int pos_at_col(lstart, wantcol)
int lstart, wantcol;
{
    static int pos, col, size, c, nc;
    pos  = lstart;
    col  = 0;
    size = gb_content_len();
    while (pos < size && gb_char_at(pos) != '\n') {
        c  = gb_char_at(pos);
        nc = (c == '\t') ? (col | (TAB_STOP - 1)) + 1 : col + 1;
        if (nc > wantcol) break;
        col = nc;
        pos++;
    }
    if (pos > lstart && (pos >= size || gb_char_at(pos) == '\n'))
        pos--;
    return pos;
}

void mv_bol()   /* beginning of line */
{
    begin_hmove();
    ed.cur_pos = find_bol(ed.cur_pos);
    end_hmove();
}

void mv_bnb()   /* first non-blank of line */
{
    static int size, c;
    size = gb_content_len();
    begin_hmove();
    ed.cur_pos = find_bol(ed.cur_pos);
    while (ed.cur_pos < size) {
        c = gb_char_at(ed.cur_pos);
        if (c != ' ' && c != '\t') break;
        ed.cur_pos++;
    }
    end_hmove();
}

void mv_eol()   /* end of line (last real char) */
{
    static int size;
    size = gb_content_len();
    begin_hmove();
    if (size == 0) return;
    if (gb_char_at(ed.cur_pos) == '\n') return;
    while (ed.cur_pos < size - 1) {
        if (gb_char_at(ed.cur_pos + 1) == '\n') break;
        ed.cur_pos++;
    }
    if (ed.cur_pos < size && gb_char_at(ed.cur_pos) == '\n'
        && ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
        ed.cur_pos--;
    end_hmove();
}

void mv_left(n)
int n;
{
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos == 0) break;
        if (gb_char_at(ed.cur_pos - 1) == '\n') break;
        ed.cur_pos--;
    }
    ed.want_col = scr_pos_col(ed.cur_pos);
    end_hmove();
}

void mv_right(n)
int n;
{
    static int size;
    size = gb_content_len();
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos >= size) break;
        /* can't move if on newline (empty line) */
        if (gb_char_at(ed.cur_pos) == '\n') break;
        /* can't move if at last byte or if next char is newline */
        if (ed.cur_pos + 1 >= size) break;
        if (gb_char_at(ed.cur_pos + 1) == '\n') break;
        ed.cur_pos++;
    }
    ed.want_col = scr_pos_col(ed.cur_pos);
    end_hmove();
}

/*
 * mv_up / mv_down scan only the neighbouring lines (O(line_length))
 * instead of calling scr_pos_line() + scr_line_start() which each scan
 * the buffer from position 0 (O(buffer)).  At 4 MHz this matters.
 *
 * They also maintain ed.cur_vrow incrementally so that
 * scr_scroll_to_cursor() can skip its O(text_rows) viewport scan entirely,
 * reducing the per-keypress cost of j/k from O(text_rows * line_len) to
 * O(line_len).  cur_vrow tracks the cursor's visual-row offset from top_pos.
 */
void mv_up(n)
int n;
{
    static int pos, prev_lstart, target, curr_vstart, target_vstart;
    while (n-- > 0) {
        pos = find_bol(ed.cur_pos);
        if (pos == 0) break;
        pos--;
        prev_lstart = find_bol(pos);
        target = pos_at_col(prev_lstart, ed.want_col);
        
        if (ed.cur_vrow >= 0) {
            curr_vstart = vrow_start_of(ed.cur_pos);
            target_vstart = vrow_start_of(target);
            while (target_vstart < curr_vstart) {
                target_vstart = next_vrow(target_vstart);
                ed.cur_vrow--;
            }
        }
        ed.cur_pos = target;
    }
}

void mv_down(n)
int n;
{
    static int pos, size, target, curr_vstart, target_vstart;
    size = gb_content_len();
    while (n-- > 0) {
        pos = find_eol(ed.cur_pos);
        if (pos >= size) break;
        pos++;
        target = pos_at_col(pos, ed.want_col);
        
        if (ed.cur_vrow >= 0) {
            curr_vstart = vrow_start_of(ed.cur_pos);
            target_vstart = vrow_start_of(target);
            while (curr_vstart < target_vstart) {
                curr_vstart = next_vrow(curr_vstart);
                ed.cur_vrow++;
            }
        }
        ed.cur_pos = target;
    }
}

/* Forward to start of next word. */
void mv_word_fwd(n)
int n;
{
    static int size, type, t2;
    size = gb_content_len();
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos >= size) break;
        type = iswordch(gb_char_at(ed.cur_pos)) ? 1 :
               isspacech(gb_char_at(ed.cur_pos)) ? 0 : 2;
        while (ed.cur_pos < size) {
            t2 = iswordch(gb_char_at(ed.cur_pos)) ? 1 :
                 isspacech(gb_char_at(ed.cur_pos)) ? 0 : 2;
            if (t2 != type) break;
            ed.cur_pos++;
        }
        while (ed.cur_pos < size && isspacech(gb_char_at(ed.cur_pos))
               && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
    }
    end_hmove();
}

/* Backward to start of previous word. */
void mv_word_back(n)
int n;
{
    static int type, t2;
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos == 0) break;
        ed.cur_pos--;
        while (ed.cur_pos > 0 && isspacech(gb_char_at(ed.cur_pos)))
            ed.cur_pos--;
        if (ed.cur_pos == 0) break;
        type = iswordch(gb_char_at(ed.cur_pos)) ? 1 : 2;
        while (ed.cur_pos > 0) {
            t2 = iswordch(gb_char_at(ed.cur_pos - 1)) ? 1 :
                 isspacech(gb_char_at(ed.cur_pos - 1)) ? 0 : 2;
            if (t2 != type) break;
            ed.cur_pos--;
        }
    }
    end_hmove();
}

/* Forward to end of current/next word. */
void mv_word_end(n)
int n;
{
    static int size, type, t2;
    size = gb_content_len();
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos >= size - 1) break;
        ed.cur_pos++;
        while (ed.cur_pos < size && isspacech(gb_char_at(ed.cur_pos)))
            ed.cur_pos++;
        type = iswordch(gb_char_at(ed.cur_pos)) ? 1 : 2;
        while (ed.cur_pos < size - 1) {
            t2 = iswordch(gb_char_at(ed.cur_pos + 1)) ? 1 :
                 isspacech(gb_char_at(ed.cur_pos + 1)) ? 0 : 2;
            if (t2 != type) break;
            ed.cur_pos++;
        }
    }
    end_hmove();
}

/* ------------------------------------------------------------------ */
/*  In-line find motion (f, F, ;, ,)                                    */
/* ------------------------------------------------------------------ */
void mv_find(ch, dir, count)
int ch, dir, count;
{
    static int p, sz, found;
    if (!ch) return;
    sz = gb_content_len();
    begin_hmove();
    while (count-- > 0) {
        found = -1;
        if (dir > 0) {
            p = ed.cur_pos + 1;
            while (p < sz && gb_char_at(p) != '\n') {
                if (gb_char_at(p) == ch) { found = p; break; }
                p++;
            }
        } else {
            p = ed.cur_pos - 1;
            while (p >= 0 && gb_char_at(p) != '\n') {
                if (gb_char_at(p) == ch) { found = p; break; }
                p--;
            }
        }
        if (found < 0) break;
        ed.cur_pos = found;
    }
    ed.want_col = scr_pos_col(ed.cur_pos);
    end_hmove();
}

/* ------------------------------------------------------------------ */
/*  Range helpers for operator+motion                                   */
/* ------------------------------------------------------------------ */

/*
 * Given a motion character and count, compute the endpoint.
 * For character motions: returns [cur_pos, end) (end exclusive).
 * For line motions: sets *linewise=1 and returns line-start/end.
 * Returns -1 if motion is unknown.
 */
int motion_endpoint(ch, count, linewise)
int  ch, count;
int *linewise;
{
    int pos  = ed.cur_pos;
    int size = gb_content_len();
    int n;

    *linewise = 0;

    switch (ch) {
    case 'l':
        n = count;
        while (n-- > 0 && pos < size && gb_char_at(pos) != '\n') pos++;
        return pos;

    case 'h':
        n = count;
        while (n-- > 0 && pos > 0 && gb_char_at(pos-1) != '\n') pos--;
        /* apply_op handles from>to ordering */
        return pos;

    case 'w':  /* to start of next word (exclusive) */
        n = count;
        while (n-- > 0) {
            int type = iswordch(gb_char_at(pos)) ? 1 :
                       isspacech(gb_char_at(pos)) ? 0 : 2;
            while (pos < size) {
                int t2 = iswordch(gb_char_at(pos)) ? 1 :
                         isspacech(gb_char_at(pos)) ? 0 : 2;
                if (t2 != type) break;
                pos++;
            }
            /* for dw, don't skip to next word - just the word+trailing space */
            while (pos < size && (gb_char_at(pos) == ' ' ||
                                  gb_char_at(pos) == '\t')) pos++;
        }
        return pos;

    case 'b':  /* back one word */
        n = count;
        {
                /* mirror of mv_word_back but just compute endpoint */
            while (n-- > 0) {
                if (pos == 0) break;
                pos--;
                while (pos > 0 && isspacech(gb_char_at(pos))) pos--;
                if (pos == 0) break;
                {
                    int type = iswordch(gb_char_at(pos)) ? 1 : 2;
                    while (pos > 0) {
                        int t2 = iswordch(gb_char_at(pos-1)) ? 1 :
                                 isspacech(gb_char_at(pos-1)) ? 0 : 2;
                        if (t2 != type) break;
                        pos--;
                    }
                }
            }
            /* apply_op handles from>to ordering */
        }
        return pos;

    case 'e':  /* end of word (inclusive -> +1 for exclusive) */
        n = count;
        while (n-- > 0) {
            int type;
            if (pos >= size - 1) break;
            pos++;
            while (pos < size && isspacech(gb_char_at(pos))) pos++;
            type = iswordch(gb_char_at(pos)) ? 1 : 2;
            while (pos < size - 1) {
                int t2 = iswordch(gb_char_at(pos+1)) ? 1 :
                         isspacech(gb_char_at(pos+1)) ? 0 : 2;
                if (t2 != type) break;
                pos++;
            }
        }
        return pos + 1; /* inclusive -> exclusive */

    case '$':  /* to end of line (inclusive -> +1) */
        while (pos < size && gb_char_at(pos) != '\n') pos++;
        return pos;

    case '0':  /* to start of line */
        while (pos > 0 && gb_char_at(pos-1) != '\n') pos--;
        return pos;  /* apply_op handles from>to ordering */

    case '^':  /* first non-blank */
        {
            int sol = pos;
            while (sol > 0 && gb_char_at(sol-1) != '\n') sol--;
            while (sol < size && (gb_char_at(sol)==' '||gb_char_at(sol)=='\t')) sol++;
            return sol; /* apply_op handles from>to ordering */
        }

    case 'j':  /* next line(s) -- linewise */
        *linewise = 1;
        {
            int cur_line = scr_pos_line(pos);
            int last     = scr_line_count() - 1;
            int end_line = cur_line + count;
            if (end_line > last) end_line = last;
            ed.cur_pos = scr_line_start(cur_line);
            return scr_line_start(end_line + 1); /* exclusive */
        }

    case 'k':  /* prev line(s) -- linewise */
        *linewise = 1;
        {
            int cur_line = scr_pos_line(pos);
            int start_line = cur_line - count;
            if (start_line < 0) start_line = 0;
            ed.cur_pos = scr_line_start(start_line);
            return scr_line_start(cur_line + 1);
        }

    case 'G':  /* to end of file -- linewise */
        *linewise = 1;
        {
            int cur_line = scr_pos_line(pos);
            ed.cur_pos   = scr_line_start(cur_line);
            return size; /* exclusive end = past last byte */
        }

    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Operator application                                                */
/* ------------------------------------------------------------------ */

/*
 * Apply operator op ('d','c','y') to the range [from, to).
 * If linewise, from/to are line-start positions.
 */
void apply_op(op, from, to, linewise)
int op, from, to, linewise;
{
    static int len, i, save, c, t, size;

    if (from > to) { t = from; from = to; to = t; }
    len = to - from;
    if (len <= 0) return;

    if (op == 'y') {
        save = (len >= YANK_MAX) ? YANK_MAX - 1 : len;
        for (i = 0; i < save; i++) {
            c = gb_char_at(from + i);
            ed.yank[i] = (c < 0) ? 0 : (char)c;
        }
        ed.yank[save] = '\0';
        ed.yank_len   = save;
        ed.yank_line  = linewise;
        ed.cur_pos    = from;
        ed.cur_vrow   = -1;
        sprintf(ed.status, "%d char%s yanked", save, save == 1 ? "" : "s");
        scr_show_status(ed.status);
        return;
    }

    undo_save_delete(from, len);
    save = (len >= YANK_MAX) ? YANK_MAX - 1 : len;
    for (i = 0; i < save; i++) {
        c = gb_char_at(from + i);
        ed.yank[i] = (c < 0) ? 0 : (char)c;
    }
    ed.yank[save] = '\0';
    ed.yank_len   = save;
    ed.yank_line  = linewise;
    gb_delete(from, len);
    ed.modified = 1;

    size = gb_content_len();
    ed.cur_pos = from;
    if (ed.cur_pos >= size) ed.cur_pos = size > 0 ? size - 1 : 0;
    if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos) == '\n')
        if (gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;

    scr_after_edit();

    if (op == 'c') {
        undo_save_insert(ed.cur_pos, 0);
        ed.mode = MODE_INSERT;
        scr_show_status(msg_insert);
    }
}

/* ------------------------------------------------------------------ */
/*  Search                                                              */
/* ------------------------------------------------------------------ */

/*
 * Read a search pattern from the command line.
 * Echoes to the last row as the user types.
 * Returns 1 if Enter pressed with a pattern, 0 if ESC.
 */
int read_pattern(prompt)
int prompt;  /* '/' or '?' */
{
    static int c, len;
    static char *pat;
    pat = ed.search;

    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    term_putch(prompt);
    len = 0;

    for (;;) {
        c = term_getch();
        if (c == KEY_ESC) return 0;
        if (c == KEY_CR || c == KEY_LF) {
            pat[len] = '\0';
            return (len > 0) ? 1 : 0;
        }
        if ((c == KEY_BS || c == KEY_DEL) && len > 0) {
            len--;
            term_putch(KEY_BS); term_putch(' '); term_putch(KEY_BS);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len < SEARCH_MAX - 1) {
            pat[len++] = (char)c;
            term_putch(c);
        }
    }
}

/*
 * Simple substring search: look for ed.search in the buffer
 * starting at start_pos in direction ed.search_dir.
 * Wraps around. Returns new position, or -1 if not found.
 */
int do_search_from(start_pos)
int start_pos;
{
    static int size, plen, i, j, match, dir, pos;
    size = gb_content_len();
    plen = strlen(ed.search);
    dir  = ed.search_dir;

    if (plen == 0 || size == 0) return -1;

    for (i = 1; i <= size; i++) {
        pos = (dir == SEARCH_FWD)
              ? (start_pos + i) % size
              : (start_pos - i + size) % size;
        match = 1;
        for (j = 0; j < plen && match; j++) {
            if ((pos + j) >= size || gb_char_at(pos + j) != (unsigned char)ed.search[j])
                match = 0;
        }
        if (match) return pos;
    }
    return -1;
}

