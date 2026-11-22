#!/bin/sh
# usage: scripts/scaling.sh <filelist> <runs> <out> [worker counts...]
# For each worker count, runs test/mtspeed for asm and for C tree-sitter (asm first, then C),
# memory capped, while sampling every CPU's clock (scaling_cur_freq) every 0.2 s. Appends the
# mtspeed output, the 1-minute load average before the run and the mean MHz of the P-core
# threads (CPUs 0-11) and E-cores (12-19) to <out>. 1 worker is pinned to CPU 4, as in bench/.
list=$1 runs=$2 out=$3; shift 3
counts=${*:-1 6 12 20}
sample() {
  while :; do
    for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
      c=${f#/sys/devices/system/cpu/cpu}; c=${c%%/*}; printf '%s %s\n' "$c" "$(cat "$f")"
    done
    sleep 0.2
  done
}
for w in $counts; do
  for impl in asm c; do
    pin=; [ "$w" = 1 ] && pin=4
    load=$(cut -d' ' -f1 /proc/loadavg)
    sample > "$out.freq" & sp=$!
    systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 timeout 1800 \
      test/mtspeed go "$list" $impl $w $runs $pin > "$out.run"
    kill $sp; wait $sp 2>/dev/null
    awk -v load="$load" '{ s[$1 < 12 ? "P" : "E"] += $2; n[$1 < 12 ? "P" : "E"]++ }
      END { printf "  load1=%s meanMHz P=%.0f E=%.0f\n", load, s["P"]/n["P"]/1000, s["E"]/n["E"]/1000 }' "$out.freq" >> "$out.run"
    cat "$out.run" >> "$out"; grep RESULT "$out.run"
  done
done
rm -f "$out.freq" "$out.run"
