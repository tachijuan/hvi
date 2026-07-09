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

extern int bdos_disk();

/* ------------------------------------------------------------------ */
/*  Console output -- uses bdos_disk(2,...) to preserve IX around CALL 5 */
/* ------------------------------------------------------------------ */

void bdos_puts(s)
char *s;
{
    while (*s)
        bdos_disk(2, (int)(unsigned char)*s++);
}

/* ------------------------------------------------------------------ */
/*  String utilities                                                    */
/* ------------------------------------------------------------------ */

int hvi_strlen(s)
char *s;
{
    int n = 0;
    while (s[n]) n++;
    return n;
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
    static int pows[5];   /* must be static to avoid IX-frame overhead */
    int i, d, started;

    pows[0] = 10000;
    pows[1] =  1000;
    pows[2] =   100;
    pows[3] =    10;
    pows[4] =     1;

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
 */
void hvi_sprintf(buf, size, fmt, a0, a1)
char *buf, *fmt;
int   size, a0, a1;
{
    char *out, *end, *fp, *s;
    int   args[2], ai;

    out = buf;
    end = buf + size - 1;
    fp  = fmt;
    args[0] = a0; args[1] = a1;
    ai = 0;

    while (*fp) {
        if (*fp != '%') { if (out < end) *out++ = *fp++; else fp++; continue; }
        fp++;
        switch (*fp++) {
        case 's':
            s = (ai < 2) ? (char *)args[ai++] : (char *)0;
            if (!s) s = "";
            while (*s && out < end) *out++ = *s++;
            break;
        case 'd':
            if (out + 6 <= end) out = fmt_int(out, (ai < 2) ? args[ai++] : 0);
            break;
        case 'c':
            if (out < end) *out++ = (char)((ai < 2) ? args[ai++] : 0);
            break;
        case '%':
            if (out < end) *out++ = '%';
            break;
        default:
            break;
        }
    }
    *out = '\0';
}
