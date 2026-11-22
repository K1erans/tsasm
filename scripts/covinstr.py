#!/usr/bin/env python3
"""Fuzz-build transform for the asm runtime: in.s -> out.s.

- After every label in an executable section, bump an 8-bit counter in libFuzzer's
  __libfuzzer_extra_counters section, so libFuzzer sees coverage of the asm itself.
  pushfq/popfq keep the flags intact; no register changes.
- Route realloc/free/aligned_alloc to tsasm_fuzz_* so the harness can place every buffer
  against a PROT_NONE guard page and inject allocation failures.
usage: covinstr.py <prefix> <in.s> <out.s>
"""
import re, sys

prefix, src, dst = sys.argv[1:4]
label = re.compile(r'^(\s*)([A-Za-z_.$][\w.$]*|\d+):(.*)$')
section_text = True
n = 0
out = []
for line in open(src):
    line = line.rstrip('\n')
    s = line.strip()
    if s.startswith('.text'):
        section_text = True
    elif s.startswith('.section'):
        section_text = s.split()[1].rstrip(',').startswith('.text')
    elif s in ('.data', '.bss'):
        section_text = False
    line = re.sub(r'\b(realloc|free|aligned_alloc)@PLT', r'tsasm_fuzz_\1@PLT', line)
    m = label.match(line)
    if section_text and m:
        out.append('%s%s:' % (m.group(1), m.group(2)))
        out.append('\tpushfq\n\tinc byte ptr [rip + tsasm_cov_%s + %d]\n\tpopfq' % (prefix, n))
        n += 1
        if m.group(3).strip():
            out.append('\t' + m.group(3).strip())
        continue
    out.append(line)
out.append('\t.section __libfuzzer_extra_counters,"aw",@progbits')
out.append('tsasm_cov_%s:\t.zero %d' % (prefix, max(n, 1)))
out.append('\t.section .note.GNU-stack,"",@progbits')
open(dst, 'w').write('\n'.join(out) + '\n')
print('%s: %d counters' % (src, n), file=sys.stderr)
