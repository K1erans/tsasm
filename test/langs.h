// Test-side table of the grammars built into libtsasm.a and their C tree-sitter references.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tsasm.h"
typedef struct TSLanguage TSLanguage;
const TSLanguage *tree_sitter_go(void), *tree_sitter_c(void), *tree_sitter_java(void), *tree_sitter_json(void);
typedef struct { const char *name; const TsasmLanguage *(*tsasm)(void); const TSLanguage *(*ts)(void); } TestLang;
static const TestLang test_langs[] = {
  {"go", tsasm_language_go, tree_sitter_go},
  {"c", tsasm_language_c, tree_sitter_c},
  {"java", tsasm_language_java, tree_sitter_java},
  {"json", tsasm_language_json, tree_sitter_json},
};
static inline const TestLang *test_lang(const char *name) {
  for (size_t i = 0; i < sizeof test_langs / sizeof *test_langs; i++)
    if (!strcmp(test_langs[i].name, name)) return &test_langs[i];
  fprintf(stderr, "unknown language %s (go, c, java, json)\n", name);
  exit(2);
}
// C tree-sitter reference (test/ref.c), emitting the TsasmNode format. Not thread-safe.
int64_t ref_parse(const TSLanguage *lang, const uint8_t *src, uint64_t len, TsasmNode *out, uint64_t cap);
int64_t ref_parse_only(const TSLanguage *lang, const uint8_t *src, uint64_t len);
