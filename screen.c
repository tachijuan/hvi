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
    /* retreat to start of logical line */
    p = pos;
    while (p > 0 && gb_char_at(p - 1) != '\n')
        p--;
    /* advance visual rows until the next one would go past pos */
    for (;;) {
        next = next_vrow(p);
        if (next > pos || next <= p) break;
        p = next;
    }
    return p;
}

/*
 * Return the display column of pos within its current visual row.
 * Used by insert_key() to detect wrap without a full redraw.
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
 * Return the display column (0-based) of pos within its logical line,
 * with tab expansion.  May exceed scr_cols for wrapped lines.
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

/*
 * Return the buffer position of the start of logical line linenum.
 */
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

    /* cursor above viewport */
    if (ed.cur_pos < ed.top_pos) {
        ed.top_pos = vrow_start_of(ed.cur_pos);
        return;
    }

    /* count visual rows from top_pos until we reach cur_pos */
    p    = ed.top_pos;
    rows = 0;
    while (p < ed.cur_pos) {
        next = next_vrow(p);
        if (next <= p) break;
        if (next > ed.cur_pos) break;   /* next row goes past cursor */
        p = next;
        rows++;
    }

    if (rows >= text_rows) {
        /* cursor below viewport: advance top_pos forward */
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
 * Computes screen row and column in terms of visual rows.
 */
void scr_update_cursor()
{
    int p, vstart, scr_row, scr_col, col, c, nc, size, next;

    size   = gb_content_len();
    vstart = vrow_start_of(ed.cur_pos);

    /* count visual rows from top_pos to the start of cursor's visual row */
    p       = ed.top_pos;
    scr_row = 0;
    while (p < vstart) {
        next = next_vrow(p);
        if (next <= p) break;
        p = next;
        scr_row++;
    }

    /* column within this visual row */
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

    /* find start of this visual row */
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
            if (nc > ed.scr_cols) break;   /* tab overflows — wrap */
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
 */
void scr_refresh()
{
    int row, text_rows;
    text_rows = ed.scr_rows - 1;
    scr_scroll_to_cursor();
    for (row = 0; row < text_rows; row++)
        scr_redraw_line(row);
    scr_update_cursor();
    scr_show_status(ed.status);
}

/*
 * Redraw from the cursor's visual row to the bottom of the text area,
 * then reposition the terminal cursor.
 *
 * Redraws more than just the cursor's row because a character insertion
 * or deletion on a long line can shift content across visual-row
 * boundaries below the cursor.
 */
void scr_redraw_cur_line()
{
    int p, vstart, scr_row, text_rows, row, next;

    vstart    = vrow_start_of(ed.cur_pos);
    p         = ed.top_pos;
    scr_row   = 0;
    text_rows = ed.scr_rows - 1;

    while (p < vstart) {
        next = next_vrow(p);
        if (next <= p) break;
        p = next;
        scr_row++;
    }

    for (row = scr_row; row < text_rows; row++)
        scr_redraw_line(row);

    scr_update_cursor();
}

/*
 * Display msg in the status line (row scr_rows-1).
 */
void scr_show_status(msg)
char *msg;
{
    char lineno[32];

    term_goto(ed.scr_rows - 1, 0);
    term_clreol();

    if (msg && *msg) {
        term_reverse();
        term_puts(msg);
        term_normal();
    } else {
        sprintf(lineno, "\"%s\"%s L%d",
            ed.filename[0] ? ed.filename : "[No Name]",
            ed.modified ? " [+]" : "",
            scr_pos_line(ed.cur_pos) + 1);
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
