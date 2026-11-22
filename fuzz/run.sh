#!/bin/sh
# usage: fuzz/run.sh <lang> <over|under> <seconds> [fuzz dir]
# Runs build/fuzz/<lang> in fork mode (crashes are saved and fuzzing continues), memory capped,
# at nice 19. Corpus, crashes and the log go under the fuzz dir (default ~/Storage/fuzz).
lang=$1 mode=$2 secs=$3 dir=${4:-$HOME/Storage/fuzz}
bin=$(cd "$(dirname "$0")/.." && pwd)/build/fuzz/$lang
mkdir -p "$dir/corpus_$lang" "$dir/seed_$lang" "$dir/crash_${lang}_$mode" "$dir/logs"
cd "$dir" || exit 1
TSASM_GUARD=$mode exec systemd-run --user --scope -q -p MemoryMax=3G -p MemorySwapMax=0 \
  nice -n 19 timeout $((secs + 600)) "$bin" -fork=1 -ignore_crashes=1 -ignore_ooms=1 -ignore_timeouts=1 \
  -max_total_time="$secs" -timeout=20 -rss_limit_mb=2500 -max_len=8192 \
  -artifact_prefix="crash_${lang}_$mode/" "corpus_$lang" "seed_$lang" > "logs/${lang}_$mode.log" 2>&1
