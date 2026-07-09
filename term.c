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
extern void con_write();  /* block console output via BDOS 6 (cstart.as) */

/* ------------------------------------------------------------------ */
/*  Output buffer                                                       */
/* ------------------------------------------------------------------ */

#define OUT_BUF_SZ  256
static char s_outbuf[OUT_BUF_SZ];
static int  s_outpos;
static char s_rn_buf[8];           /* raw_num scratch for fmt_int */
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
 * Formatting is shared with util.c fmt_int to avoid a second converter. */
static char *s_rn_p, *s_rn_end;

static void raw_num(n)
int n;
{
    s_rn_end = fmt_int(s_rn_buf, n);
    for (s_rn_p = s_rn_buf; s_rn_p < s_rn_end; s_rn_p++)
        raw_byte(*s_rn_p);
}

/*
 * Flush the output buffer to stdout.
 * Called by term_getch() before blocking; also exported so callers can
 * flush at logical checkpoints.
 * con_write (cstart.as) loops CALL 5 in assembly using BDOS function 6,
 * replacing one C-level bdos_disk(2,c) call per byte.
 */
void term_flush()
{
    if (s_outpos > 0) {
        con_write(s_outbuf, s_outpos);
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

/* Park the cursor at the start of the status row and clear it. */
void term_status_row()
{
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
}

void term_restore()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('r'); /* reset scroll region */
    term_normal();
    s_trow = -1; s_tcol = -1;
    term_status_row();
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

/*
 * Poll console status (BDOS 11) up to n times, then read one character
 * with BDOS 6 (Direct Console Input, no echo).  Returns the character,
 * or -1 when the countdown expires with nothing available.  Shared by
 * the ESC-sequence disambiguation and the terminal-size handshake.
 * n cached in a static: bdos_disk calls follow.
 */
static int cw_n;

static int con_wait(n)
int n;
{
    cw_n = n;
    while (bdos_disk(11, 0) == 0) {
        if (--cw_n == 0) return -1;
    }
    return bdos_disk(6, 0xFF) & 0xFF;
}

static int s_tgc_c, s_tgc_c2;
static int s_tgc_pend;   /* pushback: key typed quickly after a bare ESC */

int term_getch()
{
    term_flush();

    if (s_tgc_pend) {
        s_tgc_c = s_tgc_pend;
        s_tgc_pend = 0;
        return s_tgc_c;
    }

    /* BDOS 6, 0xFF = Direct Console Input (no echo, non-blocking).
     * Poll via BDOS 11 (Console Status) first to reduce spin time, then read.
     * Both go through bdos_disk to preserve IX around CALL 5. */
    while (bdos_disk(11, 0) == 0) ;            /* wait for key available */
    s_tgc_c = bdos_disk(6, 0xFF) & 0xFF;

    if (s_tgc_c == KEY_ESC) {
        s_tgc_c2 = con_wait(8000);
        if (s_tgc_c2 < 0) return KEY_ESC;
        if (s_tgc_c2 != '[') {
            /* Not an arrow sequence: the byte is the NEXT keystroke typed
             * quickly after ESC.  Push it back instead of swallowing it. */
            s_tgc_pend = s_tgc_c2;
            return KEY_ESC;
        }

        s_tgc_c2 = con_wait(8000);
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
static int  s_tgs_term;     /* expected number terminator           */

/*
 * Read a decimal number from the console, terminated by `term`.
 * Returns the value, or -1 on timeout or an unexpected character.
 * Digit multiply uses shifts to avoid linking __mulu:
 *   n * 10  ==  (n << 3) + (n << 1)
 */
static int tgs_num(term)
int term;
{
    s_tgs_term = term;
    s_tgs_n = 0;
    for (;;) {
        s_tgs_ch = con_wait(8000);
        if (s_tgs_ch >= '0' && s_tgs_ch <= '9')
            s_tgs_n = (s_tgs_n << 3) + (s_tgs_n << 1) + (s_tgs_ch - '0');
        else if (s_tgs_ch == s_tgs_term)
            return s_tgs_n;
        else
            return -1;
    }
}

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
 * (con_wait) so a non-responding terminal falls back to DEF_ROWS/COLS.
 */
void term_getsize(rows, cols)
int *rows;
int *cols;
{
    s_tgs_rp = rows;          /* cache params before any BDOS call */
    s_tgs_cp = cols;
    s_tgs_r  = DEF_ROWS;     /* default in case of timeout */
    s_tgs_c  = DEF_COLS;

    /* Steps 1+2: park the cursor at the extreme corner (the terminal
     * clamps to its real size), then request a cursor-position report.
     * One block write replaces 13 individual BDOS calls. */
    con_write("\033[999;999H\033[6n", 14);

    /* Step 3: parse ESC [ rows ; cols R */
    if (con_wait(30000) == 0x1B && con_wait(8000) == '[') {
        s_tgs_n = tgs_num(';');
        if (s_tgs_n >= 0) {
            if (s_tgs_n > 0) s_tgs_r = s_tgs_n;
            s_tgs_n = tgs_num('R');
            if (s_tgs_n > 0) s_tgs_c = s_tgs_n;
        }
    }

    *s_tgs_rp = s_tgs_r;
    *s_tgs_cp = s_tgs_c;
}
