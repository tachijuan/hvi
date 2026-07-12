;
; cstart.as - Minimal CP/M Z80 startup for HVI
; Author: Juan Orlandini
; License: MIT
;
; IMPORTANT: all accesses to page-zero (addresses below 0x0100) use
; indirect register addressing rather than the LD HL,(nn) / LD A,(nn)
; direct-address forms.  The direct forms cause ZAS to emit absolute
; relocation records; the HI-TECH linker then rejects them with
; "module has code below file base of 0100h" because 0x0006/0x0080 <
; 0x0100 (the -C100H base).  LD HL,nn is a pure immediate (ZAS never
; emits a relocation record for it), so LD HL,6 / LD A,(HL) is safe.
;
; LINK using the linker directly.  The command is split across two lines
; using backslash continuation (the HI-TECH linker accepts unlimited-length
; commands when reading from stdin with \ at end of line):
;
;   l
;   -Ptext=100H,data,bss -C100H -oh.com CSTART.OBJ CPMIO.OBJ UTIL.OBJ \
;   GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EREPEAT.OBJ EX.OBJ EDIT.OBJ HVI.OBJ LX.LIB
;
;   Then rename the output:
;   REN HVI.COM=H.COM
;
; WHY -Ptext=100H,data,bss: the -C flag alone sets the binary file-base
; but does NOT chain data/bss after text when linking manually (only the
; C driver does this automatically).  Without explicit -P, the linker
; places data and bss at address 0, causing "code below file base of 0100h"
; for every psect-relative (type 0x12) relocation in static variables.
; -Ptext=100H,data,bss places text at 0x100 and concatenates data then bss
; immediately after, keeping all addresses above the file base.
;
; NOTE: bss does NOT occupy space in the .COM file (it is zero-initialised
; at runtime by this startup code).  All static variables must therefore be
; UNINITIALIZED (no = value) so they land in bss, not in data; initialized
; statics in the data psect would still be at the correct address but would
; consume .COM file space unnecessarily.
;
; WHY THIS ORDER: the HI-TECH linker is single-pass for object files.
; A CALL to a symbol not yet in the symbol table resolves to 0, which is
; below the -C file base of 0x0100 and triggers "code below file base".
; Therefore every module must appear AFTER all modules it calls:
;   CPMIO, UTIL   -- no deps on our modules (only LX.LIB bdos/bios)
;   GAP           -- calls CPMIO + UTIL
;   TERM          -- no deps on our modules
;   SCREEN        -- calls TERM + GAP + UTIL
;   EMOVE         -- calls GAP + SCREEN + UTIL
;   EREPEAT       -- calls GAP
;   EX            -- calls CPMIO + GAP + SCREEN + UTIL
;   EDIT          -- calls SCREEN + TERM + GAP + EMOVE + EREPEAT + EX
;   HVI (main)    -- calls everything above
;   LX.LIB        -- library scan resolves remaining runtime symbols
;
; WHY LX.LIB NOT LIBC.LIB: LIBC.LIB contains csv.obj which also defines
; csv and cret, causing "multiply defined symbol" (fatal -- zero-byte output).
; LX.LIB is a copy of LIBC.LIB with csv.obj removed via LIBR:
;   PIP LX.LIB=LIBC.LIB
;   LIBR d LX.LIB csv.obj

    PSECT   text
    GLOBAL  start, _main, __Hbss, bss_begin, _bdos_base, _heap_base
    GLOBAL  csv, cret, ncsv, indir
    GLOBAL  _bdos_disk, _bdos_disk_ix
    GLOBAL  _gb_memmove
    GLOBAL  _con_write, _gb_cntnl
    GLOBAL  _gb_char_at, _gb_memchr, _gb_memrchr, _ed
    GLOBAL  _dsk_u
    GLOBAL  _gb_insert_room, _gb_insert, _gb_make_room
    GLOBAL  _gb_roomed, _gir_pos, _gir_text, _gir_len
    GLOBAL  _fill_fcb, _fcb_user, _fcb_drive

; Editor-struct field offsets used by _gb_char_at.  GapBuf is the FIRST
; member of Editor (hvi.h) so its fields sit at the start of _ed:
;   _ed+0 = gb.buf   _ed+2 = gb.size   _ed+4 = gb.gstart   _ed+6 = gb.gend
; If GapBuf stops being the first member or its fields are reordered,
; these EQUs must be updated to match.
GBBUF   equ 0
GBSIZE  equ 2
GBGST   equ 4
GBGND   equ 6
; _gb_insert_room also reads/writes ed.cur_pos at _ed+74
; (= 8 GapBuf + 64 filename[PATH_MAX] + 2 modified).  If PATH_MAX or
; the Editor members before cur_pos change, update EDCURP (hvi.h has a
; matching reminder at the cur_pos field).
EDCURP  equ 74

start:
    ; --- Stack setup -------------------------------------------------
    ; Read the BDOS base address from page-zero 0x0006-0x0007.
    ; Use LD HL,6 (pure immediate, no relocation) + indirect LD A,(HL).
    ld      hl,6            ; HL = 0x0006 -- immediate, NOT a memory ref
    ld      a,(hl)          ; A  = *(0x0006) = BDOS address low byte
    inc     hl              ; HL = 0x0007
    ld      h,(hl)          ; H  = *(0x0007) = BDOS address high byte
    ld      l,a             ; L  = BDOS address low byte
    ld      sp,hl           ; SP = BDOS base (top of TPA)
    push    hl              ; save BDOS -- must store AFTER BSS zeroing
                            ; (bdos_base is in BSS; zeroing would overwrite an
                            ;  early store)

    ; --- Zero BSS ----------------------------------------------------
    ; Compute size = __Hbss - bss_begin, then fill with the LDIR trick.
    ld      hl,__Hbss       ; symbol ref -- resolves >= 0x0100, fine
    ld      de,bss_begin    ; symbol ref -- resolves >= 0x0100, fine
    xor     a               ; clear carry for SBC
    sbc     hl,de           ; HL = BSS size
    ld      b,h
    ld      c,l             ; BC = BSS size
    ld      a,b
    or      c
    jp      z,store_bdos    ; no BSS
    ld      hl,bss_begin
    ld      (hl),0          ; zero first byte
    dec     bc
    ld      a,b
    or      c
    jp      z,store_bdos    ; only one byte
    ld      d,h
    ld      e,l
    inc     de              ; DE = bss_begin + 1
    ldir                    ; propagate zero through rest of BSS

    ; --- Store BDOS base for cpmio.c hvi_malloc ----------------------
    ; BSS is now zeroed; pop the saved BDOS value and write it to the
    ; bdos_base global.  _bdos_base resolves to an address >= 0x0100
    ; (it is a BSS variable in cpmio.c), so this relocation record is
    ; accepted by the linker.
store_bdos:
    pop     hl              ; HL = BDOS base (saved before BSS zeroing)
    ld      (_bdos_base),hl ; store for cpmio.c hvi_malloc
    ld      hl,__Hbss       ; heap starts right after BSS
    ld      (_heap_base),hl ; store for cpmio.c hvi_malloc

    ; --- Call main ---------------------------------------------------
    ; Pass the CP/M command tail as two arguments:
    ;   main(cmdlen, cmdtail)
    ;   cmdlen  = byte at 0x0080 (tail char count)
    ;   cmdtail = pointer 0x0081 (tail text start)
    ;
    ; Read from page zero with indirect addressing only.
    ; LD HL,081H is a pure immediate (no relocation); LD A,(HL) is
    ; indirect (no absolute reference either).  HI-TECH C cdecl:
    ; push last argument first (right to left).
call_main:
    ld      hl,081h         ; cmdtail = 0x0081 -- pure immediate
    push    hl              ; arg2: pointer to command tail text

    ld      hl,080h         ; HL = 0x0080 -- pure immediate
    ld      a,(hl)          ; A  = *(0x0080) = cmdlen -- indirect, safe
    ld      l,a
    ld      h,0
    push    hl              ; arg1: cmdlen (zero-extended to int)

    call    _main

    pop     bc              ; discard cmdlen
    pop     bc              ; discard cmdtail

    ; --- Warm boot ---------------------------------------------------
    ; Use LD HL,0 + JP (HL) instead of JP 0 to avoid emitting a
    ; relocation record for the absolute address 0x0000.
    ld      hl,0            ; pure immediate -- no relocation record
    jp      (hl)            ; indirect jump to 0x0000 (CP/M warm boot)

    ; --- C function prologue / epilogue ------------------------------
    ; csv and cret MUST be defined here (in CSTART.OBJ, before any C
    ; .OBJ files) because the HI-TECH linker resolves symbols single-pass.
    ; When it processes HVI.OBJ the call to csv must already be in the
    ; symbol table; LIBC.LIB comes later and is too late.
    ;
    ; LIBC.LIB also contains a csv.obj with the same symbols.  The linker
    ; will report "multiply defined symbol" for csv and cret, but this is
    ; a WARNING only -- the first definition (from cstart.as) wins and the
    ; binary is correctly produced.  If the linker treats it as fatal, use
    ; LIBR to remove csv.obj from a copy of LIBC.LIB:
    ;   PIP LX.LIB=LIBC.LIB
    ;   LIBR d LX.LIB csv
    ; then link with LX.LIB instead of LIBC.LIB.
    ;
    ; HI-TECH C Z80 calling convention (V3.09):
    ;
    ; The compiler emits a DW -N word IMMEDIATELY after CALL csv/ncsv:
    ;
    ;   function_name:
    ;       CALL csv        ; (or CALL ncsv)
    ;       DW   -N         ; N = bytes of local variable space to allocate
    ;       ; actual function body starts here
    ;
    ; Arguments: pushed right-to-left by caller; caller pops them after return.
    ; Return value: in HL.
    ; Frame pointer: IX points at saved caller's IX on the stack.
    ;   IX+0  = saved caller's IX
    ;   IX+2  = return address
    ;   IX+4  = first argument (left-most in source)
    ;   IX-2  = first local variable
    ;   IX-4  = second local variable, etc.
    ;
    ; csv and ncsv: read DW -N, save IX, set IX = SP (frame base),
    ;   allocate N bytes for locals (SP += -N = SP -= N), jump to body.
    ;
    ; cret: restores SP from IX (discarding locals), pops IX, returns.

csv:
    ; HI-TECH C V3.09 csv frame layout (matches compiled code, which reads
    ; the first argument at IX+6 and expects SP == IX at body entry):
    ;   [IX+0] = caller's IX
    ;   [IX+2] = caller's IY
    ;   [IX+4] = return address to caller
    ;   [IX+6] = first argument  (left-most in source)
    ;
    ; SP == IX at body entry is CRITICAL: functions with small frames follow
    ; CALL csv with one PUSH HL per two bytes of locals, and the compiler
    ; addresses those locals at IX-1 downward.  If csv left SP above IX
    ; (as an earlier version of this file did, by pushing a body pointer
    ; that RET later consumed), the deepest local bytes would sit BELOW SP
    ; and be silently overwritten by the next argument push -- e.g. the
    ; 'r' command inserted NUL instead of the replacement character.
    pop     hl              ; HL = func_body entry (return addr from CALL csv)
    push    iy              ; [IX+2] caller's IY
    push    ix              ; [IX+0] caller's IX
    ld      ix,0
    add     ix,sp           ; IX = frame base; SP == IX
    jp      (hl)            ; enter function body

cret:
    ; Mirrors csv/ncsv frame: IX+0=old_IX, IX+2=old_IY.
    ; Must NOT clobber HL (function return value).
    ld      sp,ix           ; restore SP to frame base (discards locals)
    pop     ix              ; restore caller's IX
    pop     iy              ; restore caller's IY
    ret                     ; return to caller with HL = return value

    ; ncsv: optimiser-emitted variant.  Follows CALL ncsv with DW -N (locals).
    ; Produces the same frame layout as csv (SP = IX - N after allocation)
    ; so cret works for both.
ncsv:
    pop     hl              ; HL = ptr to DW -N (return addr from CALL ncsv)
    ld      e,(hl)
    inc     hl
    ld      d,(hl)          ; DE = -N (negative local size)
    inc     hl              ; HL = function body (first real instruction)
    push    iy              ; [IX+2] caller's IY
    push    ix              ; [IX+0] caller's IX
    ld      ix,0
    add     ix,sp           ; IX = frame base
    ex      de,hl           ; HL = -N, DE = body
    add     hl,sp           ; HL = SP - N  (allocate locals)
    ld      sp,hl
    ex      de,hl           ; HL = body
    jp      (hl)            ; jump to function body

    ; indir: indirect call through a function pointer.
    ; The caller loads the target address into HL then falls through or
    ; jumps here; JP (HL) transfers control to the pointed-to function.
indir:
    jp      (hl)

    ; --- bdos_disk: BDOS call that preserves IX around CALL 5 --------
    ;
    ; CP/M disk BDOS functions (15,16,19,20,21,22,23,26,33) corrupt the
    ; IX register in RunCPM.  The HI-TECH C CRET epilogue uses IX to
    ; restore SP ("ld sp,ix"), so a corrupted IX crashes the caller.
    ;
    ; This wrapper saves IX to _bdos_disk_ix (a BSS word) before CALL 5
    ; and restores it afterwards.  BDOS does not modify memory (only
    ; registers), so the BSS save slot is safe across the call.
    ;
    ; Calling convention: same cdecl as bdos() in LX.LIB.
    ;   Stack on entry: [SP+0]=ret_addr  [SP+2]=fn  [SP+4]=arg
    ;   Returns: BDOS result in HL.
    ;
    ; Frame after "push ix":
    ;   IX+0 = saved caller's IX
    ;   IX+2 = return address
    ;   IX+4 = fn  (BDOS function number, low byte into C)
    ;   IX+6 = arg (low byte into E, high byte into D)
    ;
_bdos_disk:
    push    ix                  ; (1) save caller's IX to stack
    ld      ix,0
    add     ix,sp               ; IX = frame base
    ld      c,(ix+4)            ; C  = BDOS function number
    ld      e,(ix+6)            ; DE = argument
    ld      d,(ix+7)
    ld      (_bdos_disk_ix),ix  ; (2) save frame ptr to BSS before CALL 5
    call    5                   ; BDOS -- may corrupt IX
    ld      l,a                 ; return A zero-extended: A is the only
    ld      h,0                 ;  DRI-guaranteed byte result (H mirrors
                                ;  B, which real BDOSes leave as junk --
                                ;  RunCPM returns clean HL, so unmasked
                                ;  C-side checks passed only there)
    ld      ix,(_bdos_disk_ix)  ; (3) restore frame ptr from BSS
    pop     ix                  ; (4) restore caller's IX from stack
    ret

    ; --- gb_memmove: overlap-safe block move using LDIR/LDDR ---------
    ;
    ; void gb_memmove(char *dst, char *src, int len)
    ;
    ; The gap buffer moves up to BUF_MAX bytes every time the gap jumps
    ; to a distant cursor position.  LDIR moves a byte in 21 T-states;
    ; the equivalent compiled C loop needs well over 100, so this is the
    ; single biggest CPU win for editing large files on a 4 MHz Z80.
    ;
    ; cdecl frame after push ix / add ix,sp:
    ;   IX+4 = dst   IX+6 = src   IX+8 = len
    ;
_gb_memmove:
    push    ix
    ld      ix,0
    add     ix,sp
    ld      e,(ix+4)        ; DE = dst
    ld      d,(ix+5)
    ld      l,(ix+6)        ; HL = src
    ld      h,(ix+7)
    ld      c,(ix+8)        ; BC = len
    ld      b,(ix+9)
    ld      a,b
    or      c
    jp      z,gmm_done      ; len == 0
    ld      a,l             ; unsigned compare src - dst
    sub     e
    ld      a,h
    sbc     a,d             ; carry set when src < dst
    jp      c,gmm_back
    ldir                    ; dst <= src: forward copy is overlap-safe
    jp      gmm_done
gmm_back:
    add     hl,bc
    dec     hl              ; HL = src + len - 1
    ex      de,hl
    add     hl,bc
    dec     hl              ; HL = dst + len - 1
    ex      de,hl
    lddr                    ; dst > src: backward copy
gmm_done:
    pop     ix
    ret

    ; --- con_write: block console output via BDOS 6 -------------------
    ;
    ; void con_write(char *buf, int len)
    ;
    ; Writes len bytes with BDOS function 6 (Direct Console I/O; E < FFh
    ; means output).  Replaces one C-level bdos_disk(2,c) call per byte
    ; in term_flush: the per-character argument pushes, csv frame, and
    ; IX save/restore are gone, and fn 6 also skips fn 2's ^S/^P status
    ; polling.  IX is saved once around the whole loop.  BDOS preserves
    ; no registers, so the walking pointer and count live in BSS slots.
    ;
    ; cdecl frame after push ix / add ix,sp:
    ;   IX+4 = buf   IX+6 = len
    ;
_con_write:
    push    ix
    ld      ix,0
    add     ix,sp
    ld      l,(ix+4)            ; HL = buf
    ld      h,(ix+5)
    ld      c,(ix+6)            ; BC = len
    ld      b,(ix+7)
cw_loop:
    ld      a,b
    or      c
    jp      z,cw_done
    dec     bc
    ld      (cw_cnt),bc
    ld      e,(hl)              ; E = next byte (never FFh: HVI emits
    inc     hl                  ;     ASCII + ESC sequences only)
    ld      (cw_ptr),hl
    ld      c,6                 ; BDOS 6: direct console I/O (output)
    call    5
    ld      hl,(cw_ptr)
    ld      bc,(cw_cnt)
    jp      cw_loop
cw_done:
    pop     ix
    ret

    ; --- gb_cntnl: count 0Ah bytes in a raw memory range --------------
    ;
    ; int gb_cntnl(char *p, int len)
    ;
    ; CPIR scans at 21 T-states/byte versus ~150+ for a compiled
    ; gb_char_at() loop, so full-buffer line counting drops from seconds
    ; to ~0.13s at 4 MHz.  Backs gb_count_nl() in gap.c (line counting,
    ; scr_pos_line, delete/discard newline bookkeeping).  No BDOS calls,
    ; so registers only; IX is preserved for the caller.
    ;
    ; cdecl frame after push ix / add ix,sp:
    ;   IX+4 = p   IX+6 = len
    ;
_gb_cntnl:
    push    ix
    ld      ix,0
    add     ix,sp
    ld      l,(ix+4)            ; HL = p
    ld      h,(ix+5)
    ld      c,(ix+6)            ; BC = len
    ld      b,(ix+7)
    ld      de,0                ; DE = newline count
    ld      a,b
    or      c
    jp      z,cnl_done
cnl_scan:
    ld      a,0ah               ; scan target: LF (reload every pass --
    cpir                        ;   the BC!=0 test below clobbers A)
    jp      nz,cnl_done         ; range exhausted, last byte not LF
    inc     de                  ; counted one newline
    ld      a,b
    or      c
    jp      nz,cnl_scan
cnl_done:
    ex      de,hl               ; return count in HL
    pop     ix
    ret

    ; --- gb_insert_room: block insert with room-making -----------------
    ; int gb_insert_room(void)
    ; Arguments in globals (set by the caller): _gir_pos / _gir_text /
    ; _gir_len.  Inserts min(gap, len) bytes at a time via _gb_insert;
    ; when the gap is exhausted it saves the insertion point in
    ; ed.cur_pos, calls _gb_make_room (window swap: sets _gb_roomed so
    ; the caller repaints and skips the invalidated undo record), and
    ; continues at the re-mapped ed.cur_pos.  Returns HL = position
    ; just past the inserted text (== final gir_pos), or -1 with
    ; gir_pos = -1 when make_room fails (disk full / I/O error).  A
    ; negative gir_pos on entry returns -1 immediately, so chained
    ; calls propagate an earlier failure and need only one check.
    ; Terminates: a successful make_room leaves a gap >= GAP_MIN
    ; (gb_fill stops at size - GAP_MIN), so every pass makes progress.
_gb_insert_room:
    ld      hl,(_gir_pos)
    bit     7,h
    jr      nz,gib_bad          ; negative pos: propagate failure
gib_loop:
    ld      hl,(_ed+GBGND)      ; n = gap = gend - gstart  (>= 0)
    ld      de,(_ed+GBGST)
    or      a
    sbc     hl,de
    ld      de,(_gir_len)       ; if n > len then n = len
    ld      a,h                 ;   (unsigned compare: both < 32768)
    cp      d
    jr      c,gib_n             ; gap < len: n = gap
    jr      nz,gib_len          ; gap > len: n = len
    ld      a,l
    cp      e
    jr      c,gib_n
gib_len:
    ex      de,hl
gib_n:
    push    hl                  ; save n
    push    hl                  ; gb_insert(gir_pos, gir_text, n)
    ld      hl,(_gir_text)
    push    hl
    ld      hl,(_gir_pos)
    push    hl
    call    _gb_insert          ; n <= gap: cannot fail
    pop     bc
    pop     bc
    pop     bc
    pop     de                  ; DE = n
    ld      hl,(_gir_pos)       ; gir_pos += n
    add     hl,de
    ld      (_gir_pos),hl
    ld      hl,(_gir_text)      ; gir_text += n
    add     hl,de
    ld      (_gir_text),hl
    ld      hl,(_gir_len)       ; gir_len -= n
    or      a
    sbc     hl,de
    ld      (_gir_len),hl
    ld      a,h                 ; n was clamped to len, so == 0 is exact
    or      l
    jr      nz,gib_more
    ld      hl,(_gir_pos)       ; done: return past-the-end position
    ret
gib_more:
    ld      hl,(_gir_pos)       ; carry insertion point across the swap
    ld      (_ed+EDCURP),hl
    call    _gb_make_room
    ld      a,h
    or      l
    jr      z,gib_bad
    ld      a,1
    ld      (_gb_roomed),a
    ld      hl,(_ed+EDCURP)
    ld      (_gir_pos),hl
    jr      gib_loop
gib_bad:
    ld      hl,-1
    ld      (_gir_pos),hl
    ret

    ; --- dsk_u: bdos_disk within a file's user area --------------------
    ;
    ; int dsk_u(int fn, int arg, int user)
    ;
    ; bdos_disk() bracketed by BDOS 32 user-area switches: user 0-15
    ; enters that user area for the call and returns to the process
    ; user afterwards; user < 0 (no du: prefix) calls straight through
    ; with no BDOS 32 traffic at all.  The process user number is
    ; fetched once, lazily (du_cur starts as 80h = unknown; it is a
    ; data-psect byte because BSS zeroing would read as "user 0").
    ; All BDOS traffic goes through _bdos_disk, which preserves IX.
    ;
_dsk_u:
    push    ix
    ld      ix,0
    add     ix,sp               ; IX+4 fn, IX+6 arg, IX+8 user
    ld      a,(ix+9)
    rla                         ; user sign bit -> carry
    jp      c,dsku_op           ; user < 0: no switching
    ld      a,(du_cur)
    rla
    jp      nc,dsku_have        ; process user already known
    ld      hl,0ffh
    push    hl
    ld      hl,32
    push    hl
    call    _bdos_disk          ; BDOS 32: get current user
    pop     bc
    pop     bc
    ld      a,l
    ld      (du_cur),a
dsku_have:
    ld      a,(du_cur)
    cp      (ix+8)
    jp      z,dsku_op           ; file lives in the process user area
    ld      l,(ix+8)
    ld      h,0
    push    hl
    ld      hl,32
    push    hl
    call    _bdos_disk          ; BDOS 32: enter file's user area
    pop     bc
    pop     bc
    ld      e,(ix+6)
    ld      d,(ix+7)
    push    de
    ld      l,(ix+4)
    ld      h,(ix+5)
    push    hl
    call    _bdos_disk          ; the actual disk operation
    pop     bc
    pop     bc
    push    hl                  ; save its result
    ld      a,(du_cur)
    ld      l,a
    ld      h,0
    push    hl
    ld      hl,32
    push    hl
    call    _bdos_disk          ; BDOS 32: back to the process user
    pop     bc
    pop     bc
    pop     hl                  ; result of the operation
    pop     ix
    ret
dsku_op:
    ld      e,(ix+6)
    ld      d,(ix+7)
    push    de
    ld      l,(ix+4)
    ld      h,(ix+5)
    push    hl
    call    _bdos_disk
    pop     bc
    pop     bc
    pop     ix
    ret

    ; --- fill_fcb: build a 36-byte FCB from "du:NAME.EXT" --------------
    ;
    ; void fill_fcb(unsigned char *fcb, char *name)
    ;
    ; Parses the optional ZCPR "du:" prefix -- drive letter A-P/a-p,
    ; user number 0-15, terminating ':' -- then fills the FCB: drive
    ; code in byte 0, name/ext upper-cased and space-padded, all other
    ; bytes zero.  An invalid prefix (user > 15, 3+ digits, or no ':')
    ; leaves the whole string as the filename, exactly like the
    ; retired C version in cpmio.c.
    ;
    ; Results for the C side (both ints defined in cpmio.c):
    ;   _fcb_user  = parsed user area, -1 when none given
    ;   _fcb_drive = FCB drive code (0 = current, 1 = A ...); hvi_fopen
    ;                re-asserts it into FCB byte 0 after BDOS open.
    ;
_fill_fcb:
    push    ix
    ld      ix,0
    add     ix,sp               ; IX+4 fcb, IX+6 name
    ld      hl,0ffffh
    ld      (_fcb_user),hl      ; user = -1 (current)
    ld      hl,0
    ld      (_fcb_drive),hl     ; drive = 0 (current)
    ; ---- parse prefix into B (drive code) / C (user; FFh = none)
    ld      l,(ix+6)
    ld      h,(ix+7)            ; HL = name scan pointer
    ld      b,0
    ld      c,0ffh
    ld      a,(hl)
    call    ffb_upper
    cp      'A'
    jp      c,ffb_digits
    cp      'P'+1
    jp      nc,ffb_digits
    sub     'A'-1               ; drive code 1..16
    ld      b,a
    inc     hl
ffb_digits:
    ld      a,(hl)
    sub     '0'
    jp      c,ffb_colon
    cp      10
    jp      nc,ffb_colon
    ld      c,a                 ; first user digit
    inc     hl
    ld      a,(hl)
    sub     '0'
    jp      c,ffb_colon
    cp      10
    jp      nc,ffb_colon
    ld      e,a                 ; second digit: user = c*10 + e
    ld      a,c
    add     a,a
    ld      d,a                 ; d = 2c
    add     a,a
    add     a,a                 ; a = 8c
    add     a,d                 ; a = 10c
    add     a,e
    ld      c,a
    inc     hl
    ld      a,(hl)              ; a third digit invalidates the prefix
    sub     '0'
    jp      c,ffb_colon
    cp      10
    jp      c,ffb_nopfx
ffb_colon:
    ld      a,(hl)
    cp      ':'
    jp      nz,ffb_nopfx
    ld      a,c
    cp      0ffh
    jp      z,ffb_commit        ; "B:" form -- no user digits
    cp      16
    jp      nc,ffb_nopfx        ; user 16-99: not a prefix
ffb_commit:
    inc     hl                  ; consume the ':'
    ld      a,b
    ld      (_fcb_drive),a      ; low byte; high byte still 0
    ld      a,c
    cp      0ffh
    jp      z,ffb_build         ; no user digit: _fcb_user stays -1
    ld      (_fcb_user),a
    xor     a
    ld      (_fcb_user+1),a
    jp      ffb_build
ffb_nopfx:
    ld      l,(ix+6)
    ld      h,(ix+7)            ; rewind: whole string is the name
    ; ---- build the FCB (DE = name text, HL = fcb)
ffb_build:
    ex      de,hl
    ld      l,(ix+4)
    ld      h,(ix+5)
    push    hl
    ld      b,36                ; zero all 36 bytes
    xor     a
ffb_z:
    ld      (hl),a
    inc     hl
    djnz    ffb_z
    pop     hl
    push    hl
    ld      a,(_fcb_drive)
    ld      (hl),a              ; byte 0 = drive code
    inc     hl
    ld      b,11                ; blank the name+ext fields
ffb_sp:
    ld      (hl),' '
    inc     hl
    djnz    ffb_sp
    pop     hl
    inc     hl                  ; HL = &fcb[1]
    ld      b,8                 ; up to 8 name chars, stop at '.'/NUL
ffb_n:
    ld      a,(de)
    or      a
    jp      z,ffb_done
    cp      '.'
    jp      z,ffb_dot
    call    ffb_upper
    ld      (hl),a
    inc     hl
    inc     de
    djnz    ffb_n
ffb_skip:
    ld      a,(de)              ; skip extra name chars before the dot
    or      a
    jp      z,ffb_done
    cp      '.'
    jp      z,ffb_dot
    inc     de
    jp      ffb_skip
ffb_dot:
    inc     de                  ; skip the '.'
    ld      l,(ix+4)
    ld      h,(ix+5)
    ld      bc,9
    add     hl,bc               ; HL = &fcb[9] (extension field)
    ld      b,3
ffb_e:
    ld      a,(de)
    or      a
    jp      z,ffb_done
    call    ffb_upper
    ld      (hl),a
    inc     hl
    inc     de
    djnz    ffb_e
ffb_done:
    pop     ix
    ret

ffb_upper:                      ; fold a-z in A to upper case
    cp      'a'
    ret     c
    cp      'z'+1
    ret     nc
    sub     20h
    ret

    ; --- gb_char_at: logical byte fetch through the gap ---------------
    ;
    ; int gb_char_at(int pos)
    ;
    ; Returns the unsigned byte at logical position pos, or -1 when pos
    ; is out of range.  Mirrors the retired C version in gap.c exactly:
    ; pos < 0 -> -1; pos < gstart -> buf[pos]; otherwise raw = pos +
    ; gend - gstart and raw >= size -> -1, else buf[raw].
    ;
    ; This is the hottest function in the editor -- every per-character
    ; scanner (next_vrow, col_from, word motions, search, row painting)
    ; funnels through it -- so it is frameless: the argument is read
    ; relative to SP and IX/IY are never touched.
    ;
_gb_char_at:
    ld      hl,2
    add     hl,sp
    ld      a,(hl)
    inc     hl
    ld      h,(hl)
    ld      l,a             ; HL = pos
    bit     7,h
    jp      nz,gca_m1       ; pos < 0
    ld      de,(_ed+GBGST)  ; DE = gstart
    or      a
    sbc     hl,de           ; HL = pos - gstart; carry: pos < gstart
    jp      c,gca_dir
    ld      de,(_ed+GBGND)
    add     hl,de           ; HL = raw = pos - gstart + gend
    ld      de,(_ed+GBSIZE)
    or      a
    sbc     hl,de           ; carry: raw < size
    jp      nc,gca_m1       ; raw >= size -> out of range
    add     hl,de           ; restore raw
    jp      gca_ld
gca_dir:
    add     hl,de           ; restore pos (raw index == pos)
gca_ld:
    ld      de,(_ed+GBBUF)
    add     hl,de
    ld      l,(hl)
    ld      h,0             ; zero-extend the byte
    ret
gca_m1:
    ld      hl,-1
    ret

    ; --- gb_memchr: CPIR index-of over a raw memory range -------------
    ;
    ; int gb_memchr(char *p, int len, int c)
    ;
    ; Returns the index of the first byte equal to c in p[0..len), or
    ; -1 when absent or len <= 0.  21 T-states/byte versus ~300 for the
    ; compiled gb_char_at loop it replaces -- backs find_eol() in gap.c
    ; (and through it j/k, dd, line_span, scr_line_start, :N).
    ;
    ; cdecl args at SP+2/+4/+6; frameless, IX/IY untouched.
    ;
_gb_memchr:
    ld      hl,2
    add     hl,sp
    ld      e,(hl)
    inc     hl
    ld      d,(hl)          ; DE = p
    inc     hl
    ld      c,(hl)
    inc     hl
    ld      b,(hl)          ; BC = len
    inc     hl
    ld      a,b
    or      c
    jp      z,mch_no        ; len == 0
    bit     7,b
    jp      nz,mch_no       ; len < 0
    ld      a,(hl)          ; A = search byte
    ex      de,hl           ; HL = p
    push    hl              ; save p for the index calculation
    cpir
    pop     de              ; DE = p
    jp      nz,mch_no       ; range exhausted, last byte not c
    or      a
    sbc     hl,de
    dec     hl              ; index = (HL_after_match - 1) - p
    ret
mch_no:
    ld      hl,-1
    ret

    ; --- gb_memrchr: CPDR last-index-of over a raw memory range -------
    ;
    ; int gb_memrchr(char *p, int len, int c)
    ;
    ; Returns the index of the LAST byte equal to c in p[0..len), or
    ; -1 when absent or len <= 0.  Backward CPDR scan -- backs
    ; find_bol() in gap.c, the other half of every line motion.
    ;
    ; cdecl args at SP+2/+4/+6; frameless, IX/IY untouched.
    ;
_gb_memrchr:
    ld      hl,2
    add     hl,sp
    ld      e,(hl)
    inc     hl
    ld      d,(hl)          ; DE = p
    inc     hl
    ld      c,(hl)
    inc     hl
    ld      b,(hl)          ; BC = len
    inc     hl
    ld      a,b
    or      c
    jp      z,mrc_no        ; len == 0
    bit     7,b
    jp      nz,mrc_no       ; len < 0
    ld      a,(hl)          ; A = search byte
    ld      h,d
    ld      l,e
    add     hl,bc
    dec     hl              ; HL = p + len - 1 (last byte)
    cpdr
    jp      nz,mrc_no       ; range exhausted, first byte not c
    inc     hl              ; CPDR left HL = match address - 1
    or      a
    sbc     hl,de           ; index = match address - p
    ret
mrc_no:
    ld      hl,-1
    ret

    ; --- initialised data --------------------------------------------
    PSECT   data
du_cur: defb    80h             ; process user area; 80h = not yet fetched

    ; --- BSS section marker ------------------------------------------
    ; bss_begin must be in the BSS psect so the linker places it at
    ; the correct address (after text+data, not at 0x0000).
    PSECT   bss
bss_begin:
_bdos_disk_ix:  defs    2       ; save slot: IX value across CALL 5
cw_ptr:         defs    2       ; con_write: walking buffer pointer
cw_cnt:         defs    2       ; con_write: remaining byte count

    END     start           ; declare entry point -- without this the linker
                            ; defaults to 0, which is < 0x100 and triggers
                            ; "module has code below file base of 0100h"
