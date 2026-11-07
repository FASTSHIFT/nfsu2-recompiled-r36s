#!/usr/bin/env bash
# ab_pc.sh ROUNDS "NAME_A|cfg line" "NAME_B|cfg line" ...   : alternate the configs ROUNDS times, one short race
# baseline each (pc_run.py --drive), print every RESULT line and a per-config mean.
# A cfg line is appended to nfsu2.cfg for that run only, e.g.  "slots3|export NFSU2_PRESENT_SLOTS=3"
# Refuses to start while a game is running on the device.
R=/home/vifex/workpath/r36s/tmp
ROUNDS=$1; shift
CONFIGS=("$@")
OUT=$R/ab_pc.$(date +%H%M%S).txt
: > "$OUT"
if bash $R/rsh.sh 'pgrep -x nfsu2.aarch64.v >/dev/null'; then echo "game is running on the device; not starting" >&2; exit 3; fi
sshpass -p ark scp -o StrictHostKeyChecking=no $R/pc_run.py ark@192.168.10.241:/tmp/pc_run.py || exit 1
for ((r = 1; r <= ROUNDS; r++)); do
  for c in "${CONFIGS[@]}"; do
    name=${c%%|*}; line=${c#*|}
    sleep 25      # let EmulationStation settle after the previous game
    bash $R/rsh.sh "nohup python3 /tmp/pc_run.py --secs 240 --drive --nwin 3 --cfg '$line' >/dev/null 2>/tmp/pcrun.err & echo started" >/dev/null
    for i in $(seq 1 90); do
      sleep 3
      bash $R/rsh.sh 'grep -q "cfg restored" /tmp/pcrun.out' && break
    done
    res=$(bash $R/rsh.sh 'grep -a "RESULT\|LOADING HANG\|ABORT\|never appeared" /tmp/pcrun.out | tail -1; grep -a "NFSU2 PRESENT\] [0-9]* frames" /roms/ports/nfsu2/log.txt | tail -1 | cut -c1-200')
    echo "round $r $name: $res" | tee -a "$OUT"
  done
done
python3 - "$OUT" <<'E'
import re, sys, collections
d = collections.defaultdict(list)
for l in open(sys.argv[1]):
    m = re.match(r"round \d+ (\S+): .*RESULT fps ([\d.]+)\s+wall ([\d.]+) ms\s+game-thread ([\d.]+) ms\s+GL-driver ([\d.]+) ms\s+draws (\d+)", l)
    if m: d[m.group(1)].append(tuple(float(x) for x in m.groups()[1:]))
print("\nconfig   n   fps   wall   game   GLdrv  draws")
for k, v in d.items():
    n = len(v); avg = [sum(x[i] for x in v) / n for i in range(5)]
    print("%-8s %d  %5.1f  %5.1f  %5.1f  %5.1f  %5.0f   (fps per run: %s)" % (k, n, *avg, " ".join("%.1f" % x[0] for x in v)))
E
