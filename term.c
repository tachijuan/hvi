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
 *   Output buffer  -- all output is accumulated in a 256-byte buffer and
 *     flushed as a single write just before blocking on input.  This turns
 *     many individual BDOS/BIOS calls into one, dramatically cutting CPU
 *     overhead when the terminal is baud-rate limited.
 *
 *   Cursor tracking  -- s_trow / s_tcol shadow the terminal cursor so that
 *     term_goto() can emit cheap sequences (\r, \r\n) instead of the full
 *     ESC[R;CH sequence (7-10 bytes) when moving to column 0 within the
 *     text area.  A full refresh of 24 rows saves ~150 bytes this way.
 *
 *   Scroll region  -- set once in term_init() to cover rows 1..(scr_rows-1)
 *     (the text area, in 1-based terminal coordinates).  term_scroll_up()
 *     and term_scroll_dn() exploit this to scroll by 1 visual row using 2-3
 *     bytes instead of repainting the entire screen.
 *
 * No standard library headers are included; hvi_sprintf from util.c
 * is used for ANSI escape-sequence formatting.
 */

#include <cpm.h>
#include "hvi.h"

extern Editor ed;

extern int bdos_disk();   /* IX-safe BDOS wrapper (cstart.as) */

/* ------------------------------------------------------------------ */
/*  Output buffer                                                       */
/* ------------------------------------------------------------------ */

#define OUT_BUF_SZ  256
static char s_outbuf[OUT_BUF_SZ];
static int  s_outpos;
static int  s_flush_i;  /* loop counter for term_flush -- static to avoid IX-relative locals */
static int  s_rn_i, s_rn_d;        /* raw_num statics */
static int  s_rn_pows[5];          /* powers table -- static so no IX-frame overhead */
static int  s_rn_started;
static int  s_tg_dr, s_tg_i;       /* term_goto statics -- avoid IX-relative locals */

/* Tracked terminal cursor position (-1 = unknown, set by term_init). */
static int  s_trow;
static int  s_tcol;

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


/* Output the decimal representation of n (>= 0) via raw_byte.
 * Uses only static variables so no IX-relative locals are needed. */
static void raw_num(n)
int n;
{
    s_rn_pows[0] = 10000;
    s_rn_pows[1] =  1000;
    s_rn_pows[2] =   100;
    s_rn_pows[3] =    10;
    s_rn_pows[4] =     1;
    if (n <= 0) { raw_byte('0'); return; }
    s_rn_started = 0;
    for (s_rn_i = 0; s_rn_i < 5; s_rn_i++) {
        s_rn_d = 0;
        while (n >= s_rn_pows[s_rn_i]) { n -= s_rn_pows[s_rn_i]; s_rn_d++; }
        if (s_rn_d || s_rn_started) {
            raw_byte('0' + s_rn_d);
            s_rn_started = 1;
        }
    }
}

/*
 * Flush the output buffer to stdout.
 * Called by term_getch() before blocking; also exported so callers can
 * flush at logical checkpoints.
 */
void term_flush()
{
    if (s_outpos > 0) {
        s_flush_i = 0;
        while (s_flush_i < s_outpos) {
            bdos_disk(2, (int)(unsigned char)s_outbuf[s_flush_i]);
            s_flush_i++;
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
        /* Control char other than CR/LF -- lose tracking */
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
    raw_byte(0x1B); raw_byte('['); raw_byte('1'); raw_byte('m');
}

/* Set reverse video attribute (cursor doesn't move). */
void term_reverse()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('7'); raw_byte('m');
}

/* Reset all video attributes (cursor doesn't move). */
void term_normal()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('0'); raw_byte('m');
}

/* ------------------------------------------------------------------ */
/*  Cursor positioning                                                  */
/* ------------------------------------------------------------------ */

/*
 * Move the terminal cursor to (row, col), both 0-based.
 *
 * Optimisations (only when cursor position is known):
 *
 *   Same position        -- no-op.
 *   Same row, col == 0   -- emit \r (1 byte).
 *   col == 0, row N down -- emit \r + N newlines (2+N bytes) when the
 *     destination is within the text area (row <= scr_rows-2).
 *
 * All other cases fall back to the full ANSI CSI sequence.
 */
void term_goto(row, col)
int row, col;
{
    /* No-op when already there. */
    if (row == s_trow && col == s_tcol) return;

    if (s_trow >= 0 && s_tcol >= 0) {
        s_tg_dr = row - s_trow;

        /* Same row */
        if (s_tg_dr == 0) {
            if (col == 0) {
                raw_byte('\r');
                s_tcol = 0;
                return;
            }

            /* Move left using backspace (1 byte per col) */
            if (col < s_tcol && (s_tcol - col) <= 6) {
                s_tg_i = s_tcol - col;
                while (s_tg_i > 0) { raw_byte('\b'); s_tg_i--; }
                s_tcol = col;
                return;
            }

            /* Move right using ESC [ C (3 bytes per col) */
            if (col > s_tcol && (col - s_tcol) <= 2) {
                s_tg_i = col - s_tcol;
                while (s_tg_i > 0) {
                    raw_byte(0x1B); raw_byte('['); raw_byte('C');
                    s_tg_i--;
                }
                s_tcol = col;
                return;
            }
        }

        /*
         * Move to column 0 on a lower row within the text area.
         */
        if (col == 0 && s_tg_dr > 0 && s_tg_dr <= 5 && row <= ed.scr_rows - 2) {
            if (s_tcol != 0) raw_byte('\r');
            s_tg_i = s_tg_dr;
            while (s_tg_i > 0) { raw_byte('\n'); s_tg_i--; }
            s_trow = row;
            s_tcol = 0;
            return;
        }
    }

    /* Full ANSI cursor-address sequence: ESC [ row+1 ; col+1 H */
    raw_byte(0x1B); raw_byte('[');
    raw_num(row + 1);
    raw_byte(';');
    raw_num(col + 1);
    raw_byte('H');
    s_trow = row;
    s_tcol = col;
}

/* ------------------------------------------------------------------ */
/*  Terminal scrolling                                                  */
/* ------------------------------------------------------------------ */

void term_scroll_up()
{
    term_goto(ed.scr_rows - 2, 0);
    raw_byte('\n');
    s_trow = ed.scr_rows - 2;
    s_tcol = 0;
}

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
    raw_byte(0x1B); raw_byte('['); raw_byte('@');
}

/* Delete character at current cursor position */
void term_del_char()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('P');
}

/* ------------------------------------------------------------------ */
/*  Terminal lifecycle                                                  */
/* ------------------------------------------------------------------ */

void term_init()
{
    ed.scr_rows = DEF_ROWS;
    ed.scr_cols = DEF_COLS;
    term_getsize(&ed.scr_rows, &ed.scr_cols);
    term_clear();
    /* Set scroll region: ESC [ 1 ; (scr_rows-1) r */
    raw_byte(0x1B); raw_byte('['); raw_byte('1'); raw_byte(';');
    raw_num(ed.scr_rows - 1);
    raw_byte('r');
    s_trow = -1;
    s_tcol = -1;
    term_flush();
}

void term_restore()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('r'); /* reset scroll region */
    term_normal();
    s_trow = -1; s_tcol = -1;
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    raw_byte('\n');
    term_flush();
}

/*
 * Clear the entire screen and home the cursor.
 */
void term_clear()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('2'); raw_byte('J');
    raw_byte(0x1B); raw_byte('['); raw_byte('H');
    s_trow = 0;
    s_tcol = 0;
    term_flush();
}

/* ------------------------------------------------------------------ */
/*  Input                                                               */
/* ------------------------------------------------------------------ */

/* c, c2, wait are static: bios(3,0,0) may not preserve IX on CP/M,
 * so IX-relative locals would be corrupted after the BIOS CONIN call. */
static int s_tgc_c, s_tgc_c2, s_tgc_wait;

int term_getch()
{
    term_flush();

    /* BDOS 6, 0xFF = Direct Console Input (no echo, non-blocking).
     * Poll via BDOS 11 (Console Status) first to reduce spin time, then read.
     * Both go through bdos_disk to preserve IX around CALL 5. */
    while (bdos_disk(11, 0) == 0) ;            /* wait for key available */
    s_tgc_c = bdos_disk(6, 0xFF) & 0xFF;

    if (s_tgc_c == KEY_ESC) {
        s_tgc_wait = 8000;
        while (bdos_disk(11, 0) == 0) {
            if (--s_tgc_wait == 0) return KEY_ESC;
        }
        s_tgc_c2 = bdos_disk(6, 0xFF) & 0xFF;
        if (s_tgc_c2 != '[') return KEY_ESC;

        s_tgc_wait = 8000;
        while (bdos_disk(11, 0) == 0) {
            if (--s_tgc_wait == 0) return KEY_ESC;
        }
        s_tgc_c2 = bdos_disk(6, 0xFF) & 0xFF;
        switch (s_tgc_c2) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:  return KEY_ESC;
        }
    }
    return s_tgc_c;
}

/* ------------------------------------------------------------------ */
/*  Terminal size query                                                 */
/* ------------------------------------------------------------------ */

/* Statics for term_getsize -- all static so no IX-relative frame is needed
 * during the BDOS console-status polling loop. */
static int *s_tgs_rp;       /* cached pointer to caller's rows int */
static int *s_tgs_cp;       /* cached pointer to caller's cols int */
static int  s_tgs_r;        /* parsed row count from response       */
static int  s_tgs_c;        /* parsed col count from response       */
static int  s_tgs_ch;       /* character read from console          */
static int  s_tgs_n;        /* digit accumulator                    */
static int  s_tgs_wait;     /* timeout countdown                    */

/*
 * Query terminal dimensions via ANSI cursor-position report.
 *
 * Sequence:
 *   1. ESC[999;999H  -- move cursor to impossibly large position;
 *                       terminal clamps to last row/col.
 *   2. ESC[6n        -- report cursor position.
 *   3. Read ESC[r;cR -- parse rows and cols.
 *
 * Uses bdos_disk(2/6/11) exclusively (never BIOS CONIN) to preserve IX
 * across every I/O call.  Each character read is guarded by a countdown
 * so that a non-responding terminal falls through to DEF_ROWS/DEF_COLS.
 *
 * Digit multiply uses shifts to avoid linking __mulu:
 *   n * 10  ==  (n << 3) + (n << 1)
 */
void term_getsize(rows, cols)
int *rows;
int *cols;
{
    s_tgs_rp = rows;          /* cache params before any BDOS call */
    s_tgs_cp = cols;
    s_tgs_r  = DEF_ROWS;     /* default in case of timeout */
    s_tgs_c  = DEF_COLS;

    /* Step 1: move cursor to 999;999 */
    bdos_disk(2, 0x1B); bdos_disk(2, '[');
    bdos_disk(2, '9'); bdos_disk(2, '9'); bdos_disk(2, '9');
    bdos_disk(2, ';');
    bdos_disk(2, '9'); bdos_disk(2, '9'); bdos_disk(2, '9');
    bdos_disk(2, 'H');

    /* Step 2: request cursor position */
    bdos_disk(2, 0x1B); bdos_disk(2, '[');
    bdos_disk(2, '6'); bdos_disk(2, 'n');

    /* Step 3a: wait for ESC */
    s_tgs_wait = 30000;
    while (bdos_disk(11, 0) == 0) { if (--s_tgs_wait == 0) goto tgs_done; }
    s_tgs_ch = bdos_disk(6, 0xFF) & 0xFF;
    if (s_tgs_ch != 0x1B) goto tgs_done;

    /* Step 3b: wait for '[' */
    s_tgs_wait = 8000;
    while (bdos_disk(11, 0) == 0) { if (--s_tgs_wait == 0) goto tgs_done; }
    s_tgs_ch = bdos_disk(6, 0xFF) & 0xFF;
    if (s_tgs_ch != '[') goto tgs_done;

    /* Step 3c: read row digits terminated by ';' */
    s_tgs_n = 0;
    for (;;) {
        s_tgs_wait = 8000;
        while (bdos_disk(11, 0) == 0) { if (--s_tgs_wait == 0) goto tgs_done; }
        s_tgs_ch = bdos_disk(6, 0xFF) & 0xFF;
        if (s_tgs_ch >= '0' && s_tgs_ch <= '9') {
            s_tgs_n = (s_tgs_n << 3) + (s_tgs_n << 1) + (s_tgs_ch - '0');
        } else if (s_tgs_ch == ';') {
            if (s_tgs_n > 0) s_tgs_r = s_tgs_n;
            break;
        } else {
            goto tgs_done;
        }
    }

    /* Step 3d: read col digits terminated by 'R' */
    s_tgs_n = 0;
    for (;;) {
        s_tgs_wait = 8000;
        while (bdos_disk(11, 0) == 0) { if (--s_tgs_wait == 0) goto tgs_done; }
        s_tgs_ch = bdos_disk(6, 0xFF) & 0xFF;
        if (s_tgs_ch >= '0' && s_tgs_ch <= '9') {
            s_tgs_n = (s_tgs_n << 3) + (s_tgs_n << 1) + (s_tgs_ch - '0');
        } else if (s_tgs_ch == 'R') {
            if (s_tgs_n > 0) s_tgs_c = s_tgs_n;
            break;
        } else {
            goto tgs_done;
        }
    }

tgs_done:
    *s_tgs_rp = s_tgs_r;
    *s_tgs_cp = s_tgs_c;
}
