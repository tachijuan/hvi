# CLAUDE.md — working on HVI

## Project goals — apply to every change, unprompted

HVI is judged on three axes at once: **performance, capability, and
executable size**. Whenever a capability is added or changed, all three are
part of the deliverable — do not wait to be asked:

1. **Capability**: ship the feature complete across every surface it
   touches — normal-mode command, operator motion, dot-repeat, ex
   command/range, docs (`hvi.1`, `README.md`, `REQUIREMENTS.md`), and tests.
2. **Size**: run a size pass on the same change *before* declaring it done.
   Target net record growth of 0 whenever achievable; a feature that lands
   at +2 records is not finished. Report the exact byte cost from the link
   map, never just STAT's record count.
3. **Performance**: this is a Z80 running at 2–4 MHz. Judge hot paths by
   T-state reasoning, not harness wall time — RunCPM executes at host speed,
   so emulator timings hide real-hardware cost.

## Measuring size

- Relink with `-mHVI.MAP`; image bytes = `bss_begin` − 0x100. Records =
  ceil(bytes / 128). Measure per optimization round — batched "obvious"
  wins have measured negative before.
- `tests/genasm.py MODULE` dumps the OPTIM-stage assembly. Before
  hand-tuning, script-hunt repeated instruction windows across all module
  dumps — the biggest levers are found there, not by staring at C.
- BSS is free (not stored in the .COM); only text + data count.

## Size levers that repeatedly pay here (HI-TECH C V3.09)

- Dedupe repeated call/expression shapes into tiny helpers called with a
  3-byte `call` (e.g. `bol_cur`, `gb_tailing`, `mk_prev`, `status_fmt`).
  **Link order constraint**: the single-pass linker resolves calls into
  already-linked modules — helpers must live at or before their earliest
  caller. Order: CSTART CPMIO UTIL GAP TERM SCREEN EMOVE EREPEAT EX EDIT HVI.
- One shared copy of behavior beats per-call-site copies (put range fixups
  in `apply_op`, not at each caller).
- File-scope statics beat autos/params (3-byte absolute vs 6-byte
  IX-relative); copy hot params to statics only when read ≥4 times.
- `!= 0` / `!= 0L` beats signed compares on provably non-negative values;
  the long form pays double.
- HI-TECH does not pool identical string literals — share them as named
  char arrays across modules.

## Verifying

- Environment and build: `tests/README.md` (RunCPM at `/tmp/hvi-test`;
  sync sources with LF→CRLF; never run two test sessions concurrently).
- Every change: `hvitest.py` (functional) and `bigtest.py` (large-file)
  must pass 100%.
- Every release: `termtest.py --build` for all 8 non-ANSI families, rebuild
  the family binaries in `bin/` plus the root `HVI.COM`, and record the
  exact byte count in the README changelog.
