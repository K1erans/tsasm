#!/bin/sh
# Parity of every grammar against C tree-sitter, compared with the recorded results/parity_<lang>.txt.
# Fails if any summary line or any mismatch detail differs. Memory capped at 4 GB per grammar.
# usage: scripts/parity.sh [threads]
threads=${1:-12}
make -s tools || exit 1
status=0
mkdir -p build/parity
for lang in go c java json; do
  case $lang in
    go) files=test/files.txt ;;
    *) files=corpus/$lang.txt ;;
  esac
  out=build/parity/$lang.txt
  systemd-run --user --scope -q -p MemoryMax=4G -p MemorySwapMax=0 \
    timeout 1200 test/check_all $lang $files "$threads" > "$out" 2>/dev/null
  if cmp -s "$out" results/parity_$lang.txt; then
    echo "parity $lang: same as recorded ($(grep '^lang=' "$out"))"
  else
    echo "parity $lang: DIFFERS from results/parity_$lang.txt"
    diff results/parity_$lang.txt "$out" | head -20
    status=1
  fi
done
exit $status
