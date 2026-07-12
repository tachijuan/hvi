/*
 * util.c - Custom string and format utilities for HVI (VI clone for CP/M)
 * Author: Juan Orlandini
 * License: MIT
 *
 * Replaces <string.h> (strcpy, strncpy, strlen, strcmp) and sprintf,
 * eliminating the HI-TECH C standard-library format-string parser.
 * Integer formatting uses a powers-of-10 subtraction table so the Z80
 * software-division library routines (__divu, __modu, etc.) are never
 * linked.  bdos_puts() uses BDOS function 2 directly for pre-terminal
 * error messages without pulling in any stdio code.
 */

#include <cpm.h>
#include "hvi.h"

extern void con_write();  /* block console output via BDOS 6 (cstart.as) */

/* ------------------------------------------------------------------ */
/*  Console output                                                      */
/* ------------------------------------------------------------------ */

void bdos_puts(s)
char *s;
{
    con_write(s, hvi_strlen(s));
}

/* ------------------------------------------------------------------ */
/*  String utilities                                                    */
/* ------------------------------------------------------------------ */

static int hsl_n;

int hvi_strlen(s)
char *s;
{
    hsl_n = 0;
    while (s[hsl_n]) hsl_n++;
    return hsl_n;
}

void hvi_strcpy(d, s)
char *d, *s;
{
    while ((*d++ = *s++) != '\0') ;
}

void hvi_strncpy(d, s, n)
char *d, *s;
int   n;
{
    while (n > 0 && *s) { *d++ = *s++; n--; }
    while (n-- > 0) *d++ = '\0';
}

int hvi_strcmp(a, b)
char *a, *b;
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ------------------------------------------------------------------ */
/*  Integer formatting (no division -- repeated subtraction only)      */
/* ------------------------------------------------------------------ */

/*
 * Write the decimal representation of n into buf.
 * Returns a pointer to the byte just past the last digit written.
 * Handles values -32768..32767 (int range on Z80).
 * Maximum output length: 6 bytes ("-32768").
 * Public: term.c uses it for ANSI escape-sequence numbers.
 */
char *fmt_int(buf, n)
char *buf;
int   n;
{
    /* Initialised static: costs 10 bytes of stored data psect but saves
     * the ~40 bytes of code five per-call stores compiled to. */
    static int pows[5] = { 10000, 1000, 100, 10, 1 };
    static int i, d, started;

    if (n < 0) { *buf++ = '-'; n = -n; }
    if (n == 0) { *buf++ = '0'; return buf; }

    started = 0;
    for (i = 0; i < 5; i++) {
        d = 0;
        while (n >= pows[i]) { n -= pows[i]; d++; }
        if (d || started) { *buf++ = (char)('0' + d); started = 1; }
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Minimal sprintf: %s, %d, %c only                                   */
/* ------------------------------------------------------------------ */

/*
 * Format string into buf.  Supports %s (char * cast as int), %d (int),
 * and %c (int).  Up to 2 format arguments accepted as plain ints so no
 * varargs library is needed (no HVI message uses more; every extra slot
 * costs a push at all ~17 call sites).  On Z80/HI-TECH C, pointers and
 * ints are both 16 bits; char * arguments are safely passed as int and
 * cast back.  Callers must not supply more than 2 format specifiers.
 *
 * Each %s expansion is capped at SPRINTF_SMAX bytes: %s arguments can
 * come from the ex command line (up to CMD_MAX-1 = 127 chars -- long
 * filenames, unknown commands) while the destination is usually the
 * STATUS_MAX (128) byte ed.status buffer, so an unbounded copy could
 * run past it into the adjacent editor state.  The cap keeps the
 * longest fixed prefix ("Unknown command: ", 17 chars) plus one %s
 * plus the NUL inside STATUS_MAX.  Real CP/M filenames are ~14 chars,
 * so the cap only ever truncates the on-screen echo of junk input.
 */
#define SPRINTF_SMAX (STATUS_MAX - 28)

void hvi_sprintf(buf, fmt, a0, a1)
char *buf, *fmt;
int   a0, a1;
{
    static char *out, *fp, *s;
    static int   args[2], ai, sn;

    out = buf;
    fp  = fmt;
    args[0] = a0; args[1] = a1;
    ai = 0;

    while (*fp) {
        if (*fp != '%') { *out++ = *fp++; continue; }
        fp++;
        switch (*fp++) {
        case 's':
            s = (ai < 2) ? (char *)args[ai++] : (char *)0;
            if (!s) s = "";
            sn = SPRINTF_SMAX;
            while (*s && sn-- > 0) *out++ = *s++;
            break;
        case 'd':
            out = fmt_int(out, (ai < 2) ? args[ai++] : 0);
            break;
        case 'c':
            *out++ = (char)((ai < 2) ? args[ai++] : 0);
            break;
        case '%':
            *out++ = '%';
            break;
        default:
            break;
        }
    }
    *out = '\0';
}
