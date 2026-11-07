#!/usr/bin/env python3
"""On-device CPU probe for the PC port: samples the game process from outside, independent of the game's own
counters. Waits for the process, then every INTERVAL seconds writes one JSON line to /tmp/cpuprobe.jsonl:
  per thread  : user+system jiffies (10 ms ticks, task stat), major/minor faults, state, context switches; run time and
                run-queue wait from schedstat when the kernel has it (the R36S kernel 4.4 does not: -1)
  system      : per-core jiffies incl. iowait/irq/softirq (/proc/stat), swap and fault counters (/proc/vmstat),
                MemAvailable/SwapFree, per-core clock and the clock limit, SoC temperature
usage: python3 cpu_probe.py [interval_seconds=1.0] [output=/tmp/cpuprobe.jsonl]   (run it before starting the game; it exits with the game)"""
import json, os, subprocess, sys, time

COMM = "nfsu2.aarch64.v"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/cpuprobe.jsonl"
IV = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0


def rd(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return ""


def pid():
    o = subprocess.run("pgrep -x " + COMM, shell=True, capture_output=True, text=True).stdout.split()
    return o[0] if o else None


def threads(p):
    r = {}
    try:
        tids = os.listdir("/proc/%s/task" % p)
    except OSError:
        return r
    for t in tids:
        base = "/proc/%s/task/%s/" % (p, t)
        st = rd(base + "stat")
        if not st:
            continue
        nm = st[st.index("(") + 1:st.rindex(")")]
        f = st[st.rindex(")") + 2:].split()
        ss = rd(base + "schedstat").split()
        vol = inv = 0
        for l in rd(base + "status").splitlines():
            if l.startswith("voluntary_ctxt_switches"):
                vol = int(l.split()[1])
            elif l.startswith("nonvoluntary_ctxt_switches"):
                inv = int(l.split()[1])
        r[t] = {"n": nm, "state": f[0], "minflt": int(f[7]), "majflt": int(f[9]), "utime": int(f[11]), "stime": int(f[12]),
                "run_ns": int(ss[0]) if len(ss) >= 2 else -1, "wait_ns": int(ss[1]) if len(ss) >= 2 else -1,
                "vol": vol, "invol": inv}
    return r


def system():
    cores = {}
    for l in rd("/proc/stat").splitlines():
        if l.startswith("cpu") and l[3].isdigit():
            a = l.split()
            cores[a[0]] = [int(x) for x in a[1:9]]      # user nice system idle iowait irq softirq steal
    vm = {}
    for l in rd("/proc/vmstat").splitlines():
        k, v = l.split()
        if k in ("pswpin", "pswpout", "pgmajfault", "pgfault", "pgpgin", "pgpgout", "pgscan_kswapd_normal",
                 "pgscan_direct_normal", "pgsteal_kswapd_normal", "pgsteal_direct_normal"):
            vm[k] = int(v)
    mi = {}
    for l in rd("/proc/meminfo").splitlines():
        k, v = l.split(":")[0], l.split()[1]
        if k in ("MemAvailable", "SwapFree", "SwapTotal", "Dirty", "Writeback"):
            mi[k] = int(v)
    fq = {}
    for c in range(4):
        d = "/sys/devices/system/cpu/cpu%d/cpufreq/" % c
        fq["cpu%d" % c] = [int(rd(d + "scaling_cur_freq") or 0), int(rd(d + "scaling_max_freq") or 0)]
    temp = int(rd("/sys/class/thermal/thermal_zone0/temp") or 0)
    return {"cores": cores, "vm": vm, "mem": mi, "freq": fq, "temp": temp}


open(OUT, "w").close()
p = None
while not p:
    p = pid()
    time.sleep(0.2)
with open(OUT, "a") as out:
    while os.path.exists("/proc/%s" % p):
        t = time.time()
        out.write(json.dumps({"t": t, "threads": threads(p), "sys": system()}) + "\n")
        out.flush()
        time.sleep(max(0.0, IV - (time.time() - t)))
