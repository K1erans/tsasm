#!/bin/sh
# Fails if any object in the archive has writable static data: a section that is allocated and
# writable (.data, .bss, ...) and non-empty, other than .data.rel.ro, which the loader makes
# read-only after relocation. This is the structural half of the thread-safety guarantee.
lib=${1:-libtsasm.a}
readelf -SW "$lib" | awk '
  function hex(s,   i, v) { v = 0; for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", tolower(substr(s, i, 1))) - 1; return v }
  /^File: / { obj = $2; n++ }
  /^ *\[ *[0-9]+\]/ {
    sub(/^ *\[ *[0-9]+\] */, "")
    name = $1; size = hex($5); flags = $7
    if (flags ~ /W/ && flags ~ /A/ && name != ".data.rel.ro" && size > 0) {
      printf "%s: writable section %s (%d bytes)\n", obj, name, size; bad = 1
    }
  }
  END {
    if (!bad) printf "check-static: no writable static data in %d objects\n", n
    exit bad
  }'
