/*
 * termcfg.h - compile-time terminal family selection for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Select a terminal family with -DTERM_xxx on the compile line.  No flag
 * selects ANSI/VT100 (the historical default: dynamic size query, scroll
 * region, insert/delete char, reverse video).  Every other family is a
 * fixed-geometry build that emits only the control codes that terminal
 * understands; capabilities it lacks are compiled out entirely so nothing
 * garbles a dumb terminal and the binary shrinks.
 *
 *   (none)        HVI.COM      ANSI / VT100      dynamic size, full features
 *   TERM_VT52     HVIVT52.COM  DEC VT52          ESC Y addressing, ESC arrows
 *   TERM_H19      HVIH19.COM   Heath/Zenith H19  VT52 + ins/del line + reverse
 *   TERM_ADM3A    HVIADM3.COM  Lear Siegler ADM-3A   worst case: goto + clear
 *   TERM_KPRO     HVIKPRO.COM  Kaypro 83/84      ADM-3A + insert/delete line
 *   TERM_TVI      HVITVI.COM   Televideo 9xx     ESC = addr, ins/del line+char
 *   TERM_WYSE50   HVIWY50.COM  Wyse 50           TVI-compatible
 *   TERM_HAZ1500  HVIHZ15.COM  Hazeltine 1500    ~ lead-in, binary addressing
 *   TERM_OSB1     HVIOSB1.COM  Osborne 1         ADM-3A style, 52 columns
 *
 * Fixed geometry defaults to 24 rows x 80 cols for the non-ANSI builds
 * (Osborne 52 cols); override at compile time with -DTERM_ROWS=25 etc.
 *
 * Only plain #ifdef / #ifndef / #else / #endif are used (no #elif, no
 * #if defined()) for maximum HI-TECH C V3.09 CPP compatibility.  Where two
 * families share behaviour the shared trait is expressed as a derived macro
 * (TERM_ADDR_OFFSET, TERM_NEED_CONWAIT, ...) so term.c tests one flag.
 *
 * Capability macros (defined = present):
 *   TERM_HAS_GETSIZE   ESC[6n cursor-position report size query
 *   TERM_HAS_REGION    VT100 scroll region (ESC[1;Nr)
 *   TERM_HAS_ILDL      insert-line / delete-line
 *   TERM_HAS_SCROLL    a 1-line hardware scroll exists (REGION or ILDL)
 *   TERM_HAS_CLREOL    hardware clear-to-end-of-line
 *   TERM_HAS_ICDC      single insert-char / delete-char
 *   TERM_HAS_REVERSE   reverse-video attribute for the status bar
 *   TERM_ESC_ANSI      input arrows arrive as ESC [ A..D
 *   TERM_ESC_VT52      input arrows arrive as ESC A..D
 *   TERM_WRAP_IMMEDIATE  writing the last column moves the cursor at once
 * Addressing style (exactly one):
 *   TERM_ADDR_ANSI     ESC [ row+1 ; col+1 H          (decimal)
 *   TERM_ADDR_VT52     ESC Y row+32 col+32            (binary, offset 32)
 *   TERM_ADDR_OFFSET   ESC = row+32 col+32            (binary, offset 32)
 *   TERM_ADDR_HAZ      ~ DC1 col row                  (binary, column first)
 */

#ifndef TERMCFG_H
#define TERMCFG_H

/* H19 is a VT52 superset. */
#ifdef TERM_H19
#ifndef TERM_VT52
#define TERM_VT52
#endif
#endif

/* ---------------------------------------------------------------- */
/*  Family selection: define addressing style, capabilities, geometry */
/* ---------------------------------------------------------------- */

#ifdef TERM_VT52
/* -------- DEC VT52 / Heath-Zenith H19 -------- */
#define TERM_NAME        "VT52"
#define TERM_ADDR_VT52
#define TERM_HAS_CLREOL          /* ESC K */
#define TERM_ESC_VT52            /* arrows ESC A..D */
#define TERM_WRAP_IMMEDIATE
#ifdef TERM_H19
#define TERM_HAS_ILDL            /* ESC L / ESC M   (VERIFY H19 manual) */
#define TERM_HAS_REVERSE         /* ESC p / ESC q   (VERIFY H19 manual) */
#endif

#else
#ifdef TERM_ADM3A
/* -------- Lear Siegler ADM-3A (worst case) -------- */
#define TERM_NAME        "ADM-3A"
#define TERM_ADDR_OFFSET         /* ESC = row+32 col+32 */
#define TERM_WRAP_IMMEDIATE
/* no CLREOL, no SCROLL, no ICDC, no REVERSE, no ESC arrows */

#else
#ifdef TERM_KPRO
/* -------- Kaypro 83/84 (ADM-3A superset with insert/delete line) -------- */
#define TERM_NAME        "Kaypro"
#define TERM_ADDR_OFFSET         /* ESC = row+32 col+32 (ADM-3A style) */
#define TERM_WRAP_IMMEDIATE
#define TERM_HAS_ILDL            /* ESC E insert line / ESC R delete line */
/* Insert/delete-line drives the 1-row scroll fast path (TERM_HAS_SCROLL is
 * derived from TERM_HAS_ILDL below) instead of a full repaint.  The Kaypro
 * shares the ADM-3A ESC = addressing and ^Z clear/home, so the OFFSET code
 * paths in term.c already emit the right bytes: ESC E / ESC R for ins/del
 * line match the Kaypro firmware.
 * Left out on purpose (see issue #5): no CLREOL (the space-pad fallback is
 * always correct), no ICDC, no REVERSE (the /84's attributes are optional),
 * and no ESC arrows -- the Kaypro cursor keys emit ^H ^J ^K ^L, which
 * collide with hjkl and would need BIOS keymap patching to remap. */

#else
#ifdef TERM_TVI
/* -------- Televideo 912/920/925/950 -------- */
#define TERM_NAME        "Televideo"
#define TERM_ADDR_OFFSET
#define TERM_HAS_CLREOL          /* ESC T   (VERIFY) */
#define TERM_HAS_ILDL            /* ESC E / ESC R   (VERIFY) */
#define TERM_HAS_ICDC            /* ESC Q / ESC W   (VERIFY; 912/920 opt ROM) */
#define TERM_WRAP_IMMEDIATE

#else
#ifdef TERM_WYSE50
/* -------- Wyse 50 (Televideo-compatible native mode) -------- */
#define TERM_NAME        "Wyse 50"
#define TERM_ADDR_OFFSET
#define TERM_HAS_CLREOL          /* ESC T   (VERIFY native mode) */
#define TERM_HAS_ILDL            /* ESC E / ESC R   (VERIFY) */
#define TERM_HAS_ICDC            /* ESC Q / ESC W   (VERIFY) */
#define TERM_WRAP_IMMEDIATE

#else
#ifdef TERM_HAZ1500
/* -------- Hazeltine 1500 (~ lead-in, binary addressing) -------- */
#define TERM_NAME        "Hazeltine 1500"
#define TERM_ADDR_HAZ            /* ~ DC1 col row, column first (VERIFY) */
#define TERM_HAS_CLREOL          /* ~ SI  (VERIFY) */
#define TERM_HAS_ILDL            /* ~ SUB / ~ DC3  (VERIFY) */
#define TERM_WRAP_IMMEDIATE
/* Hazeltine cannot display '~' (0x7E is the command lead-in); term.c
 * substitutes '^' for tilde in term_putch. */

#else
#ifdef TERM_OSB1
/* -------- Osborne 1 (ADM-3A style, 52-column screen) -------- */
#define TERM_NAME        "Osborne 1"
#define TERM_ADDR_OFFSET         /* ESC = row+32 col+32 */
#define TERM_HAS_CLREOL          /* ESC T  (VERIFY) */
#define TERM_WRAP_IMMEDIATE
#ifndef TERM_COLS
#define TERM_COLS 52
#endif

#else
/* -------- default: ANSI / VT100 -------- */
#define TERM_ANSI
#define TERM_NAME        "ANSI"
#define TERM_ADDR_ANSI
#define TERM_HAS_GETSIZE
#define TERM_HAS_REGION
#define TERM_HAS_CLREOL
#define TERM_HAS_ICDC
#define TERM_HAS_REVERSE
#define TERM_ESC_ANSI
#ifndef TERM_ROWS
#define TERM_ROWS 24             /* ANSI: only the CPR-timeout fallback */
#endif
#ifndef TERM_COLS
#define TERM_COLS 80
#endif

#endif /* OSB1   */
#endif /* HAZ1500 */
#endif /* WYSE50 */
#endif /* TVI    */
#endif /* KPRO   */
#endif /* ADM3A  */
#endif /* VT52   */

/* ---------------------------------------------------------------- */
/*  Derived flags                                                     */
/* ---------------------------------------------------------------- */

/* TERM_HAS_SCROLL: a 1-line hardware scroll exists.  It is DERIVED from the
 * underlying mechanism -- a VT100 scroll region or insert/delete-line -- so
 * a family need only declare its mechanism (TERM_HAS_REGION / TERM_HAS_ILDL)
 * and the scroll fast path in term.c and screen.c switches on automatically.
 * Deriving it (rather than making each family also hand-define TERM_HAS_SCROLL)
 * removes the trap that caused issue #5: a Kaypro build that declared
 * TERM_HAS_ILDL but forgot TERM_HAS_SCROLL compiled with ins/del-line dead
 * and fell back to full-screen repaints. */
#ifdef TERM_HAS_REGION
#define TERM_HAS_SCROLL
#else
#ifdef TERM_HAS_ILDL
#define TERM_HAS_SCROLL
#endif
#endif

/* TERM_ESC_INPUT: this family delivers arrow keys as an ESC-prefixed
 * sequence, so term_getch must parse (and keep a 1-byte pushback). */
#ifdef TERM_ESC_ANSI
#define TERM_ESC_INPUT
#endif
#ifdef TERM_ESC_VT52
#define TERM_ESC_INPUT
#endif

/* con_wait() (timed console poll) is needed only for the size query and
 * for ESC-prefixed arrow-key parsing. */
#ifdef TERM_HAS_GETSIZE
#define TERM_NEED_CONWAIT
#else
#ifdef TERM_ESC_INPUT
#define TERM_NEED_CONWAIT
#endif
#endif

/* ---------------------------------------------------------------- */
/*  Fixed geometry defaults (non-ANSI families)                       */
/* ---------------------------------------------------------------- */

#ifndef TERM_ROWS
#define TERM_ROWS 24
#endif
#ifndef TERM_COLS
#define TERM_COLS 80
#endif

/* ---------------------------------------------------------------- */
/*  No-op stand-ins for absent optional operations                    */
/*  (so call sites compile to nothing on families that lack them).    */
/* ---------------------------------------------------------------- */

#ifndef TERM_HAS_REVERSE
#define term_reverse()
#define term_normal()
#endif
#ifndef TERM_HAS_REGION
#define term_scroll_region()
#endif

#endif /* TERMCFG_H */
