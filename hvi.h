/*
 * hvi.h - HVI: A VI clone for CP/M
 * Author: Juan Orlandini
 * License: MIT
 *
 * Common definitions, types, and extern declarations.
 */

#ifndef HVI_H
#define HVI_H

#define HVI_VERSION "1.4"

/* Terminal defaults */
#define DEF_COLS    80
#define DEF_ROWS    24

/* Gap buffer limits */
#define GAP_MIN     256     /* gap reserve kept after each insertion */
#define BUF_MAX     30000   /* target in-memory content (actual may be less) */
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

    /* Cursor: byte offset in logical buffer */
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

    /* Ex command line */
    char    cmdline[CMD_MAX];
    int     cmdlen;

    /* Status message (shown until next keypress) */
    char    status[STATUS_MAX];

    /* Wanted column for vertical movement */
    int     want_col;

    /* Count prefix for commands */
    int     count;

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
} Editor;

extern Editor ed;
extern char msg_insert[];

/* ---- gap.c ---- */
int  gb_init();
void gb_free();
int  gb_content_len();  /* logical content size (no gap) */
int  gb_char_at(/* int pos */);
int  find_bol(/* int pos */);
int  find_eol(/* int pos */);
int  gb_insert(/* int pos, char *text, int len */);
int  gb_delete(/* int pos, int len */);
int  gb_load(/* char *filename, FILE *fp */); /* fp=NULL -> fopen filename */
int  gb_save(/* char *filename */);
int  gb_load_more(/* int n */);
int  gb_reload_from(/* long offset */);

/* ---- term.c ---- */
void term_init();
void term_restore();
void term_clear();
void term_clreol();
void term_goto(/* int row, int col */);
void term_putch(/* int c */);
void term_puts(/* char *s */);
void term_flush();
int  term_getch();
void term_getsize(/* int *rows, int *cols */);
void term_bold();
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
void scr_scroll_to_cursor();
void scr_update_after_move();
void scr_adj();
void scr_after_edit();
int  scr_cur_line();
int  scr_pos_line();
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
