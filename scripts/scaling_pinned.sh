#!/bin/sh
# usage: scripts/scaling_pinned.sh <filelist> <runs> <out> <impls> <spec>...
# spec = workers@cpu,cpu,... (workers pinned round-robin to those CPUs). For each spec and impl
# (asm and/or c, comma-separated) runs test/mtspeed memory capped and samples the clock of just
# those CPUs every 0.2 s while it runs; appends the result and the CPUs' mean MHz to <out>.
list=$1 runs=$2 out=$3 impls=$4; shift 4
for spec in "$@"; do
  w=${spec%@*} cpus=${spec#*@}
  for impl in $(echo "$impls" | tr , ' '); do
    ( while :; do for c in $(echo "$cpus" | tr , ' '); do cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq; done; sleep 0.2; done ) > "$out.freq" & sp=$!
    systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 timeout 1800 \
      test/mtspeed go "$list" $impl $w $runs $cpus > "$out.run"
    kill $sp; wait $sp 2>/dev/null
    r=$(grep RESULT "$out.run")
    mhz=$(awk '{ s += $1; n++ } END { printf "%.0f", s / n / 1000 }' "$out.freq")
    echo "$r spec=$spec meanMHz=$mhz load1=$(cut -d' ' -f1 /proc/loadavg)" | tee -a "$out"
  done
done
rm -f "$out.freq" "$out.run"
