#!/bin/bash
# Checks the tsasm parser-memory model (and optionally the glr-stack model) against src/parser.s and tsasm.h.
#   check.sh            ParserMemory.tla: anchors, then every property; exit 0 when results match expected.tsv
#   check.sh --glr      GlrStack.tla: delegates to verification/glr-stack/check.sh
#   check.sh --all      both models (ParserMemory.tla first, then GlrStack.tla)
#   check.sh --mutants  also prove each property fails on a broken variant (combines with the above)
#   check.sh --confirm  record the covered files as confirmed in sync (run only after reading them)
# Exit: 0 matches expected.tsv, 1 a property differs, 2 out of sync, 3 setup problem.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
JAR="$HERE/.tools/tla2tools.jar"
JAR_URL="https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar"
JAR_SHA="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
COVERED=(src/parser.s tsasm.h)

ANCHORS="$HERE/anchors.tsv"
EXPECTED="$HERE/expected.tsv"
SYNCED="$HERE/synced.sha256"

DO_MEM=0 DO_GLR=0 MUTANTS=0 CONFIRM=0
for arg in "$@"; do
  case "$arg" in
    --mem) DO_MEM=1;;
    --glr) DO_GLR=1;;
    --all) DO_MEM=1; DO_GLR=1;;
    --mutants) MUTANTS=1;;
    --confirm) CONFIRM=1;;
    *) echo "unknown option $arg (use --mem, --glr, --all, --mutants, --confirm)"; exit 3;;
  esac
done
[ $DO_MEM = 0 ] && [ $DO_GLR = 0 ] && DO_MEM=1

sha256_files() {
  if command -v sha256sum >/dev/null 2>&1; then
    (cd "$ROOT" && sha256sum "$@")
  else
    (cd "$ROOT" && shasum -a 256 "$@")
  fi
}

if [ $CONFIRM = 1 ]; then
  if [ $DO_MEM = 1 ]; then
    sha256_files "${COVERED[@]}" > "$SYNCED"
    echo "Recorded ${#COVERED[@]} covered files as in sync in $(basename "$SYNCED")."
  fi
  if [ $DO_GLR = 1 ]; then
    "$HERE/../glr-stack/check.sh" --confirm || exit $?
  fi
  exit 0
fi

command -v java >/dev/null || { echo "java is needed (apt install openjdk-21-jre or brew install openjdk)"; exit 3; }
if [ ! -f "$JAR" ]; then
  mkdir -p "$HERE/.tools"
  curl -sL -o "$JAR" "$JAR_URL" || { echo "could not download TLC from $JAR_URL"; exit 3; }
fi
GOT_SHA="$(sha256_files "$JAR" 2>/dev/null | awk '{print $1}')"
if [ -z "$GOT_SHA" ]; then
  if command -v sha256sum >/dev/null 2>&1; then GOT_SHA="$(sha256sum "$JAR" | awk '{print $1}')"; else GOT_SHA="$(shasum -a 256 "$JAR" | awk '{print $1}')"; fi
fi
[ "$GOT_SHA" = "$JAR_SHA" ] || { echo "tla2tools.jar has the wrong checksum ($GOT_SHA); delete $JAR"; exit 3; }

run_mem() {
  local status=0
  [ $DO_GLR = 1 ] && echo "== ParserMemory.tla (buffer lifecycle, MAP_MIN migration, limits, and C_RETAIN retention) =="

  # 1. Anchors: each line of code the model mirrors must still be there, the expected number of times.
  local drift=0 file count text mirrors found
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
    echo "The code the model describes has changed. Update ParserMemory.tla and anchors.tsv to match it, then rerun."
    return 2
  fi
  if [ -f "$SYNCED" ] && ! diff -q <(sha256_files "${COVERED[@]}") "$SYNCED" >/dev/null; then
    echo "NOTE  covered files changed since the model was last confirmed (anchors still match):"
    diff <(sha256_files "${COVERED[@]}") "$SYNCED" | grep '^<' | awk '{print "      " $3}'
    echo "      Read the changes for new buffer growth, abort, or release paths, then run check.sh --confirm."
  fi

  local WORK
  mkdir -p "$HERE/.tools"
  WORK="$(mktemp -d "$HERE/.tools/work.XXXXXX")"
  trap 'rm -rf "$WORK"' RETURN
  cp "$HERE/ParserMemory.tla" "$WORK/"

  run_tlc() {
    local label="$1" inv="$2"; shift 2
    local charges=TRUE commits=TRUE migrates=TRUE navail=TRUE failrel=TRUE clearres=TRUE lowerrel=TRUE dropbig=TRUE
    for kv in "$@"; do
      case "$kv" in
        ChargesBeforeAlloc=*)          charges="${kv#*=}";;
        CommitsAfterAlloc=*)           commits="${kv#*=}";;
        MigratesAtMapMin=*)            migrates="${kv#*=}";;
        CapsNavailByNodeLim=*)         navail="${kv#*=}";;
        ReleasesAllOnFailedBigParse=*) failrel="${kv#*=}";;
        ClearsResultOnError=*)         clearres="${kv#*=}";;
        ReleasesOnLoweredCap=*)        lowerrel="${kv#*=}";;
        DropsBigOutAfterWorkRelease=*) dropbig="${kv#*=}";;
      esac
    done
    cat > "$WORK/$label.cfg" <<CFG
SPECIFICATION Spec
CONSTANTS
  ChargesBeforeAlloc = $charges
  CommitsAfterAlloc = $commits
  MigratesAtMapMin = $migrates
  CapsNavailByNodeLim = $navail
  ReleasesAllOnFailedBigParse = $failrel
  ClearsResultOnError = $clearres
  ReleasesOnLoweredCap = $lowerrel
  DropsBigOutAfterWorkRelease = $dropbig
INVARIANT $inv
CFG
    (cd "$WORK" && java -XX:+UseParallelGC -Djava.io.tmpdir="$WORK" -cp "$JAR" tlc2.TLC -config "$label.cfg" -metadir "$WORK/states-$label" -workers 1 ParserMemory.tla > "$label.out" 2>&1)
    if grep -q "No error has been found" "$WORK/$label.out"; then echo PASS
    elif grep -q "Invariant $inv is violated" "$WORK/$label.out"; then echo FAIL
    else echo ERROR; fi
  }

  trace() {
    awk '/^State [0-9]+:/{getline; s=""} /^\/\\ act = /{a=$4} /^\/\\ phase = /{p=$4} /^\/\\ workCap = /{w=$4} /^\/\\ outCap = /{o=$4} /^\/\\ memUsed = /{m=$4} /^\/\\ retain = /{r=$4} /^\/\\ workReleased = /{wr=$4} /^$/{if(a!=""){printf "        %-18s phase=%-7s work=%s out=%s memUsed=%s/%s workReleased=%s\n",a,p,w,o,m,r,wr; a=""}}' "$1" | tr -d '"'
  }

  # 2. Properties on the current code.
  local inv expected note got
  echo "Properties on the code as it is (MapMin = 2, Retain = 2, Limit = 4):"
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

    # 3. Mutants.
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
    expect FAIL mutant-commit-before-alloc    AccountingExact          CommitsAfterAlloc=FALSE
    expect FAIL mutant-no-limit-charge        MemoryWithinLimit        ChargesBeforeAlloc=FALSE
    expect FAIL mutant-no-release-on-lower    MemoryWithinLimit        ReleasesOnLoweredCap=FALSE
    expect FAIL mutant-no-mapmin-migration    AllocatorKindConsistent  MigratesAtMapMin=FALSE
    expect FAIL mutant-navail-ignores-nodelim NodeBudgetEnforced       CapsNavailByNodeLim=FALSE
    expect FAIL mutant-stale-result-on-err    ResultPointerValid       ClearsResultOnError=FALSE
    expect FAIL mutant-work-only-on-fail      RetainCapRespectedOnIdle ReleasesAllOnFailedBigParse=FALSE
    expect FAIL mutant-keep-big-out-on-entry  BigParseOutputDropped    DropsBigOutAfterWorkRelease=FALSE
  fi

  [ $status = 0 ] && echo "Result: matches $(basename "$EXPECTED")." || echo "Result: DIFFERS from $(basename "$EXPECTED")."
  return $status
}

overall=0
if [ $DO_MEM = 1 ]; then
  run_mem; s=$?
  [ $s -gt $overall ] && overall=$s
fi
if [ $DO_GLR = 1 ]; then
  glr_args=()
  [ $MUTANTS = 1 ] && glr_args+=(--mutants)
  [ $DO_MEM = 1 ] && echo ""
  "$HERE/../glr-stack/check.sh" "${glr_args[@]}"; s=$?
  [ $s -gt $overall ] && overall=$s
fi
exit $overall
