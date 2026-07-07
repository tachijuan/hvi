#!/usr/bin/env python3
"""Minimal pty driver for RunCPM.

Usage as a library:
    c = CPM()
    c.expect_prompt()           # wait for CCP prompt like "A0>"
    c.cmd("DIR")                # run a command, return its output
    c.send("...")               # raw keystrokes (for HVI testing)
    c.close()

Automatically answers the ANSI cursor-position query ESC[6n with
ESC[24;80R so HVI sees a 24x80 terminal.
"""
import os, pty, select, subprocess, sys, time, re, signal

RUNCPM_DIR = "/tmp/hvi-test"

class CPM:
    def __init__(self, log=None):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(
            ["./RunCPM"], cwd=RUNCPM_DIR,
            stdin=slave, stdout=slave, stderr=slave,
            close_fds=True, preexec_fn=os.setsid)
        os.close(slave)
        self.buf = b""       # everything received
        self.mark = 0        # consumed watermark for expect()
        self.log = log

    def _pump(self, timeout=0.05):
        """Read whatever is available; answer ESC[6n size queries."""
        got = False
        while True:
            r, _, _ = select.select([self.master], [], [], timeout)
            if not r:
                return got
            try:
                data = os.read(self.master, 4096)
            except OSError:
                return got
            if not data:
                return got
            self.buf += data
            got = True
            if self.log:
                self.log.write(data)
                self.log.flush()
            # answer any cursor-position queries
            n = self.buf.count(b"\x1b[6n", self.mark)
            # only answer new ones: track answered count
            if not hasattr(self, "_answered"):
                self._answered = 0
            total = self.buf.count(b"\x1b[6n")
            while self._answered < total:
                os.write(self.master, b"\x1b[24;80R")
                self._answered += 1
            timeout = 0.05

    def expect(self, pattern, timeout=15):
        """Wait until regex `pattern` (bytes) appears after the watermark."""
        pat = re.compile(pattern if isinstance(pattern, bytes)
                         else pattern.encode())
        end = time.time() + timeout
        while True:
            m = pat.search(self.buf, self.mark)
            if m:
                out = self.buf[self.mark:m.end()]
                self.mark = m.end()
                return out
            if time.time() > end:
                raise TimeoutError(
                    "expect %r timed out; tail=%r" %
                    (pattern, self.buf[-400:]))
            self._pump(0.1)

    def expect_prompt(self, timeout=30):
        # CCP prompt: drive+user like "A0>" at line start
        return self.expect(rb"[A-P]\d?>", timeout)

    def send(self, s, delay=0.0):
        data = s if isinstance(s, bytes) else s.encode()
        for ch in data:
            os.write(self.master, bytes([ch]))
            if delay:
                time.sleep(delay)

    def sendline(self, s=""):
        self.send(s)
        self.send("\r")

    def cmd(self, s, timeout=120, quiet=0.4):
        """Run a CCP command; wait until output is quiet AND ends at a
        CCP prompt.  Robust against warm-boot banners emitting extra
        prompts mid-stream."""
        self.send(s, delay=0.002)
        self.send("\r")
        end = time.time() + timeout
        prompt = re.compile(rb"[A-P]\d?>\s*$")
        start_mark = self.mark
        deadline = time.time() + quiet
        last = len(self.buf)
        while time.time() < end:
            self._pump(0.05)
            if len(self.buf) != last:
                last = len(self.buf)
                deadline = time.time() + quiet
                continue
            if time.time() >= deadline and prompt.search(self.buf[-8:]):
                self.mark = len(self.buf)
                return self.buf[start_mark:]
        raise TimeoutError("cmd %r timed out; tail=%r" % (s, self.buf[-400:]))

    def drain(self, quiet=0.3, maxwait=10):
        """Read until output is quiet for `quiet` seconds."""
        end = time.time() + maxwait
        last = len(self.buf)
        deadline = time.time() + quiet
        while time.time() < end:
            self._pump(0.05)
            if len(self.buf) != last:
                last = len(self.buf)
                deadline = time.time() + quiet
            elif time.time() > deadline:
                break
        out = self.buf[self.mark:]
        self.mark = len(self.buf)
        return out

    def close(self):
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        except Exception:
            pass
        try:
            os.close(self.master)
        except Exception:
            pass

if __name__ == "__main__":
    # smoke test: boot, show dir, exit
    c = CPM()
    print(c.expect_prompt().decode("latin1"))
    print(c.cmd("DIR").decode("latin1"))
    c.close()
