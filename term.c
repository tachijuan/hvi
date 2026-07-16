/*
 * term.c - terminal I/O for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Provides cursor movement, attribute control, and input for the terminal
 * family selected in termcfg.h (ANSI/VT100 by default; VT52, ADM-3A,
 * Televideo, Wyse, Hazeltine and Osborne builds via -DTERM_xxx).  The escape
 * sequences below vary by family; the public function contract does not, so
 * screen.c/edit.c stay terminal-agnostic apart from a few capability #ifdefs.
 * Input via getch() (no echo, no line buffering).
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
#ifdef TERM_ANSI
static char s_rn_buf[8];           /* raw_num scratch for fmt_int */
#endif
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


#ifdef TERM_ANSI
/* Output the decimal representation of n (>= 0) via raw_byte.
 * Formatting is shared with util.c fmt_int to avoid a second converter.
 * Only the ANSI build formats decimal coordinates; the other families use
 * binary/offset addressing and never link fmt_int through here. */
static char *s_rn_p, *s_rn_end;

static void raw_num(n)
int n;
{
    s_rn_end = fmt_int(s_rn_buf, n);
    for (s_rn_p = s_rn_buf; s_rn_p < s_rn_end; s_rn_p++)
        raw_byte(*s_rn_p);
}
#endif

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
void term_putch(c0)
int c0;
{
    static int c;           /* param copy (absolute beats IX) */

    c = c0;
#ifdef TERM_HAZ1500
    /* The Hazeltine reserves 0x7E ('~') as its command lead-in and cannot
     * display it -- substitute '^' for both file content and the '~'
     * end-of-buffer markers.  Escape sequences bypass term_putch via
     * raw_byte, so this never corrupts a control string. */
    if (c == '~') c = '^';
#endif
    raw_byte(c);
    if (c >= 0x20 && c != 0x7F) {
        if (s_tcol >= 0) {
            s_tcol++;
#ifdef TERM_WRAP_IMMEDIATE
            /* Real terminals wrap (and may scroll) the instant the last
             * column is written, unlike ANSI's deferred wrap.  Drop cursor
             * tracking so term_goto re-addresses instead of trusting a
             * stale row for its \r / \n-run fast paths. */
            if (s_tcol >= ed.scr_cols) { s_trow = -1; s_tcol = -1; }
#endif
        }
    } else if (c == '\r') {
        s_tcol = 0;
    } else if (c == '\n') {
        if (s_trow >= 0) s_trow++;
        s_tcol = 0;
    } else if (c == '\b') {
        /* Backspace moves left one column (used before an emulated clreol
         * on no-clreol terminals, which needs a known column). */
        if (s_tcol > 0) s_tcol--;
    } else {
        /* Control char other than CR/LF/BS -- lose tracking */
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
 * Clear from cursor to end of current line.  The terminal cursor must end
 * where it started.  On families with a hardware clear-to-EOL the sequence
 * leaves the cursor put; the ADM-3A has none, so we pad spaces to the last
 * usable column and re-address (draw_row_at and edit.c avoid this slow path
 * by padding inline, but term_status_row still needs it).
 */
#ifdef TERM_HAS_CLREOL
void term_clreol()
{
#ifdef TERM_ADDR_ANSI
    raw_byte(0x1B); raw_byte('['); raw_byte('K');
#endif
#ifdef TERM_ADDR_VT52
    raw_byte(0x1B); raw_byte('K');
#endif
#ifdef TERM_ADDR_OFFSET
    raw_byte(0x1B); raw_byte('T');          /* Televideo/Wyse/Osborne (VERIFY) */
#endif
#ifdef TERM_ADDR_HAZ
    raw_byte('~'); raw_byte(0x0F);          /* ~ SI  (VERIFY) */
#endif
}
#else
static int s_ce_col;
void term_clreol()
{
    /* No hardware clear-to-EOL: overwrite the tail with spaces.  Stop one
     * short of the last column so the bottom-right cell is never written
     * (that scrolls an auto-wrap terminal).  Requires a known column. */
    if (s_tcol < 0) return;
    s_ce_col = s_tcol;
    while (s_tcol < ed.scr_cols - 1)
        term_putch(' ');
    term_goto(s_trow, s_ce_col);
}
#endif

#ifdef TERM_HAS_REVERSE
/* Set reverse video attribute (cursor doesn't move). */
void term_reverse()
{
#ifdef TERM_ANSI
    raw_byte(0x1B); raw_byte('['); raw_byte('7'); raw_byte('m');
#else
    raw_byte(0x1B); raw_byte('p');          /* H19 reverse on (VERIFY) */
#endif
}

/* Reset all video attributes (cursor doesn't move). */
void term_normal()
{
#ifdef TERM_ANSI
    raw_byte(0x1B); raw_byte('['); raw_byte('0'); raw_byte('m');
#else
    raw_byte(0x1B); raw_byte('q');          /* H19 reverse off (VERIFY) */
#endif
}
#endif

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
void term_goto(row0, col0)
int row0, col0;
{
    /* Params copied to statics: read ~16 times below, and term_goto
     * runs on every cursor move (absolute beats IX; never nests). */
    static int row, col;

    row = row0; col = col0;
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

            /* Move right with the terminal's short cursor-forward code.
             * Skipped on Hazeltine (no cheap forward; full re-address). */
#ifndef TERM_ADDR_HAZ
#ifdef TERM_ADDR_ANSI
            if (col > s_tcol && (col - s_tcol) <= 2) {   /* ESC[C = 3 bytes */
                s_tg_i = col - s_tcol;
                while (s_tg_i > 0) {
                    raw_byte(0x1B); raw_byte('['); raw_byte('C');
                    s_tg_i--;
                }
                s_tcol = col;
                return;
            }
#endif
#ifdef TERM_ADDR_VT52
            if (col > s_tcol && (col - s_tcol) <= 3) {   /* ESC C = 2 bytes */
                s_tg_i = col - s_tcol;
                while (s_tg_i > 0) {
                    raw_byte(0x1B); raw_byte('C');
                    s_tg_i--;
                }
                s_tcol = col;
                return;
            }
#endif
#ifdef TERM_ADDR_OFFSET
            if (col > s_tcol && (col - s_tcol) <= 6) {   /* ^L = 1 byte (VERIFY) */
                s_tg_i = col - s_tcol;
                while (s_tg_i > 0) {
                    raw_byte(0x0C);
                    s_tg_i--;
                }
                s_tcol = col;
                return;
            }
#endif
#endif /* !TERM_ADDR_HAZ */
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

    /* Full cursor-address sequence for the selected family. */
#ifdef TERM_ADDR_ANSI
    raw_byte(0x1B); raw_byte('[');          /* ESC [ row+1 ; col+1 H */
    raw_num(row + 1);
    raw_byte(';');
    raw_num(col + 1);
    raw_byte('H');
#endif
#ifdef TERM_ADDR_VT52
    raw_byte(0x1B); raw_byte('Y');          /* ESC Y row+32 col+32 */
    raw_byte(row + 32);
    raw_byte(col + 32);
#endif
#ifdef TERM_ADDR_OFFSET
    raw_byte(0x1B); raw_byte('=');          /* ESC = row+32 col+32 */
    raw_byte(row + 32);
    raw_byte(col + 32);
#endif
#ifdef TERM_ADDR_HAZ
    raw_byte('~'); raw_byte(0x11);          /* ~ DC1 col row (VERIFY offset) */
    raw_byte(col);
    raw_byte(row);
#endif
    s_trow = row;
    s_tcol = col;
}

/* ------------------------------------------------------------------ */
/*  Terminal scrolling                                                  */
/* ------------------------------------------------------------------ */

#ifdef TERM_HAS_SCROLL
#ifdef TERM_HAS_REGION
/* VT100 scroll region: a newline at the bottom of the region scrolls up,
 * a reverse index at the top scrolls down; the status row is outside the
 * region and stays put. */
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
#else
/* No scroll region: synthesize a 1-row scroll of the text area with a
 * delete-line/insert-line pair that leaves the status row (bottom row)
 * untouched.  The exposed blank row is the one scr_update_after_move
 * repaints next, so no screen.c change is needed. */
static void term_del_line()
{
#ifdef TERM_H19
    raw_byte(0x1B); raw_byte('M');          /* H19 delete line (VERIFY) */
#endif
#ifdef TERM_ADDR_OFFSET
    raw_byte(0x1B); raw_byte('R');          /* Televideo/Wyse/Kaypro delete line (VERIFY) */
#endif
#ifdef TERM_ADDR_HAZ
    raw_byte('~'); raw_byte(0x13);          /* ~ DC3 delete line (VERIFY) */
#endif
}

static void term_ins_line()
{
#ifdef TERM_H19
    raw_byte(0x1B); raw_byte('L');          /* H19 insert line (VERIFY) */
#endif
#ifdef TERM_ADDR_OFFSET
    raw_byte(0x1B); raw_byte('E');          /* Televideo/Wyse/Kaypro insert line (VERIFY) */
#endif
#ifdef TERM_ADDR_HAZ
    raw_byte('~'); raw_byte(0x1A);          /* ~ SUB insert line (VERIFY) */
#endif
}

void term_scroll_up()
{
    term_goto(0, 0);              term_del_line();
    term_goto(ed.scr_rows - 2, 0); term_ins_line();
    s_trow = -1; s_tcol = -1;    /* IL/DL cursor placement varies -- re-address */
}

void term_scroll_dn()
{
    term_goto(ed.scr_rows - 2, 0); term_del_line();
    term_goto(0, 0);              term_ins_line();
    s_trow = -1; s_tcol = -1;
}
#endif /* region vs ILDL */
#endif /* TERM_HAS_SCROLL */

#ifdef TERM_HAS_ICDC
/* Insert a blank character at the current cursor position */
void term_ins_char()
{
#ifdef TERM_ANSI
    raw_byte(0x1B); raw_byte('['); raw_byte('@');
#else
    raw_byte(0x1B); raw_byte('Q');          /* Televideo/Wyse insert char (VERIFY) */
#endif
}

/* Delete character at current cursor position */
void term_del_char()
{
#ifdef TERM_ANSI
    raw_byte(0x1B); raw_byte('['); raw_byte('P');
#else
    raw_byte(0x1B); raw_byte('W');          /* Televideo/Wyse delete char (VERIFY) */
#endif
}
#endif /* TERM_HAS_ICDC */

/* ------------------------------------------------------------------ */
/*  Terminal lifecycle                                                  */
/* ------------------------------------------------------------------ */
#ifdef TERM_KPRO
unsigned char *keymap;
unsigned char savmap[4];
/* get addr of cursor keys map table in BIOS */
unsigned char *kpro_getmap()
{
	/* in-line asm doesn't seem to work (compiler hangs) */
	register unsigned char *bios;

	/* get addr of BIOS page */
	bios = (unsigned char *)(*((unsigned char *)2) * 256);
	/* check for older versions */
	if ((bios[0x33] | bios[0x34] | bios[0x35] | bios[0x36]) != 0)
		return bios + 0x35;
	/* must be CP/M 2.2u */
	return bios + *((unsigned int *)(bios + 0x3a)) + 0x20 + 255;
}
#endif

void term_init()
{
    ed.scr_rows = DEF_ROWS;
    ed.scr_cols = DEF_COLS;
#ifdef TERM_HAS_GETSIZE
    term_getsize(&ed.scr_rows, &ed.scr_cols);
#endif
#ifdef TERM_KPRO
    keymap = kpro_getmap();
    gb_memmove(savmap, keymap, sizeof(savmap));
    gb_memmove(keymap, "kjhl", sizeof(savmap));
#endif
    term_clear();
    term_scroll_region();       /* no-op macro on families without a region */
    s_trow = -1;
    s_tcol = -1;
    term_flush();
}

#ifdef TERM_HAS_REGION
/* Set the scroll region to the text area: ESC [ 1 ; (scr_rows-1) r.
 * Also used by Ctrl-L after term_clear() erases it. */
void term_scroll_region()
{
    raw_byte(0x1B); raw_byte('['); raw_byte('1'); raw_byte(';');
    raw_num(ed.scr_rows - 1);
    raw_byte('r');
}
#endif

/* Park the cursor at the start of the status row and clear it. */
void term_status_row()
{
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
}

void term_restore()
{
#ifdef TERM_HAS_REGION
    raw_byte(0x1B); raw_byte('['); raw_byte('r'); /* reset scroll region */
#endif
    term_normal();              /* no-op macro on families without reverse */
    s_trow = -1; s_tcol = -1;
    term_status_row();
    raw_byte('\n');
    term_flush();
#ifdef TERM_KPRO
    gb_memmove(keymap, savmap, sizeof(savmap));
#endif
}

/*
 * Clear the entire screen and home the cursor.
 */
void term_clear()
{
#ifdef TERM_ADDR_ANSI
    raw_byte(0x1B); raw_byte('['); raw_byte('2'); raw_byte('J');
    raw_byte(0x1B); raw_byte('['); raw_byte('H');
#endif
#ifdef TERM_ADDR_VT52
    raw_byte(0x1B); raw_byte('H');          /* home */
    raw_byte(0x1B); raw_byte('J');          /* erase to end of screen */
#endif
#ifdef TERM_ADDR_OFFSET
    raw_byte(0x1A);                         /* ^Z clear+home (ADM-3A strap; VERIFY) */
#endif
#ifdef TERM_ADDR_HAZ
    raw_byte('~'); raw_byte(0x1C);          /* ~ FS clear+home (VERIFY) */
#endif
    s_trow = 0;
    s_tcol = 0;
    term_flush();
}

/* ------------------------------------------------------------------ */
/*  Input                                                               */
/* ------------------------------------------------------------------ */

/*
 * Poll BDOS 6 (Direct Console Input, E=0xFF: non-blocking, no echo) up
 * to n times.  Returns the character, or -1 when the countdown expires
 * with nothing available.  Shared by the ESC-sequence disambiguation
 * and the terminal-size handshake.
 *
 * Function 6 alone is the poll: it returns 0x00 when no character is
 * ready, so no separate status call is needed.  The previous BDOS 11
 * (Console Status) pre-poll broke on real CP/M 2.2 (North Star Horizon,
 * both North Star and Lifeboat brands): its result was consumed
 * unmasked, and only A holds the DRI-guaranteed byte result -- H
 * mirrors B, which real BDOS implementations leave as junk (RunCPM
 * returns a clean HL, hiding the bug).  Idle then looked "ready",
 * function 6 returned 0x00, and HVI saw an endless stream of NUL
 * keystrokes.  bdos_disk now zero-extends A (cstart.as) so every
 * result is clean, and the redundant status call is gone entirely --
 * which also sidesteps BIOSes whose CONST strays from the specified
 * 00h/FFh.  The trade-off is inherent to function 6: a real ^@
 * keystroke is indistinguishable from idle and is ignored (vi binds
 * nothing to NUL).
 */
#ifdef TERM_NEED_CONWAIT
static int cw_n, cw_c;

static int con_wait(n)
int n;
{
    cw_n = n;
    for (;;) {
        cw_c = bdos_disk(6, 0xFF) & 0xFF;
        if (cw_c != 0) return cw_c;
        if (--cw_n == 0) return -1;
    }
}
#endif

static int s_tgc_c;
#ifdef TERM_ESC_INPUT
static int s_tgc_c2;
static int s_tgc_pend;   /* pushback: key typed quickly after a bare ESC */
#endif

int term_getch()
{
    term_flush();

#ifdef TERM_ESC_INPUT
    if (s_tgc_pend) {
        s_tgc_c = s_tgc_pend;
        s_tgc_pend = 0;
        return s_tgc_c;
    }
#endif

    /* Spin on BDOS 6, E=0xFF (Direct Console Input: no echo,
     * non-blocking, 0x00 = nothing ready) through bdos_disk to
     * preserve IX around CALL 5.  See con_wait for why no BDOS 11
     * status pre-poll is used (North Star CONST incompatibility). */
    do {
        s_tgc_c = bdos_disk(6, 0xFF) & 0xFF;
    } while (s_tgc_c == 0);

#ifdef TERM_ESC_INPUT
    if (s_tgc_c == KEY_ESC) {
        s_tgc_c2 = con_wait(8000);
        if (s_tgc_c2 < 0) return KEY_ESC;
#ifdef TERM_ESC_ANSI
        if (s_tgc_c2 != '[') {
            /* Not an arrow sequence: the byte is the NEXT keystroke typed
             * quickly after ESC.  Push it back instead of swallowing it. */
            s_tgc_pend = s_tgc_c2;
            return KEY_ESC;
        }
        s_tgc_c2 = con_wait(8000);   /* the final arrow letter */
#endif
        /* VT52 delivers the arrow letter immediately after ESC. */
        switch (s_tgc_c2) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:
#ifdef TERM_ESC_VT52
            s_tgc_pend = s_tgc_c2;   /* ESC + non-arrow: push back the byte */
#endif
            return KEY_ESC;
        }
    }
#endif /* TERM_ESC_INPUT */
    return s_tgc_c;
}

/* ------------------------------------------------------------------ */
/*  Terminal size query (ANSI only)                                     */
/* ------------------------------------------------------------------ */

#ifdef TERM_HAS_GETSIZE
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
 */
static int tgs_num(term)
int term;
{
    s_tgs_term = term;
    s_tgs_n = 0;
    for (;;) {
        s_tgs_ch = con_wait(8000);
        if (s_tgs_ch >= '0' && s_tgs_ch <= '9')
            s_tgs_n = s_tgs_n * 10 + (s_tgs_ch - '0');  /* imul is linked */
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
 * Uses bdos_disk(6) exclusively (never BIOS CONIN) to preserve IX
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
#endif /* TERM_HAS_GETSIZE */
