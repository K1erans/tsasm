#!/bin/bash
# Checks the tsasm GLR stack & mark-and-sweep GC model (GlrStack.tla) against src/parser.s.
#   check.sh            anchors, then every property; exit 0 when results match expected.tsv
#   check.sh --mutants  also prove each property fails on a broken variant
#   check.sh --confirm  record the covered files as confirmed in sync (run only after reading them)
# Exit: 0 matches expected.tsv, 1 a property differs, 2 out of sync, 3 setup problem.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
JAR="$HERE/../parser-memory/.tools/tla2tools.jar"
JAR_URL="https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar"
JAR_SHA="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
COVERED=(src/parser.s)

ANCHORS="$HERE/anchors.tsv"
EXPECTED="$HERE/expected.tsv"
SYNCED="$HERE/synced.sha256"

MUTANTS=0 CONFIRM=0
for arg in "$@"; do
  case "$arg" in
    --mutants) MUTANTS=1;;
    --confirm) CONFIRM=1;;
    *) echo "unknown option $arg (use --mutants, --confirm)"; exit 3;;
  esac
done

sha256_files() {
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$ROOT" && sha256sum "$@")
  else
    (cd "$ROOT" && shasum -a 256 "$@")
  fi
}

if [ $CONFIRM = 1 ]; then
  sha256_files "${COVERED[@]}" > "$SYNCED"
  echo "Recorded ${#COVERED[@]} covered files as in sync in $(basename "$SYNCED")."
  exit 0
fi

command -v java >/dev/null || { echo "java is needed (apt install openjdk-21-jre or brew install openjdk)"; exit 3; }
if [ ! -f "$JAR" ]; then
  mkdir -p "$(dirname "$JAR")"
  curl -sL -o "$JAR" "$JAR_URL" || { echo "could not download TLC from $JAR_URL"; exit 3; }
fi
if command -v sha256sum >/dev/null 2>&1; then GOT_SHA="$(sha256sum "$JAR" | awk '{print $1}')"; else GOT_SHA="$(shasum -a 256 "$JAR" | awk '{print $1}')"; fi
[ "$GOT_SHA" = "$JAR_SHA" ] || { echo "tla2tools.jar has the wrong checksum ($GOT_SHA); delete $JAR"; exit 3; }

echo "== GlrStack.tla (GLR graph-structured stack, fast-path reductions, condense, and mark-and-sweep GC) =="

# 1. Anchors.
drift=0
while IFS=$'\t' read -r file count text mirrors; do
  case "$file" in ''|'#'*) continue;; esac
  found=$(grep -cF -- "$text" "$ROOT/$file" 2>/dev/null || true)
  if [ "${found:-0}" != "$count" ]; then
    echo "OUT OF SYNC  $file: expected $count, found ${found:-0}: $text"
    echo "             model element to re-read: $mirrors"
    drift=1
  fi
done < "$ANCHORS"
if [ $drift = 1 ]; then
  echo "The code the model describes has changed. Update GlrStack.tla and anchors.tsv to match it, then rerun."
  exit 2
fi
if [ -f "$SYNCED" ] && ! diff -q <(sha256_files "${COVERED[@]}") "$SYNCED" >/dev/null; then
  echo "NOTE  covered files changed since the model was last confirmed (anchors still match):"
  diff <(sha256_files "${COVERED[@]}") "$SYNCED" | grep '^<' | awk '{print "      " $3}'
  echo "      Read the changes for new stack-node allocation, recycling, merge, or GC paths, then run check.sh --confirm."
fi

mkdir -p "$(dirname "$JAR")"
WORK="$(mktemp -d "$(dirname "$JAR")/work.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp "$HERE/GlrStack.tla" "$WORK/"

run_tlc() {
  local label="$1" inv="$2"; shift 2
  local singlever=TRUE singlelink=TRUE gcbase=TRUE gcext=TRUE gcsweepext=TRUE gcclear=TRUE noself=TRUE conddead=TRUE mergelive=TRUE
  for kv in "$@"; do
    case "$kv" in
      InPlaceRequiresSingleVersion=*) singlever="${kv#*=}";;
      InPlaceRequiresSingleLink=*)    singlelink="${kv#*=}";;
      GCMarksBase=*)                  gcbase="${kv#*=}";;
      GCMarksExtLinks=*)              gcext="${kv#*=}";;
      GCSweepsExtBlocks=*)            gcsweepext="${kv#*=}";;
      GCClearsMarks=*)                gcclear="${kv#*=}";;
      AddLinkRejectsSelf=*)           noself="${kv#*=}";;
      CondenseRemovesDead=*)          conddead="${kv#*=}";;
      MergeChecksLive=*)              mergelive="${kv#*=}";;
    esac
  done
  cat > "$WORK/$label.cfg" <<CFG
SPECIFICATION Spec
CONSTANTS
  InPlaceRequiresSingleVersion = $singlever
  InPlaceRequiresSingleLink = $singlelink
  GCMarksBase = $gcbase
  GCMarksExtLinks = $gcext
  GCSweepsExtBlocks = $gcsweepext
  GCClearsMarks = $gcclear
  AddLinkRejectsSelf = $noself
  CondenseRemovesDead = $conddead
  MergeChecksLive = $mergelive
INVARIANT $inv
CFG
  (cd "$WORK" && java -XX:+UseParallelGC -Djava.io.tmpdir="$WORK" -cp "$JAR" tlc2.TLC -config "$label.cfg" -metadir "$WORK/states-$label" -workers 1 GlrStack.tla > "$label.out" 2>&1)
  if grep -q "No error has been found" "$WORK/$label.out"; then echo PASS
  elif grep -q "Invariant $inv is violated" "$WORK/$label.out"; then echo FAIL
  else echo ERROR; fi
}

trace() {
  awk '/^State [0-9]+:/{getline; s=""} /^\/\\ act = /{a=$4} /^\/\\ nh = /{nh=$4} /^\/\\ h1Node = /{h1=$4} /^\/\\ h2Node = /{h2=$4} /^\/\\ sfree = /{sf=$0; sub(/^\/\\ sfree = /,"",sf)} /^$/{if(a!=""){printf "        %-18s nh=%s h1=%s h2=%s sfree=%s\n",a,nh,h1,h2,sf; a=""}}' "$1" | tr -d '"'
}

status=0
echo "Properties on the code as it is (Nodes = 0..3, MaxHeads = 2):"
while IFS=$'\t' read -r inv expected note; do
  case "$inv" in ''|'#'*) continue;; esac
  got=$(run_tlc "code-$inv" "$inv")
  if [ "$got" = "$expected" ]; then
    if [ "$got" = FAIL ]; then
      echo "  KNOWN FINDING  $inv: $note"
      trace "$WORK/code-$inv.out"
    else
      echo "  ok             $inv"
    fi
  else
    echo "  CHANGED        $inv: expected $expected, got $got"
    [ "$got" = FAIL ] && trace "$WORK/code-$inv.out"
    [ "$got" = ERROR ] && tail -15 "$WORK/code-$inv.out"
    status=1
  fi
done < "$EXPECTED"

if [ $MUTANTS = 1 ]; then
  echo "Each property must fail on a broken variant:"
  expect() {
    local want="$1" label="$2" inv="$3"; shift 3
    local got
    got=$(run_tlc "$label" "$inv" "$@")
    if [ "$got" = "$want" ]; then
      echo "  ok             $label -> $got"
    else
      echo "  WRONG          $label: wanted $want, got $got"
      status=1
    fi
  }
  expect FAIL mutant-gc-unmarked-base       BaseNodeProtected    GCMarksBase=FALSE
  expect FAIL mutant-inplace-multi-version  LiveNodesNeverFreed  InPlaceRequiresSingleVersion=FALSE
  expect FAIL mutant-gc-ignores-ext-links   LiveNodesNeverFreed  GCMarksExtLinks=FALSE
  expect FAIL mutant-inplace-multi-link     NoExtensionBlockLeak InPlaceRequiresSingleLink=FALSE
  expect FAIL mutant-gc-leaks-ext-block     NoExtensionBlockLeak GCSweepsExtBlocks=FALSE
  expect FAIL mutant-gc-keeps-marks         MarksClearedAfterGC  GCClearsMarks=FALSE
  expect FAIL mutant-add-link-self-loop     NoSelfLoopLinks      AddLinkRejectsSelf=FALSE
  expect FAIL mutant-condense-keeps-dead    CondenseCleanHeads   CondenseRemovesDead=FALSE
  expect FAIL mutant-merge-dead-head        CondenseCleanHeads   MergeChecksLive=FALSE
fi

[ $status = 0 ] && echo "Result: matches $(basename "$EXPECTED")." || echo "Result: DIFFERS from $(basename "$EXPECTED")."
exit $status
