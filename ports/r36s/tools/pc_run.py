#!/usr/bin/env python3
"""On-device test driver for the PC port (nfsu2.aarch64.v12).
Presses A in EmulationStation (cursor must be on NFSU2), samples the game, then quits it with Select+Start.
Refuses to start while a game is already running. Restores nfsu2.cfg when done.
usage: python3 pc_run.py --secs N [--cfg 'LINE' ...] [--shots N] [--until-log TEXT] [--noquit]
Output: /tmp/pcrun.out (events), /tmp/pcrun.tsv (samples)"""
import argparse, os, re, subprocess, sys, time
import evdev

PORT = "/roms/ports/nfsu2"
CFG, CFG_BAK = PORT + "/nfsu2.cfg", "/tmp/nfsu2.cfg.pcrun-bak"
LOG = PORT + "/log.txt"
ES_LOG = "/home/ark/.emulationstation/es_log.txt"
OUT, TSV = "/tmp/pcrun.out", "/tmp/pcrun.tsv"
EVDEV = "/dev/input/event2"
BTN_A, BTN_B, BTN_SELECT, BTN_START = 305, 304, 704, 705
# evdev codes of the R36S pad; SDL/vkbd names: 'WEST' (printed Y, left) = 307 = backspace in the on-screen keyboard,
# 'NORTH' (printed X, top) = 308 = shift there.
PAD = {'A': 305, 'B': 304, 'WEST': 307, 'NORTH': 308, 'L1': 310, 'R1': 311, 'L2': 312, 'R2': 313, 'UP': 544, 'DOWN': 545, 'LEFT': 546, 'RIGHT': 547, 'SELECT': 704, 'START': 705, 'R3': 707, 'L3': 706}
COMM = "nfsu2.aarch64.v"          # comm is cut to 15 characters

def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip()

def say(m):
    with open(OUT, "a") as f:
        f.write("%s %s\n" % (time.strftime("%H:%M:%S"), m))

def pid():
    p = sh("pgrep -x " + COMM)
    return p.split()[0] if p else None

def last_frame():
    m = sh("grep -a 'Guest D3D9 frame=' %s | tail -1 | sed 's/.*frame=\\([0-9]*\\).*/\\1/'" % LOG)
    return m

def shot_frame():
    try:
        with open(LOG, 'rb') as f:
            f.seek(0, 2); n = f.tell(); f.seek(max(0, n - 6000)); t = f.read().decode('latin1')
        m = re.findall(r'diag_frame_(\d+)\.ppm', t)
        return int(m[-1]) if m else 0
    except OSError:
        return 0

def tap(codes, hold=0.2):
    d = evdev.InputDevice(EVDEV)
    for c in codes: d.write(evdev.ecodes.EV_KEY, c, 1)
    d.write(evdev.ecodes.EV_SYN, 0, 0); time.sleep(hold)
    for c in codes: d.write(evdev.ecodes.EV_KEY, c, 0)
    d.write(evdev.ecodes.EV_SYN, 0, 0)

def cpu_ticks(p):
    r = {}
    try:
        for t in os.listdir("/proc/%s/task" % p):
            try:
                st = open("/proc/%s/task/%s/stat" % (p, t)).read()
                nm = st[st.index("(") + 1:st.rindex(")")]
                f = st[st.rindex(")") + 2:].split()
                r[t] = (nm, int(f[11]) + int(f[12]))
            except (OSError, ValueError):
                pass
    except OSError:
        pass
    return r

ap = argparse.ArgumentParser()
ap.add_argument("--secs", type=int, default=120)
ap.add_argument("--cfg", action="append", default=[])
ap.add_argument("--until-log", default="")
ap.add_argument("--noquit", action="store_true")
ap.add_argument("--load-timeout", dest="load_timeout", type=int, default=120, help="with --drive: seconds of race loading before it counts as hung")
ap.add_argument("--nwin", type=int, default=2, help="with --drive: PERF windows (120 frames each) to average once loading has settled")
ap.add_argument("--settle", type=int, default=10, help="with --drive: seconds after the track stream opens before measuring (countdown, start)")
ap.add_argument("--window", type=int, default=12, help="with --drive: seconds to measure after the first busy race frame")
ap.add_argument("--tap-every", dest="tap_every", type=float, default=0, help="tap B every N seconds (menu exploration, no race logic)")
ap.add_argument("--seq", default="", help="timed pad taps, seconds since the game started: '14:B,16:RIGHT,18:RIGHT' (A B WEST NORTH UP DOWN LEFT RIGHT START SELECT L1 R1 L3 R3)")
ap.add_argument("--drive", action="store_true", help="tap B (confirm) until the race loads, then hold B (accelerate)")
a = ap.parse_args()

open(OUT, "w").close(); open(TSV, "w").close()
if pid():
    say("ABORT: the game is already running (pid %s); not touching the device" % pid()); sys.exit(2)
sh("cp %s %s" % (CFG, CFG_BAK))
try:
    if a.cfg:
        with open(CFG, "a") as f:
            f.write("\n" + "\n".join(a.cfg) + "\n")
        say("cfg += " + " | ".join(a.cfg))
    n0 = sh("grep -c 'Attempting to launch game' " + ES_LOG)
    p = None
    for attempt in range(1, 3):       # the first press can be eaten by ES (just re-initialised after a game): press again
        tap([BTN_A]); say("pressed A (try %d)" % attempt)
        for _ in range(100):             # a launch can take over a minute (launcher sync + drop caches)
            p = pid()
            if p: break
            time.sleep(1)
        if p: break
    if not p:
        say("game never appeared; ES log tail: " + sh("tail -3 " + ES_LOG)); sys.exit(1)
    last = sh("grep -A1 'Attempting to launch game' %s | tail -2 | tr '\\n' ' '" % ES_LOG)
    say("game pid %s; ES: %s" % (p, last[:150]))
    t0 = time.time(); prev = cpu_ticks(p); prevt = t0; hz = os.sysconf("SC_CLK_TCK")
    seq = []          # [(anchor_frame or None, offset_seconds, key)]
    for x in a.seq.split(','):
        if not x.strip(): continue
        when, key = x.split(':')
        mm = re.match(r"F(\d+)\+([\d.]+)$", when)
        seq.append((int(mm.group(1)), float(mm.group(2)), key.upper()) if mm else (None, float(when), key.upper()))
    anchors = {}
    race_at = None; ingame_at = None; held = False; tick = 0; meas_at = None
    TICK = 0.25
    while time.time() - t0 < a.secs:
        time.sleep(TICK); tick += 1
        if seq:
            fr = shot_frame() if any(e[0] for e in seq) else 0
            for f in {e[0] for e in seq if e[0]}:
                if f not in anchors and fr >= f: anchors[f] = time.time(); say("anchor: present frame >= %d" % f)
            due = [e for e in seq if (e[0] is None and time.time() - t0 >= e[1]) or (e[0] is not None and e[0] in anchors and time.time() - anchors[e[0]] >= e[1])]
            for e in due:
                seq.remove(e); tap([PAD[e[2]]], 0.15); say("seq: %s (F%s+%.1f) at t=%.0fs" % (e[2], e[0], e[1], time.time() - t0))
        if a.tap_every and not a.drive and tick % max(1, int(a.tap_every / TICK)) == 0 and tick > 4 / TICK: tap([BTN_B], 0.12)
        if a.drive and os.path.exists("/proc/%s" % p):
            lg = open(LOG, errors="replace").read() if tick % 4 == 0 else ""
            # Loading time varies a lot (cold SD card: minutes). The race assets are loaded last: the track stream
            # archive STREAML4RA.BUN is opened for the first time after InGameRace.bun and everything else.
            if ingame_at is None and "InGameRace.bun" in lg:
                ingame_at = time.time(); say("race loading started (InGameRace.bun) at t=%.0fs; no more taps" % (ingame_at - t0))
            if race_at is None and ingame_at is not None and lg and "STREAML4RA.BUN" in lg[lg.index("InGameRace.bun"):]:
                race_at = time.time(); say("track stream opened (loading done, %.0fs) at t=%.0fs" % (race_at - ingame_at, race_at - t0))
            if race_at is None and ingame_at is not None and time.time() - ingame_at > a.load_timeout:
                say("LOADING HANG: no track stream %ds after InGameRace.bun; dumping backtraces" % a.load_timeout)
                open("/tmp/pcrun.hang.txt", "w").write(sh("echo ark | sudo -S gdb -p %s -batch -ex 'set pagination off' -ex 'thread apply all bt 14' 2>&1 | grep -v '^\\[New\\|^warning\\|Reading symbols' | cut -c1-200" % p))
                break
            if race_at is None:
                if ingame_at is None and tick % 2 == 0: tap([BTN_B], 0.1)   # a confirm every 0.5 s while in the menus
            else:
                if not held:
                    d = evdev.InputDevice(EVDEV); d.write(evdev.ecodes.EV_KEY, BTN_B, 1); d.write(evdev.ecodes.EV_SYN, 0, 0)
                    held = True; say("holding B (accelerate) at t=%.0fs" % (time.time() - t0))
                if lg and meas_at is None:
                    # loading is over when a PERF window (every 120 frames) runs at >= 22 fps again; the next
                    # a.nwin windows (~4 s each) are the measurement.
                    tail = lg[lg.index("STREAML4RA.BUN"):]
                    perf = [(int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5)), float(m.group(6)))
                            for m in re.finditer(r"PERF\] frame=(\d+) game-thread CPU ([\d.]+) ms/frame, wall ([\d.]+) ms/frame \(([\d.]+) fps\), (\d+) draws/frame, ([\d.]+) ms", tail)]
                    steady = next((i for i, x in enumerate(perf) if x[2] <= 42.0 and i >= 1), None)
                    if steady is not None and len(perf) >= steady + 1 + a.nwin:
                        meas_at = time.time(); sel = perf[steady + 1:steady + 1 + a.nwin]
                        say("load settled at PERF frame %d; measured frames %d..%d" % (perf[steady][0], sel[0][0] - 119, sel[-1][0]))
                        say("RESULT fps %.1f  wall %.1f ms  game-thread %.1f ms  GL-driver %.1f ms  draws %d  (windows: %s)" % (
                            sum(x[3] for x in sel) / len(sel), sum(x[2] for x in sel) / len(sel), sum(x[1] for x in sel) / len(sel),
                            sum(x[5] for x in sel) / len(sel), sum(x[4] for x in sel) / len(sel),
                            " ".join("f%d:%.1ffps" % (x[0], x[3]) for x in sel)))
                        thr = [l for l in tail.splitlines() if "PERF] process CPU" in l]
                        if thr: say("threads: " + thr[-1][thr[-1].index("process"):][:230])
                        break
        if tick % 8: continue          # sample every 2 s
        if not os.path.exists("/proc/%s" % p):
            say("game exited by itself after %.0fs" % (time.time() - t0)); break
        now = time.time(); cur = cpu_ticks(p)
        per = {}
        for t, (nm, tk) in cur.items():
            if t in prev: per[nm] = per.get(nm, 0) + (tk - prev[t][1])
        top = sorted(per.items(), key=lambda x: -x[1])[:4]
        dt = now - prevt; prev, prevt = cur, now
        st = open("/proc/%s/status" % p).read()
        g = lambda k: [l.split()[1] for l in st.splitlines() if l.startswith(k)][0]
        temp = sh("cat /sys/class/thermal/thermal_zone0/temp")
        fq = sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")
        fr = sh("grep 'Guest D3D9 frame=' %s | tail -1 | cut -d' ' -f4-6" % LOG)
        with open(TSV, "a") as f:
            f.write("%d\trss=%sk\tswap=%sk\ttemp=%s\tfreq=%s\t%s\tcpu: %s\n" % (
                now - t0, g("VmRSS"), g("VmSwap"), temp, fq, fr,
                " ".join("%s=%.0f%%" % (n, 100.0 * tk / hz / dt) for n, tk in top)))
        if a.until_log and a.until_log in open(LOG, errors="replace").read():
            say("log contains '%s' after %.0fs" % (a.until_log, now - t0)); break
    if held:
        d = evdev.InputDevice(EVDEV); d.write(evdev.ecodes.EV_KEY, BTN_B, 0); d.write(evdev.ecodes.EV_SYN, 0, 0)
    if os.path.exists("/proc/%s" % p) and not a.noquit:
        tap([BTN_SELECT, BTN_START], 0.5); say("sent Select+Start")
        for _ in range(20):
            if not os.path.exists("/proc/%s" % p): break
            time.sleep(1)
        if os.path.exists("/proc/%s" % p):
            say("did not quit; SIGTERM"); sh("kill -TERM %s" % p); time.sleep(5)
    say("ended; game " + ("still running" if os.path.exists("/proc/%s" % p) else "gone"))
finally:
    sh("cp %s %s" % (CFG_BAK, CFG)); say("cfg restored")
    say("log tail:\n" + sh("tail -6 %s | cut -c1-200" % LOG))
