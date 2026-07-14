# Makefile for HVI - VI clone for CP/M
# Author: Juan Orlandini
# License: MIT
#
# COMPILE each source file individually (HI-TECH C V3.09 separate compile):
#
#   C -CPM -O -C cstart.as
#   C -CPM -O -C hvi.c gap.c term.c screen.c emove.c edit.c erepeat.c ex.c util.c cpmio.c
#
# LINK using the linker directly (l is the HI-TECH linker, renamed from LINQ):
#   - cstart.as replaces CRTCPM.OBJ; it must be FIRST so it lands at 0x0100.
#   - The HI-TECH linker is SINGLE-PASS for object files: a CALL to a symbol
#     not yet defined resolves to 0, which is below the -C file base (0x0100)
#     and triggers "code below file base of 0100h".  Every module must therefore
#     appear AFTER all modules it calls (dependency order).
#   - LX.LIB is a copy of LIBC.LIB with csv.obj removed (LIBR d LX.LIB csv.obj)
#     to avoid "multiply defined symbol" for csv/cret (which cstart.as provides).
#
# PREPARE LX.LIB (one-time setup -- only needed once):
#   PIP LX.LIB=LIBC.LIB
#   LIBR d LX.LIB csv.obj
#
# LINK (split across two lines using backslash continuation):
#   l
#   -Ptext=100H,data,bss -C100H -oh.com CSTART.OBJ CPMIO.OBJ UTIL.OBJ \
#   GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EREPEAT.OBJ EX.OBJ EDIT.OBJ HVI.OBJ LX.LIB
#
# RENAME:
#   REN HVI.COM=H.COM
#
# NOTE: -Ptext=100H,data,bss is required.  Without it the linker places
# the data and bss psects at address 0, causing "code below file base"
# for every static variable reference.  The backslash \ continues the
# command to a second line, bypassing the 128-char CP/M line limit.
#
# NOTE: the -CPM flag produces a CP/M .COM file.
#       the -O  flag enables the code optimizer.
#
# For debugging symbols add -H:
#   C -CPM -H -C cstart.as hvi.c gap.c ...
#
# TERMINAL FAMILY (compile-time, see termcfg.h): no flag builds the ANSI
# HVI.COM (dynamic size query, full features).  Pass TERMDEF to select
# another family and rename the linked H.COM to the family binary:
#
#   make clean                        (hvi.h pulls termcfg.h into every
#   make TERMDEF=-DTERM_VT52           module, so switching family needs a
#   ... link as below ...             full rebuild)
#   REN HVIVT52.COM=H.COM
#
#   TERM_VT52  HVIVT52   TERM_ADM3A HVIADM3   TERM_TVI    HVITVI
#   TERM_H19   HVIH19    TERM_WYSE50 HVIWY50  TERM_HAZ1500 HVIHZ15
#                                             TERM_OSB1   HVIOSB1
#
# Override the fixed geometry of a non-ANSI build (defaults 25x80, Osborne
# 52 cols) with extra defines, e.g. TERMDEF="-DTERM_TVI -DTERM_ROWS=24".

CC      = C
TERMDEF =
CFLAGS  = -CPM -O $(TERMDEF)

OBJS = cstart.obj hvi.obj gap.obj term.obj screen.obj emove.obj edit.obj erepeat.obj ex.obj util.obj cpmio.obj

# Individual compile steps
cstart.obj: cstart.as
	$(CC) $(CFLAGS) -C cstart.as

hvi.obj: hvi.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C hvi.c

gap.obj: gap.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C gap.c

term.obj: term.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C term.c

screen.obj: screen.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C screen.c

emove.obj: emove.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C emove.c

edit.obj: edit.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C edit.c

erepeat.obj: erepeat.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C erepeat.c

ex.obj: ex.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C ex.c

util.obj: util.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C util.c

cpmio.obj: cpmio.c hvi.h termcfg.h
	$(CC) $(CFLAGS) -C cpmio.c

clean:
	era *.obj
	era *.com
