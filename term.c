/*
 * term.c - ANSI terminal I/O for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Provides cursor movement, attribute control, and input
 * using ANSI/VT100 escape sequences. Input via getch() so
 * no echo and no line buffering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpm.h>
#include "hvi.h"

extern Editor ed;

/* getch() is in the HI-TECH C runtime; bdos() and bios() come from <cpm.h> */
extern int getch();

/* Flush stdout (CP/M stdio may buffer output). */
static void term_flush()
{
    fflush(stdout);
}

/* Output a single character to the console. */
void term_putch(c)
int c;
{
    putchar(c);
}

/* Output a null-terminated string to the console. */
void term_puts(s)
char *s;
{
    while (*s)
        putchar((unsigned char)*s++);
}

/*
 * Initialise terminal.
 * On CP/M there is no termios; we rely on the terminal
 * already being in raw mode via getch().
 * Send a clear-screen and home-cursor to start fresh.
 */
void term_init()
{
    ed.scr_rows = DEF_ROWS;
    ed.scr_cols = DEF_COLS;
    term_getsize(&ed.scr_rows, &ed.scr_cols);
    term_clear();
}

/* Restore terminal (no-op on CP/M, but output a newline). */
void term_restore()
{
    term_normal();
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    term_flush();
}

/* Clear the entire screen. */
void term_clear()
{
    term_puts("\033[2J");
    term_goto(0, 0);
    term_flush();
}

/* Clear from cursor to end of current line. */
void term_clreol()
{
    term_puts("\033[K");
}

/*
 * Move cursor to row, col (both 0-based).
 * ANSI sequences are 1-based, so we add 1.
 */
void term_goto(row, col)
int row, col;
{
    char buf[16];
    sprintf(buf, "\033[%d;%dH", row + 1, col + 1);
    term_puts(buf);
}

/* Set bold attribute. */
void term_bold()
{
    term_puts("\033[1m");
}

/* Set reverse-video attribute. */
void term_reverse()
{
    term_puts("\033[7m");
}

/* Reset all attributes to normal. */
void term_normal()
{
    term_puts("\033[0m");
}

/*
 * Read one character from the console without echo.
 * Uses HiTech-C getch() which calls CP/M BIOS directly.
 */
int term_getch()
{
    int c;
    term_flush();
    c = getch();
    return c & 0xFF;
}

/*
 * Query terminal dimensions using the ANSI cursor-position report.
 *
 * Send ESC[999;999H (move cursor to extreme corner) then ESC[6n (request
 * cursor position).  The terminal responds with ESC[rows;colsR.
 *
 * Root cause of prior failures: on CP/M systems whose host terminal is in
 * canonical (line-buffered) mode, ESC (0x1B) is delivered immediately by
 * the Unix line discipline as a special character, but the rest of the
 * response ([rows;colsR) is held until the user presses ENTER.  BDOS fn 2
 * (console output), called by fprintf inside the read loop, also invokes
 * BIOS CONST to check for ^S/^C; if the buffered response bytes are
 * visible to CONST at that moment they can be consumed before we read them.
 *
 * Fix — two changes:
 *   1. Phase 2 uses bios(3,0,0) (BIOS CONIN) directly rather than getch()
 *      or BDOS wrappers.  BIOS CONIN is the lowest-level console input; on
 *      emulators that implement it via a raw-mode read it avoids the
 *      canonical buffer entirely.
 *   2. All debug output is deferred until after Phase 2 is complete, so
 *      no BDOS console-output call runs between consecutive bios(3) reads.
 *
 * On systems where BIOS CONIN is still line-buffered one ENTER keypress
 * after the query flushes the canonical buffer and the response is read
 * correctly in a single burst.
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

    /* Send query; fflush guarantees it leaves the stdio buffer. */
    term_puts("\033[999;999H\033[6n");
    fflush(stdout);

    /* Phase 1: wait up to 30000 BIOS CONST polls for any response byte.
     * bios(2,...) is BIOS CONST: returns 0 if no char ready, else non-zero. */
    wait = 30000;
    while (bios(2, 0, 0) == 0) {
        if (--wait == 0) {
            if (ed.debug)
                fprintf(stderr, "\r\nDBG getsize: no response\r\n");
            return;
        }
    }

    /*
     * Phase 2: read the full CPR sequence via BIOS CONIN (bios(3,...)).
     * No debug output inside this loop — BDOS console writes between reads
     * can interfere with the buffered response on some CP/M implementations.
     * The loop is bounded by 48 iterations so a truncated response cannot
     * stall indefinitely.
     */
    i = 0;
    state = 0;
    for (total = 0; total < 48 && i < (int)(sizeof(buf) - 1); total++) {
        c = (int)(unsigned char)bios(3, 0, 0);  /* BIOS CONIN */

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

    /* All reads done; debug output is safe now. */
    if (ed.debug)
        fprintf(stderr, "\r\nDBG getsize: state=%d buf=[%s]\r\n", state, buf);

    if (state == 3) {
        p = buf;
        r = co = 0;
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') r  = r  * 10 + (*p++ - '0');
        while (*p && (*p < '0' || *p > '9')) p++;
        while (*p >= '0' && *p <= '9') co = co * 10 + (*p++ - '0');
        if (r > 0 && co > 0) { *rows = r; *cols = co; }
        if (ed.debug)
            fprintf(stderr, "DBG getsize: rows=%d cols=%d\r\n", r, co);
    } else if (ed.debug) {
        fprintf(stderr, "DBG getsize: parse failed\r\n");
    }
}
