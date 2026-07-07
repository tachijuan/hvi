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
 * and %c (int).  Up to 5 format arguments accepted as plain ints so no
 * varargs library is needed.  On Z80/HI-TECH C, pointers and ints are
 * both 16 bits; char * arguments are safely passed as int and cast back.
 * Callers must not supply more than 5 format specifiers.
 */
void hvi_sprintf(buf, fmt, a0, a1, a2, a3, a4)
char *buf, *fmt;
int   a0, a1, a2, a3, a4;
{
    char *out, *fp, *s;
    int   args[5], ai;

    out = buf;
    fp  = fmt;
    args[0] = a0; args[1] = a1; args[2] = a2;
    args[3] = a3; args[4] = a4;
    ai = 0;

    while (*fp) {
        if (*fp != '%') { *out++ = *fp++; continue; }
        fp++;
        switch (*fp++) {
        case 's':
            s = (ai < 5) ? (char *)args[ai++] : (char *)0;
            if (!s) s = "";
            while (*s) *out++ = *s++;
            break;
        case 'd':
            out = fmt_int(out, (ai < 5) ? args[ai++] : 0);
            break;
        case 'c':
            *out++ = (char)((ai < 5) ? args[ai++] : 0);
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
