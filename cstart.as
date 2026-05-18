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
    GLOBAL  _bios_conin, _bios_conin_addr

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
    ; HI-TECH C V3.09 standard csv frame layout (verified from compiled output):
    ;   [IX+0] = func_body_ptr  (pushed last, so at top of IX frame)
    ;   [IX+2] = caller's IX
    ;   [IX+4] = return address to caller
    ;   [IX+6] = first argument  (left-most in source)
    ;
    ; The compiler emits CALL csv immediately before the function body.
    ; The return address popped here IS the function body entry.
    ; RET re-pops func_body_ptr into PC, entering the function body.
    pop     hl              ; HL = func_body entry (return addr from CALL csv)
    push    ix              ; [SP] = caller's IX
    push    hl              ; [SP] = func_body_ptr  → IX+0 after add ix,sp
    ld      ix,0
    add     ix,sp           ; IX = SP: IX+0=func_body, IX+2=old_IX, IX+4=ret, IX+6=first_arg
    ret                     ; pops func_body_ptr into PC → enters function body

cret:
    ; Mirrors csv frame: IX+0=func_body_ptr, IX+2=old_IX.
    ; IMPORTANT: use pop bc (not pop hl) to discard func_body_ptr so that
    ; the return value already in HL is preserved for the caller.
    ld      sp,ix           ; restore SP to frame base (discards locals)
    pop     bc              ; discard func_body_ptr at IX+0 (must NOT clobber HL)
    pop     ix              ; restore caller's IX from IX+2
    ret                     ; return to caller with HL = return value

    ; ncsv: optimiser-emitted variant.  Follows CALL ncsv with DW -N (locals).
    ; Produces the same IX frame layout as csv so cret works for both.
ncsv:
    pop     hl              ; HL = ptr to DW -N (return addr from CALL ncsv)
    ld      c,(hl)
    inc     hl
    ld      b,(hl)          ; BC = -N (signed, so BC is negative)
    inc     hl              ; HL = function body (first real instruction)
    push    ix              ; save caller's IX
    push    hl              ; save func_body_ptr → IX+0 after add ix,sp
    ld      ix,0
    add     ix,sp           ; IX = SP: IX+0=func_body, IX+2=old_IX, IX+4=ret, IX+6=first_arg
    ld      hl,0
    add     hl,sp
    add     hl,bc           ; HL = SP - N  (allocate locals)
    ld      sp,hl
    ld      l,(ix+0)        ; reload func_body_ptr from frame
    ld      h,(ix+1)
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
    ld      ix,(_bdos_disk_ix)  ; (3) restore frame ptr from BSS
    pop     ix                  ; (4) restore caller's IX from stack
    ret

    ; --- bios_conin: BIOS CONIN call that preserves IX ---------------
    ;
    ; BIOS CONIN (function 3) is at BIOS_base + 9.
    ; BIOS_base = warmboot_addr - 3 = [0x0001] - 3.
    ; So CONIN = [0x0001] + 6 (warmboot + 6).
    ;
    ; No arguments.  Returns character in HL (H=0, L=char).
    ; Reuses _bdos_disk_ix as the IX save slot.
    ;
_bios_conin:
    push    ix
    ld      ix,0
    add     ix,sp
    ld      (_bdos_disk_ix),ix   ; save frame ptr (reuse disk save slot)

    ; Compute CONIN address = warmboot + 6
    ld      hl,1                  ; 0x0001 -- pure immediate, no reloc
    ld      e,(hl)                ; E = lo(warmboot)
    inc     hl
    ld      d,(hl)                ; D = hi(warmboot)  → DE = warmboot = BIOS+3
    ld      hl,6
    add     hl,de                 ; HL = BIOS+9 = CONIN vector address
    ld      (_bios_conin_addr),hl ; save for trampoline

    call    _bios_jmp             ; "call (HL)" via trampoline; char returned in A
    ld      l,a
    ld      h,0                   ; HL = char (return value, H=0)

    ld      ix,(_bdos_disk_ix)   ; restore frame ptr
    pop     ix                    ; restore caller's IX
    ret

    ; Trampoline: load saved CONIN address into HL and JP (HL).
    ; The CALL from _bios_conin leaves a return address on the stack, so
    ; CONIN's RET comes back here to the instruction after CALL _bios_jmp.
_bios_jmp:
    ld      hl,(_bios_conin_addr)
    jp      (hl)

    ; --- BSS section marker ------------------------------------------
    ; bss_begin must be in the BSS psect so the linker places it at
    ; the correct address (after text+data, not at 0x0000).
    PSECT   bss
bss_begin:
_bdos_disk_ix:  defs    2       ; save slot: IX value across CALL 5 / BIOS call
_bios_conin_addr: defs  2       ; CONIN vector address for _bios_jmp trampoline

    END     start           ; declare entry point -- without this the linker
                            ; defaults to 0, which is < 0x100 and triggers
                            ; "module has code below file base of 0100h"
