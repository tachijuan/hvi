/*
 * screen.c - Screen rendering for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Manages the text viewport and status line.
 * The text area occupies rows 0..(scr_rows-2).
 * Row (scr_rows-1) is the status/command line.
 *
 * Lines wider than the terminal wrap to the next screen row.  The unit
 * of vertical measurement throughout this file is the "visual row": one
 * terminal line's worth of content (scr_cols display columns, or fewer
 * when a newline ends the logical line sooner).  top_pos may point to
 * the middle of a long logical line (the start of any visual row).
 *
 * Performance notes (9600 baud, 4 MHz Z80):
 *   scr_update_after_move() uses term_scroll_up/dn() to repaint just
 *   one new line (~53 bytes) instead of a full screen (~1200 bytes)
 *   when the viewport shifts by exactly 1 visual row.
 *
 *   scr_cur_line() maintains an incremental cache of the current line
 *   number so that the status bar update and movement routines avoid
 *   the O(buffer) scan of scr_pos_line(cur_pos) on every keystroke.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hvi.h"

extern Editor ed;

/* ------------------------------------------------------------------ */
/*  Visual-row helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * Advance one visual row from buffer position 'from'.
 * Returns the buffer position that starts the NEXT visual row:
 *   - the character after '\n' if the logical line ends first, or
 *   - the first character that would overflow column scr_cols.
 */
static int next_vrow(from)
int from;
{
    int col = 0, c, nc, size;
    size = gb_content_len();
    while (from < size) {
        c = gb_char_at(from);
        if (c == '\n') return from + 1;
        nc = (c == '\t') ? (col / TAB_STOP + 1) * TAB_STOP : col + 1;
        if (nc > ed.scr_cols) return from;
        col = nc;
        from++;
    }
    return from;
}

/*
 * Return the buffer position at the START of the visual row that
 * contains 'pos'.  Searches forward from the logical line start.
 */
static int vrow_start_of(pos)
int pos;
{
    int p, next;
    p = pos;
    while (p > 0 && gb_char_at(p - 1) != '\n')
        p--;
    for (;;) {
        next = next_vrow(p);
        if (next > pos || next <= p) break;
        p = next;
    }
    return p;
}

/*
 * Return the display column of pos within its current visual row.
 */
int scr_vrow_col(pos)
int pos;
{
    int p, vstart, col, c, nc, size;
    size   = gb_content_len();
    vstart = vrow_start_of(pos);
    col = 0;
    p   = vstart;
    while (p < pos && p < size) {
        c  = gb_char_at(p);
        nc = (c == '\t') ? (col / TAB_STOP + 1) * TAB_STOP : col + 1;
        col = nc;
        p++;
    }
    return col;
}

/* ------------------------------------------------------------------ */
/*  Logical-position helpers                                            */
/* ------------------------------------------------------------------ */

/*
 * Return the line number (0-based) of buffer position pos.
 * O(pos) scan — use scr_cur_line() for the current cursor position.
 */
int scr_pos_line(pos)
int pos;
{
    int i, line;
    line = 0;
    for (i = 0; i < pos; i++)
        if (gb_char_at(i) == '\n')
            line++;
    return line;
}

/*
 * Cached, incremental line number for ed.cur_pos.
 *
 * On the first call after initialisation (cur_line_pos == -1) a full
 * scr_pos_line() scan is done once.  Subsequent calls update the cache
 * incrementally: for a forward move scan only [old_pos, new_pos); for a
 * backward move scan [new_pos, old_pos) and subtract.  For typical
 * single-line movement (j/k) this is O(line_length) instead of O(buffer).
 *
 * The cache is invalidated (cur_line_pos set to -1) by any operation
 * that changes cur_pos non-incrementally (G, gg, search, undo, put, …).
 * Those callers call scr_cur_line() once afterwards to rebuild the cache.
 */
int scr_cur_line()
{
    int pos, old_pos, old_line, i;

    pos = ed.cur_pos;

    if (ed.cur_line_pos == pos)
        return ed.cur_line;

    old_pos  = ed.cur_line_pos;
    old_line = ed.cur_line;

    if (old_pos >= 0 && old_line >= 0) {
        if (pos > old_pos) {
            for (i = old_pos; i < pos; i++)
                if (gb_char_at(i) == '\n') old_line++;
        } else {
            for (i = pos; i < old_pos; i++)
                if (gb_char_at(i) == '\n') old_line--;
            if (old_line < 0) old_line = 0;
        }
        ed.cur_line = old_line;
    } else {
        ed.cur_line = scr_pos_line(pos);
    }
    ed.cur_line_pos = pos;
    return ed.cur_line;
}

/*
 * Return the display column (0-based) of pos within its logical line.
 */
int scr_pos_col(pos)
int pos;
{
    int col, i, start;
    start = pos;
    while (start > 0 && gb_char_at(start - 1) != '\n')
        start--;
    col = 0;
    for (i = start; i < pos; i++) {
        int c = gb_char_at(i);
        if (c == '\t')
            col = (col / TAB_STOP + 1) * TAB_STOP;
        else
            col++;
    }
    return col;
}

/* Return the buffer position of the start of logical line linenum. */
int scr_line_start(linenum)
int linenum;
{
    int pos, line, size;
    pos  = 0;
    line = 0;
    size = gb_content_len();
    while (pos < size && line < linenum) {
        if (gb_char_at(pos) == '\n')
            line++;
        pos++;
    }
    return pos;
}

/*
 * Return the buffer position of the last char on the same line as pos
 * (not including the newline).
 */
int scr_line_end(pos)
int pos;
{
    int size = gb_content_len();
    while (pos < size && gb_char_at(pos) != '\n')
        pos++;
    if (pos > 0 && (pos >= size || gb_char_at(pos) == '\n')) {
        if (pos > 0 && gb_char_at(pos - 1) != '\n')
            pos--;
    }
    return pos;
}

/* Return total number of logical lines in the buffer. */
int scr_line_count()
{
    int i, size, lines;
    size = gb_content_len();
    if (size == 0) return 1;
    lines = 0;
    for (i = 0; i < size; i++)
        if (gb_char_at(i) == '\n')
            lines++;
    if (gb_char_at(size - 1) != '\n')
        lines++;
    return (lines > 0) ? lines : 1;
}

/* ------------------------------------------------------------------ */
/*  Scroll / cursor placement                                           */
/* ------------------------------------------------------------------ */

/*
 * Ensure the cursor's visual row is within the viewport.
 * Adjusts top_pos (which may land mid-line for wrapped content).
 */
void scr_scroll_to_cursor()
{
    int p, rows, advance, text_rows, next;

    text_rows = ed.scr_rows - 1;

    if (ed.cur_pos < ed.top_pos) {
        ed.top_pos = vrow_start_of(ed.cur_pos);
        return;
    }

    p    = ed.top_pos;
    rows = 0;
    while (p < ed.cur_pos) {
        next = next_vrow(p);
        if (next <= p) break;
        if (next > ed.cur_pos) break;
        p = next;
        rows++;
    }

    if (rows >= text_rows) {
        advance = rows - text_rows + 1;
        p = ed.top_pos;
        while (advance-- > 0) {
            next = next_vrow(p);
            if (next <= p) break;
            p = next;
        }
        ed.top_pos = p;
    }
}

/*
 * Move the terminal cursor to match the editor cursor position.
 */
void scr_update_cursor()
{
    int p, vstart, scr_row, scr_col, col, c, nc, size, next;

    size   = gb_content_len();
    vstart = vrow_start_of(ed.cur_pos);

    p       = ed.top_pos;
    scr_row = 0;
    while (p < vstart) {
        next = next_vrow(p);
        if (next <= p) break;
        p = next;
        scr_row++;
    }

    col = 0;
    p   = vstart;
    while (p < ed.cur_pos && p < size) {
        c  = gb_char_at(p);
        nc = (c == '\t') ? (col / TAB_STOP + 1) * TAB_STOP : col + 1;
        col = nc;
        p++;
    }
    scr_col = col;

    if (scr_col >= ed.scr_cols)      scr_col = ed.scr_cols - 1;
    if (scr_row < 0)                 scr_row = 0;
    if (scr_row >= ed.scr_rows - 1) scr_row = ed.scr_rows - 2;

    term_goto(scr_row, scr_col);
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                           */
/* ------------------------------------------------------------------ */

/*
 * Draw one terminal row (0-based index within the text area).
 * Advances 'screen_row' visual rows from top_pos to find the content.
 */
void scr_redraw_line(screen_row)
int screen_row;
{
    int pos, r, col, size, c, nc, next;

    pos = ed.top_pos;
    for (r = 0; r < screen_row; r++) {
        next = next_vrow(pos);
        if (next <= pos) break;
        pos = next;
    }

    size = gb_content_len();
    term_goto(screen_row, 0);
    term_clreol();

    if (pos >= size) {
        if (screen_row > 0) term_putch('~');
        return;
    }

    col = 0;
    while (pos < size && col < ed.scr_cols) {
        c = gb_char_at(pos);
        if (c == '\n') break;
        if (c == '\t') {
            nc = (col / TAB_STOP + 1) * TAB_STOP;
            if (nc > ed.scr_cols) break;
            while (col < nc) { term_putch(' '); col++; }
        } else {
            term_putch(c);
            col++;
        }
        pos++;
    }
}

/*
 * Full screen refresh: redraws all text rows and the status line.
 * Sequential rows use \r\n (2 bytes) instead of a full ESC[R;CH goto
 * (7-10 bytes) — term_goto() handles this automatically via cursor
 * tracking.
 */
void scr_refresh()
{
    int row, text_rows;
    text_rows = ed.scr_rows - 1;
    scr_scroll_to_cursor();
    for (row = 0; row < text_rows; row++)
        scr_redraw_line(row);
    scr_show_status(ed.status);
}

/*
 * Redraw only the visual rows that belong to the current logical line,
 * plus one extra row after the line ends (to clear any row freed by a
 * deletion that shortened the line).  Much cheaper than redrawing the
 * entire area below the cursor for single-line edits (x, X, D, ~, …).
 */
void scr_redraw_cur_line()
{
    int p, vstart, scr_row, text_rows, row, next, size;

    vstart    = vrow_start_of(ed.cur_pos);
    p         = ed.top_pos;
    scr_row   = 0;
    text_rows = ed.scr_rows - 1;
    size      = gb_content_len();

    while (p < vstart) {
        next = next_vrow(p);
        if (next <= p) break;
        p = next;
        scr_row++;
    }

    p   = vstart;
    row = scr_row;
    for (;;) {
        if (row >= text_rows) break;
        scr_redraw_line(row);
        row++;
        next = next_vrow(p);
        /* Stop at end of buffer or end of this logical line */
        if (next <= p || next >= size || gb_char_at(next - 1) == '\n') {
            if (row < text_rows) scr_redraw_line(row); /* 1 extra: clears freed row */
            break;
        }
        p = next;
    }

    scr_update_cursor();
}

/*
 * Redraw only the single visual row that contains the cursor.
 * Use this for in-place replacements (r, ~) where the character count
 * does not change — just one terminal row needs to be refreshed.
 */
void scr_redraw_cur_vrow()
{
    int p, vstart, scr_row, next;

    vstart  = vrow_start_of(ed.cur_pos);
    p       = ed.top_pos;
    scr_row = 0;

    while (p < vstart) {
        next = next_vrow(p);
        if (next <= p) break;
        p = next;
        scr_row++;
    }

    scr_redraw_line(scr_row);
    scr_update_cursor();
}

/* ------------------------------------------------------------------ */
/*  Smart scroll after cursor movement                                  */
/* ------------------------------------------------------------------ */

/*
 * After a movement command that may have changed top_pos, decide the
 * cheapest way to update the display.
 *
 *   No scroll    — top_pos unchanged: only reposition the terminal cursor.
 *   ±1 vrow      — use terminal scroll (term_scroll_up/dn) + redraw one
 *                  new line.  ~53 bytes vs ~1200 for a full refresh.
 *   Larger jump  — fall back to full scr_refresh().
 *
 * Pass the value of top_pos that was saved BEFORE calling
 * scr_scroll_to_cursor().
 */
void scr_update_after_move(old_top)
int old_top;
{
    int text_rows, p, new_top, delta, nx;

    text_rows = ed.scr_rows - 1;
    new_top   = ed.top_pos;

    if (new_top == old_top) {
        scr_show_status(ed.status);
        return;
    }

    delta = 0;
    if (new_top > old_top) {
        p = old_top;
        while (p < new_top && delta < 2) {
            nx = next_vrow(p);
            if (nx <= p) break;
            p = nx;
            delta++;
        }
        if (delta == 1 && p == new_top) {
            term_scroll_up();
            scr_redraw_line(text_rows - 1);
            scr_show_status(ed.status);
            return;
        }
    } else {
        p = new_top;
        while (p < old_top && delta < 2) {
            nx = next_vrow(p);
            if (nx <= p) break;
            p = nx;
            delta++;
        }
        if (delta == 1 && p == old_top) {
            term_scroll_dn();
            scr_redraw_line(0);
            scr_show_status(ed.status);
            return;
        }
    }

    scr_refresh();
}

/* ------------------------------------------------------------------ */
/*  Status line                                                         */
/* ------------------------------------------------------------------ */

/*
 * Display msg in the status line (row scr_rows-1).
 * If msg is empty or NULL, show the default:
 *   "filename" [+] L<cur>/<total>
 * When the file is larger than the buffer (tail_offset > 0), the total
 * is the count of lines currently in memory followed by '+' to indicate
 * that more content exists beyond what has been loaded.
 * Uses scr_cur_line() (O(1) when cache is warm) for the line number.
 */
void scr_show_status(msg)
char *msg;
{
    char lineno[48];
    int  cur, total;

    term_goto(ed.scr_rows - 1, 0);
    term_clreol();

    if (msg && *msg) {
        term_reverse();
        term_puts(msg);
        term_normal();
    } else {
        cur   = scr_cur_line() + 1;
        total = scr_line_count();
        if (ed.tail_offset > 0L)
            sprintf(lineno, "\"%s\"%s L%d/%d+",
                ed.filename[0] ? ed.filename : "[No Name]",
                ed.modified ? " [+]" : "",
                cur, total);
        else
            sprintf(lineno, "\"%s\"%s L%d/%d",
                ed.filename[0] ? ed.filename : "[No Name]",
                ed.modified ? " [+]" : "",
                cur, total);
        term_reverse();
        term_puts(lineno);
        term_normal();
    }
    scr_update_cursor();
}

/* Clear the status line and return cursor to edit area. */
void scr_clear_status()
{
    ed.status[0] = '\0';
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    scr_update_cursor();
}
