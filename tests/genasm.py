#!/usr/bin/env python3
"""Generate the OPTIM-stage assembly for a source file and save it."""
import sys, time, shutil
sys.path.insert(0, "/tmp/hvi-test")
from cpm import CPM

src = sys.argv[1].upper() if len(sys.argv) > 1 else "EDIT"

c = CPM()
c.expect_prompt()
time.sleep(0.3)
c.drain()
c.cmd("CPP -DCPM -DHI_TECH_C -Dz80 -I %s.C T1.TMP" % src)
c.cmd("P1 T1.TMP T2.TMP T3.TMP")
c.cmd("CGEN T2.TMP T1.TMP")
c.cmd("OPTIM T1.TMP T2.TMP")
c.close()
shutil.copy("/tmp/hvi-test/A/0/T2.TMP", "/tmp/hvi-test/%s.asm" % src.lower())
print("saved /tmp/hvi-test/%s.asm" % src.lower())
