/*
 * hvi.h - HVI: A VI clone for CP/M
 * Author: Juan Orlandini
 * License: MIT
 *
 * Common definitions, types, and extern declarations.
 * No standard library headers are included; all I/O and string operations
 * are provided by util.c (hvi_sprintf, hvi_strcpy, ...) and cpmio.c
 * (hvi_fopen, hvi_fgetc, hvi_malloc, ...).
 */

#ifndef HVI_H
#define HVI_H

#define HVI_VERSION "2.7.2"

/*
 * Enable debug I/O tracing: prints one line per BDOS 33 refill showing
 * the sector number requested, the return code, and the first 4 bytes of
 * the buffer.  Output goes directly to the console via BDOS 2; it will be
 * overwritten when the next scr_refresh() runs.
 * Uncomment to enable; rebuild all files.
 */
/* #define HVI_DEBUG */

/* Terminal defaults */
#define DEF_COLS    80
#define DEF_ROWS    24

/* Gap buffer limits */
#define GAP_MIN     256     /* gap reserve kept after each insertion */
#define BUF_MAX     24000   /* target in-memory content (actual may be less) */
#define LOAD_CHUNK  4096    /* chars loaded per page-in from tail */

/* Undo text buffer size */
#define UNDO_MAX    1024

/* Dot-repeat text buffer size */
#define DOT_TEXT_MAX 128

/* Yank buffer size */
#define YANK_MAX    1024

/* Tab stop width */
#define TAB_STOP    8

/* Search pattern buffer */
#define SEARCH_MAX  64

/* Marks: slot 0 (MARK_PREV) holds the position before the last jump
 * (used by ``); slots 1-26 hold `a-`z.  The layout mirrors ASCII:
 * slot = mark char - '`' (0x60), so '`' and a-z resolve with a single
 * subtraction and one unsigned range test (motion_endpoint '`').
 * A mark value of -1 means "not set". */
#define NMARKS      27
#define MARK_PREV   0

/* Filename/status buffer */
#define PATH_MAX    64
#define STATUS_MAX  128
#define CMD_MAX     128

/* Key code definitions */
#define KEY_NUL     0x00
#define KEY_CTRL_A  0x01
#define KEY_CTRL_B  0x02
#define KEY_CTRL_C  0x03
#define KEY_CTRL_D  0x04
#define KEY_CTRL_E  0x05
#define KEY_CTRL_F  0x06
#define KEY_CTRL_G  0x07
#define KEY_BS      0x08
#define KEY_CTRL_H  0x08
#define KEY_TAB     0x09
#define KEY_LF      0x0A
#define KEY_CTRL_K  0x0B
#define KEY_CTRL_L  0x0C
#define KEY_CR      0x0D
#define KEY_CTRL_N  0x0E
#define KEY_CTRL_P  0x0F
#define KEY_CTRL_R  0x12
#define KEY_CTRL_U  0x15
#define KEY_CTRL_W  0x17
#define KEY_ESC     0x1B
#define KEY_DEL     0x7F

/* Synthetic codes for ANSI arrow keys (above 0xFF, never from raw input) */
#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_LEFT    0x103
#define KEY_RIGHT   0x104

/* Editor modes */
#define MODE_NORMAL  0
#define MODE_INSERT  1
#define MODE_REPLACE 2
#define MODE_CMDLINE 3

/* Undo operation types */
#define UNDO_NONE    0
#define UNDO_INSERT  1
#define UNDO_DELETE  2

/* Search direction */
#define SEARCH_FWD   1
#define SEARCH_BWD  -1

/* End-of-file sentinel returned by hvi_fgetc() (replaces stdio EOF). */
#define HEOF (-1)

/*
 * HFILE: CP/M direct-BDOS file handle (replaces stdio FILE).
 * Each instance carries its own 128-byte sector buffer, which is set as
 * the BDOS DMA address before every BDOS read/write call.
 *
 * File names accept a ZCPR-style "du:" prefix (drive A-P, user 0-15):
 * "B:F.TXT", "3:F.TXT", "B3:F.TXT".  CP/M has no per-FCB user number,
 * so the handle records the file's user area and cpmio.c switches to
 * it (BDOS 32) around every directory/data call on the file.
 */
typedef struct {
    unsigned char fcb[36];  /* CP/M File Control Block              */
    unsigned char buf[128]; /* 128-byte sector buffer (one CP/M rec) */
    int  buf_pos;           /* read/write byte offset within buf     */
    int  buf_valid;         /* bytes valid in buf (read mode)        */
    long pos;               /* absolute byte position in file        */
    int  mode;              /* 0=closed  1=read  2=write             */
    int  dirty;             /* write buffer has unflushed data       */
    int  at_eof;            /* 1 when BDOS reported end-of-file      */
    int  user;              /* user area 0-15; -1 = HVI's own user   */
} HFILE;

/*
 * Gap buffer: content is split around a gap.
 * [data before gap][  GAP  ][data after gap]
 * Content length = size - (gend - gstart)
 */
typedef struct {
    char *buf;      /* allocated buffer */
    int   size;     /* total allocation including gap */
    int   gstart;   /* gap start (first gap byte) */
    int   gend;     /* gap end   (first non-gap byte after gap) */
} GapBuf;

/* Single-level undo record */
typedef struct {
    int  type;              /* UNDO_INSERT or UNDO_DELETE */
    int  pos;               /* buffer position of operation */
    int  len;               /* number of characters */
    int  was_clean;         /* non-zero if buffer was unmodified before this op */
    char text[UNDO_MAX];    /* saved text (for delete undo) */
} UndoRec;

/* Editor global state */
typedef struct {
    /* Gap buffer */
    GapBuf  gb;

    /* File */
    char    filename[PATH_MAX];
    int     modified;       /* non-zero if buffer differs from disk */

    /* Cursor: byte offset in logical buffer.
     * cstart.as reads/writes this at the fixed offset _ed+74 (EDCURP,
     * used by gb_insert_room) -- keep the members above in sync. */
    int     cur_pos;

    /* Display */
    int     top_pos;        /* buffer pos of first visible char */
    int     scr_rows;       /* terminal height */
    int     scr_cols;       /* terminal width */

    /* Mode */
    int     mode;

    /* Undo */
    UndoRec undo;

    /* Yank buffer */
    char    yank[YANK_MAX];
    int     yank_len;
    int     yank_line;      /* non-zero if yanked whole lines */

    /* Search */
    char    search[SEARCH_MAX];
    int     search_dir;
    int     search_wrapped;  /* non-zero if last search wrapped around the file */

    /* Ex command line */
    char    cmdline[CMD_MAX];
    int     cmdlen;

    /* Status message (shown until next keypress) */
    char    status[STATUS_MAX];

    /* Wanted column for vertical movement */
    int     want_col;

    /* Dot-repeat: replay last change with '.' */
    int     dot_cmd;            /* 0=none, else command key */
    int     dot_motion;         /* motion for d/c operators */
    int     dot_arg;            /* extra arg (replacement char for 'r') */
    int     dot_count;          /* count when originally issued */
    int     dot_len;            /* length of text for insert replay */
    char    dot_text[DOT_TEXT_MAX]; /* inserted text for replay */

    /* Quit flag */
    int     quit;

    /* Large-file sliding window */
    long    win_start;           /* byte offset in tail_file where buffer begins */
    long    tail_offset;         /* byte offset in tail_file where buffer ends */
    char    tail_file[PATH_MAX]; /* original source file for head and tail */

    /* Cached current line number (incremental, avoids O(n) scans) */
    int     cur_line;            /* line number of cur_pos; -1 = stale */
    int     cur_line_pos;        /* cur_pos when cur_line was computed; -1 = stale */

    /* Cached total line count (0 = invalid; set by scr_line_count) */
    int     line_cnt_cached;

    /* Cached visual row of cursor within viewport (-1 = needs recompute).
     * Maintained incrementally by mv_down/mv_up so scr_scroll_to_cursor()
     * can skip the O(text_rows) viewport scan on every j/k keypress. */
    int     cur_vrow;

    /* Marks: buffer positions for m/` (a-z + MARK_PREV); -1 = not set.
     * Adjusted on every insert/delete; cleared when the large-file
     * window slides or a new file is loaded (positions become invalid). */
    int     marks[NMARKS];
} Editor;

extern Editor ed;
extern char msg_insert[];

/* ---- gap.c ---- */
int  gb_init();
void gb_free();
int  gb_content_len();  /* logical content size (no gap) */
int  gb_char_at(/* int pos */);  /* assembly, cstart.as: reads GapBuf
                                  * fields at fixed offsets from ed --
                                  * GapBuf must stay Editor's 1st member */
int  find_bol(/* int pos */);
int  find_eol(/* int pos */);
int  gb_find_ch(/* int pos, int c */);  /* find_eol for any byte */
int  gb_insert(/* int pos, char *text, int len */);
int  gb_delete(/* int pos, int len */);
void gb_copy_out(/* char *dst, int pos, int len */);
int  gb_count_nl(/* int pos, int len */);
int  gb_load(/* char *filename, HFILE *fp */); /* fp=NULL -> hvi_fopen filename */
int  gb_save(/* char *filename */);
int  gb_load_more(/* int n */);
int  gb_reload_from(/* long offset */);
void gb_load_last();   /* window to file tail, cursor to last line */
void gb_load_prev();   /* window back one LOAD_CHUNK from win_start */
int  gb_make_room();
int  gb_insert_room();  /* args in gir_pos/gir_text/gir_len globals */
int  gb_goto_line(/* int n */);

/* ---- util.c ---- */
void bdos_puts(/* char *s */);
int  hvi_strlen(/* char *s */);
void hvi_strcpy(/* char *d, char *s */);
void hvi_strncpy(/* char *d, char *s, int n */);
int  hvi_strcmp(/* char *a, char *b */);
char *fmt_int(/* char *buf, int n -- returns ptr past last digit */);
void hvi_sprintf(/* char *buf, char *fmt, int a0, a1 -- max 2 args */);

/* ---- cpmio.c ---- */
char  *hvi_malloc(/* int size */);
void   hvi_free(/* char *p */);
HFILE *hvi_fopen(/* char *name, char *mode */);
void   hvi_fclose(/* HFILE *fp */);
int    hvi_fgetc(/* HFILE *fp */);
int    hvi_fputc(/* int c, HFILE *fp */);
long   hvi_ftell(/* HFILE *fp */);
int    hvi_fseek(/* HFILE *fp, long offset, int whence */);
void   hvi_remove(/* char *name */);
void   hvi_rename(/* char *oldname, char *newname */);
long   hvi_fsize(/* char *name */);

/* ---- term.c ---- */
void term_init();
void term_restore();
void term_clear();
void term_clreol();
void term_goto(/* int row, int col */);
void term_status_row();
void term_scroll_region();
void term_putch(/* int c */);
void term_puts(/* char *s */);
void term_flush();
int  term_getch();
void term_getsize(/* int *rows, int *cols */);
void term_reverse();
void term_normal();
void term_scroll_up();
void term_scroll_dn();
void term_ins_char();
void term_del_char();

/* ---- screen.c ---- */
void scr_refresh();
void scr_redraw_line();
void scr_redraw_cur_line();
void scr_redraw_from_cur();
void scr_update_cursor();
void scr_show_status();
void scr_clear_status();
void scr_status_invalidate();
int  scr_line_is_1row();
void scr_edit_end(/* int light -- 1: one-row repaint, 0: from cursor */);
void scr_fix_char();
void scr_fix_span();
int  scr_last_line_start();
void scr_scroll_to_cursor();
void scr_update_after_move();
void scr_adj();
void scr_after_edit();
int  scr_cur_line();
int  scr_pos_col();
int  scr_vrow_col();
int  scr_line_start();
int  scr_line_count();
int  next_vrow();
int  vrow_start_of();

/* ---- edit.c ---- */
void edit_run();

/* ---- ex.c ---- */
int ex_execute(/* char *cmd */);

#endif /* HVI_H */
