/*
 * term.c - ANSI terminal I/O for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Provides cursor movement, attribute control, and input using ANSI/VT100
 * escape sequences.  Input via getch() (no echo, no line buffering).
 *
 * Performance optimisations for slow serial links (9600 baud, 4 MHz Z80):
 *
 *   Output buffer  — all output is accumulated in a 256-byte buffer and
 *     flushed as a single write just before blocking on input.  This turns
 *     many individual BDOS/BIOS calls into one, dramatically cutting CPU
 *     overhead when the terminal is baud-rate limited.
 *
 *   Cursor tracking  — s_trow / s_tcol shadow the terminal cursor so that
 *     term_goto() can emit cheap sequences (\r, \r\n) instead of the full
 *     ESC[R;CH sequence (7-10 bytes) when moving to column 0 within the
 *     text area.  A full refresh of 24 rows saves ~150 bytes this way.
 *
 *   Scroll region  — set once in term_init() to cover rows 1..(scr_rows-1)
 *     (the text area, in 1-based terminal coordinates).  term_scroll_up()
 *     and term_scroll_dn() exploit this to scroll by 1 visual row using 2-3
 *     bytes instead of repainting the entire screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpm.h>
#include "hvi.h"

extern Editor ed;

/* getch() is in the HI-TECH C runtime; bdos() and bios() come from <cpm.h> */
extern int getch();

/* ------------------------------------------------------------------ */
/*  Output buffer                                                       */
/* ------------------------------------------------------------------ */

#define OUT_BUF_SZ  256
static char s_outbuf[OUT_BUF_SZ];
static int  s_outpos = 0;

/* Tracked terminal cursor position (-1 = unknown). */
static int  s_trow = -1;
static int  s_tcol = -1;

/*
 * Write one byte to the output buffer.
 * Auto-flushes when the buffer fills; the caller is responsible for
 * a final flush before blocking on input.
 */
static void raw_byte(c)
int c;
{
    if (s_outpos >= OUT_BUF_SZ) {
        term_flush();
    }
    s_outbuf[s_outpos++] = (char)c;
}

/*
 * Write a C-string to the output buffer without updating cursor tracking.
 * Use this for escape sequences whose content is fully controlled by the
 * caller (which then sets s_trow/s_tcol explicitly).
 */
static void raw_str(s)
char *s;
{
    while (*s) raw_byte((unsigned char)*s++);
}

/*
 * Flush the output buffer to stdout.
 * Called by term_getch() before blocking; also exported so callers can
 * flush at logical checkpoints.
 */
void term_flush()
{
    int i;
    if (s_outpos > 0) {
        for (i = 0; i < s_outpos; i++) {
            bios(4, s_outbuf[i] & 0xFF, 0);
        }
        s_outpos = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Public output primitives                                            */
/* ------------------------------------------------------------------ */

/*
 * Output a single character and update cursor tracking.
 * Visible chars (0x20-0x7E) advance the column by 1.
 * \r resets the column to 0.
 * \n increments the row and resets the column.
 * Any other byte (ESC etc.) invalidates tracking because its effect on
 * cursor position depends on the full escape sequence.
 */
void term_putch(c)
int c;
{
    raw_byte(c);
    if (c >= 0x20 && c != 0x7F) {
        if (s_tcol >= 0) s_tcol++;
    } else if (c == '\r') {
        s_tcol = 0;
    } else if (c == '\n') {
        if (s_trow >= 0) s_trow++;
        s_tcol = 0;
    } else {
        /* Control char other than CR/LF — lose tracking */
        s_trow = -1;
        s_tcol = -1;
    }
}

/* Output a null-terminated string, updating cursor tracking per char. */
void term_puts(s)
char *s;
{
    while (*s)
        term_putch((unsigned char)*s++);
}

/*
 * Clear from cursor to end of current line (ESC[K).
 * The terminal cursor does NOT move; do not invalidate tracking.
 */
void term_clreol()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('K');
}

/* Set bold video attribute (cursor doesn't move). */
void term_bold()
{
    raw_str("\033[1m");
}

/* Set reverse video attribute (cursor doesn't move). */
void term_reverse()
{
    raw_str("\033[7m");
}

/* Reset all video attributes (cursor doesn't move). */
void term_normal()
{
    raw_str("\033[0m");
}

/* ------------------------------------------------------------------ */
/*  Cursor positioning                                                  */
/* ------------------------------------------------------------------ */

/*
 * Move the terminal cursor to (row, col), both 0-based.
 *
 * Optimisations (only when cursor position is known):
 *
 *   Same position        — no-op.
 *   Same row, col == 0   — emit \r (1 byte).
 *   col == 0, row N down — emit \r + N newlines (2+N bytes) when the
 *     destination is within the text area (row <= scr_rows-2).  Within the
 *     scroll region a newline at any row except the bottom margin simply
 *     moves the cursor down; it does NOT scroll the screen.
 *
 * All other cases fall back to the full ANSI CSI sequence.
 */
void term_goto(row, col)
int row, col;
{
    char buf[16];
    int  dr, i;

    /* No-op when already there. */
    if (row == s_trow && col == s_tcol) return;

    if (s_trow >= 0 && s_tcol >= 0) {
        dr = row - s_trow;

        /* Same row */
        if (dr == 0) {
            int dc = col - s_tcol;

            if (col == 0) {
                raw_byte('\r');
                s_tcol = 0;
                return;
            }

            /* Move left using backspace (1 byte per col) */
            if (dc < 0 && dc >= -6) {
                for (i = 0; i < -dc; i++) raw_byte('\b');
                s_tcol = col;
                return;
            }

            /* Move right using ESC [ C (3 bytes per col)
               Break-even compared to 8-byte ESC [ r ; c H is dc <= 2 */
            if (dc > 0 && dc <= 2) {
                for (i = 0; i < dc; i++) {
                    raw_byte(0x1B); raw_byte('['); raw_byte('C');
                }
                s_tcol = col;
                return;
            }
        }

        /*
         * Move to column 0 on a lower row within the text area.
         * Emitting N newlines starting from s_trow is safe as long as we
         * never land on the bottom margin (scr_rows-2) mid-sequence:
         * that is guaranteed by requiring row <= scr_rows-2.
         * Limit to dr<=5 (6 bytes vs 7+ for full CSI — break-even favours
         * this path).
         */
        if (col == 0 && dr > 0 && dr <= 5 && row <= ed.scr_rows - 2) {
            if (s_tcol != 0) raw_byte('\r');
            for (i = 0; i < dr; i++) raw_byte('\n');
            s_trow = row;
            s_tcol = 0;
            return;
        }
    }

    /* Full ANSI cursor-address sequence. */
    sprintf(buf, "\033[%d;%dH", row + 1, col + 1);
    raw_str(buf);
    s_trow = row;
    s_tcol = col;
}

/* ------------------------------------------------------------------ */
/*  Terminal scrolling                                                  */
/* ------------------------------------------------------------------ */

/*
 * Scroll the text area up by 1 visual row (content moves up, blank line
 * appears at the bottom of the text area).
 *
 * Technique: position at the last text row (which is the bottom margin of
 * the scroll region set in term_init) and emit a newline.  Within the
 * scroll region this scrolls the region up without touching the status
 * line.  3-10 bytes depending on current cursor position vs ~1200 bytes
 * for a full screen repaint.
 */
void term_scroll_up()
{
    term_goto(ed.scr_rows - 2, 0);
    raw_byte('\n');
    /* Cursor stays at the last text row after the scroll. */
    s_trow = ed.scr_rows - 2;
    s_tcol = 0;
}

/*
 * Scroll the text area down by 1 visual row (content moves down, blank
 * line appears at the top of the text area).
 *
 * Technique: position at row 0 (top margin of scroll region) and emit
 * ESC M (Reverse Index).  The scroll region scrolls down; cursor stays
 * at row 0.
 */
void term_scroll_dn()
{
    term_goto(0, 0);
    raw_byte(0x1B);
    raw_byte('M');   /* Reverse Index */
    s_trow = 0;
    s_tcol = 0;
}

/* Insert a blank character at the current cursor position */
void term_ins_char()
{
    raw_str("\033[@");
}

/* Delete character at current cursor position */
void term_del_char()
{
    raw_str("\033[P");
}

/* ------------------------------------------------------------------ */
/*  Terminal lifecycle                                                  */
/* ------------------------------------------------------------------ */

/*
 * Initialise the terminal:
 *   1. Query size (or use defaults).
 *   2. Clear the screen.
 *   3. Set the scroll region to the text area rows 1..(scr_rows-1) so that
 *      term_scroll_up/dn() work without disturbing the status line.
 */
void term_init()
{
    char buf[16];
    ed.scr_rows = DEF_ROWS;
    ed.scr_cols = DEF_COLS;
    term_getsize(&ed.scr_rows, &ed.scr_cols);
    term_clear();
    /* Set scroll region: rows 1..(scr_rows-1) in 1-based ANSI coordinates
     * = rows 0..(scr_rows-2) in 0-based editor coordinates (the text area). */
    sprintf(buf, "\033[1;%dr", ed.scr_rows - 1);
    raw_str(buf);
    s_trow = -1;
    s_tcol = -1;
    term_flush();
}

/*
 * Restore terminal on exit: reset the scroll region to full screen so the
 * shell's output is not constrained, then clear the status line.
 */
void term_restore()
{
    raw_str("\033[r");   /* reset scroll region to full screen */
    term_normal();
    /* Invalidate tracking so term_goto emits an unconditional move.
     * The tracking state may be stale after reading the :q keystroke. */
    s_trow = -1; s_tcol = -1;
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    raw_byte('\n');      /* leave cursor at bottom, below editor content */
    term_flush();
}

/*
 * Clear the entire screen and home the cursor.
 * Does NOT touch the scroll region — term_init() sets it; term_restore()
 * clears it.  Callers (e.g. KEY_CTRL_L) can call this freely.
 */
void term_clear()
{
    raw_str("\033[2J\033[H");
    s_trow = 0;
    s_tcol = 0;
    term_flush();
}

/* ------------------------------------------------------------------ */
/*  Input                                                               */
/* ------------------------------------------------------------------ */

/*
 * Read one raw keypress.  Flushes all pending output first so the screen
 * is fully updated before we block.
 *
 * ANSI arrow key sequences (ESC [ A/B/C/D) are decoded and returned as
 * synthetic KEY_UP/DOWN/LEFT/RIGHT codes (>0xFF) so callers do not need
 * to track multi-byte state.  A bare ESC or an unrecognised sequence
 * returns KEY_ESC.
 */
int term_getch()
{
    int c, c2, wait;
    term_flush();
    c = bios(3, 0, 0) & 0xFF;

    if (c == KEY_ESC) {
        /* Quick poll for '[' — if nothing arrives promptly it is a bare ESC. */
        wait = 8000;
        while (bios(2, 0, 0) == 0) {
            if (--wait == 0) return KEY_ESC;
        }
        c2 = (int)(unsigned char)bios(3, 0, 0);
        if (c2 != '[') return KEY_ESC;

        /* Read the direction letter. */
        wait = 8000;
        while (bios(2, 0, 0) == 0) {
            if (--wait == 0) return KEY_ESC;
        }
        c2 = (int)(unsigned char)bios(3, 0, 0);
        switch (c2) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:  return KEY_ESC;
        }
    }
    return c;
}

/* ------------------------------------------------------------------ */
/*  Terminal size query                                                 */
/* ------------------------------------------------------------------ */

/*
 * Query terminal dimensions using the ANSI cursor-position report.
 * See the original comment in the earlier version for the full explanation
 * of the canonical-mode / BIOS-CONIN rationale.
 */
void term_getsize(rows, cols)
int *rows;
int *cols;
{
    int  c, r, co, state, i, wait, total;
    char buf[32];
    char *p;

    *rows = DEF_ROWS;
    *cols = DEF_COLS;

    term_puts("\033[999;999H\033[6n");
    term_flush();

    wait = 30000;
    while (bios(2, 0, 0) == 0) {
        if (--wait == 0) {
            return;
        }
    }

    i = 0;
    state = 0;
    for (total = 0; total < 48 && i < (int)(sizeof(buf) - 1); total++) {
        c = (int)(unsigned char)bios(3, 0, 0);

        switch (state) {
        case 0:
            if (c == 0x1B) { buf[i++] = (char)c; state = 1; }
            break;
        case 1:
            if (c == '[') {
                buf[i++] = (char)c; state = 2;
            } else if (c == 0x1B) {
                i = 0; buf[i++] = (char)c;
            } else {
                i = 0; state = 0;
            }
            break;
        case 2:
            if (c == 'R') {
                buf[i++] = (char)c; state = 3;
            } else if ((c >= '0' && c <= '9') || c == ';') {
                buf[i++] = (char)c;
            } else if (c == 0x1B) {
                i = 0; buf[i++] = (char)c; state = 1;
            } else {
                i = 0; state = 0;
            }
            break;
        }

        if (state == 3) break;
    }
    buf[i] = '\0';

    if (state == 3) {
        p = buf;
        r = co = 0;
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') r  = r  * 10 + (*p++ - '0');
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') co = co * 10 + (*p++ - '0');
        if (r > 0 && co > 0) { *rows = r; *cols = co; }
    }
}
