# HVI test harness

End-to-end tests that build HVI with the real HI-TECH C V3.09 toolchain and
drive the editor through a pseudo-terminal inside RunCPM, verifying every
documented command by round-tripping files and screen-scraping a VT100
emulator (pyte).

## One-time environment setup

The harness expects a CP/M environment at `/tmp/hvi-test`:

```sh
R=~/Downloads/runcpm/run4          # RunCPM + HI-TECH C V3.09 live here
T=/tmp/hvi-test
mkdir -p $T/A/0
cp $R/RunCPM $T/
cp $R/A/0/{SUBMIT.COM,PIP.COM,STAT.COM,DDT.COM} $T/A/0/
cp $R/C/0/{CPP.COM,P1.COM,CGEN.COM,OPTIM.COM,ZAS.COM,L.COM,LIBR.COM,LIBC.LIB,CPM.H,HITECH.H} $T/A/0/
cp tests/*.py $T/
pip3 install --user pyte
```

## Running

```sh
cd /tmp/hvi-test
python3 build.py      # copies repo sources (LF->CRLF), compiles, links HVI.COM
python3 hvitest.py    # 125 functional tests (movement, edit, ex, undo, marks, screen)
python3 bigtest.py    # 13 large-file sliding-window and paging tests
python3 genasm.py X   # dump OPTIM-stage assembly for module X (debugging)
```

`build.py` runs the five compiler passes manually (CPP -> P1 -> CGEN ->
OPTIM -> ZAS) because RunCPM's internal CCP cannot exec the driver's
`$EXEC.COM` chain loader.  Temp files use `T1.TMP`-style names: anything
matching `$...$$$` is replayed by the CCP as a pending SUBMIT script
(CP/M stores submit lines in reverse record order) and wrecks the session.

The pty driver (`cpm.py`) answers HVI's `ESC[6n` terminal-size query with
`ESC[24;80R`, so all screen assertions assume an 80x24 terminal.
