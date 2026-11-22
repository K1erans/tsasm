#!/bin/sh
# Replays each saved fuzz finding in fuzz/regress/<lang>/ through build/fuzz/<lang>, once per
# guard mode. libFuzzer runs file arguments once each and exits non-zero on the first failure.
for dir in fuzz/regress/*/; do
  lang=$(basename "$dir")
  for mode in over under; do
    log=build/fuzz/regress_${lang}_$mode.log
    if ! TSASM_GUARD=$mode "build/fuzz/$lang" "$dir"* > "$log" 2>&1; then
      tail -20 "$log"
      exit 1
    fi
    echo "fuzz-regress $lang $mode: $(ls "$dir" | wc -l) inputs ok"
  done
done
