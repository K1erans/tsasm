//go:build linux && amd64 && cgo

// Package tsasm parses source code with tsasm, a tree-sitter parser runtime
// in x86-64 assembly, through cgo. It reads the official grammars' parse
// tables, built in for Go, C, Java and JSON, and gives the syntax tree as a
// flat preorder array of the nodes tree-sitter shows: the same tree C
// tree-sitter gives for source without syntax errors.
//
// Building it needs cgo and a C compiler, which assembles the package's
// .s files; nothing else. It builds on linux/amd64 only.
//
// tsasm has no error recovery: source with syntax errors gives ErrSyntax
// and no tree.
package tsasm

/*
#include "tsasm.h"
*/
import "C"

import (
	"errors"
	"slices"
	"sync"
	"unsafe"
)

// Node is one node of a tree: TsasmNode in tsasm.h. Its Field is the one C
// tree-sitter's tree cursor gives it: the field its parent's production
// gives it, else the one its nearest hidden ancestors' give them; an extra
// node, such as a comment, has none.
type Node struct {
	Symbol uint16 // the node's type; Language.SymbolName names it
	Named  uint8  // 1 for a named node, 0 for punctuation and keywords
	Field  uint8  // the node's grammar field, 0 for none; Language.FieldName names it
	Start  uint32 // byte offset of the node's start
	End    uint32 // byte offset just past its end
	Parent int32  // the parent's index in the same array; -1 for the root
}

var _ [unsafe.Sizeof(Node{}) - unsafe.Sizeof(C.TsasmNode{})]byte
var _ [unsafe.Sizeof(C.TsasmNode{}) - unsafe.Sizeof(Node{})]byte

// Why Parse made no tree.
var (
	// ErrSyntax: the source has syntax errors.
	ErrSyntax = errors.New("tsasm: syntax error")
	// ErrLimit: a budget ran out: the memory limit, the work budget (a
	// fixed number of steps per input byte), 2^27 nodes, or an input of
	// 4 GiB or more.
	ErrLimit = errors.New("tsasm: limit reached")
	// ErrNoMem: the allocator failed. The parser stays usable.
	ErrNoMem = errors.New("tsasm: out of memory")
)

// Language is a grammar built into the library. It is read-only and can be
// shared by every goroutine.
type Language struct {
	name   string
	l      *C.TsasmLanguage
	names  []string
	fields []string // by field id, "" for 0
}

var languages = sync.OnceValue(func() map[string]*Language {
	m := map[string]*Language{}
	add := func(name string, l *C.TsasmLanguage) {
		lang := &Language{name: name, l: l, names: make([]string, int(C.tsasm_language_symbol_count(l)))}
		for i := range lang.names {
			lang.names[i] = C.GoString(C.tsasm_language_symbol_name(l, C.uint16_t(i)))
		}
		lang.fields = make([]string, int(C.tsasm_language_field_count(l))+1)
		for i := range lang.fields {
			lang.fields[i] = C.GoString(C.tsasm_language_field_name(l, C.uint8_t(i)))
		}
		m[name] = lang
	}
	add("go", C.tsasm_language_go())     // tree-sitter-go v0.25.0
	add("c", C.tsasm_language_c())       // tree-sitter-c v0.24.2
	add("java", C.tsasm_language_java()) // tree-sitter-java v0.23.5
	add("json", C.tsasm_language_json()) // tree-sitter-json v0.24.8
	return m
})

// Lookup is the built-in grammar called name ("go", "c", "java" or
// "json"), or nil.
func Lookup(name string) *Language { return languages()[name] }

// Languages are the built-in grammars' names, sorted.
func Languages() []string {
	var out []string
	for name := range languages() {
		out = append(out, name)
	}
	slices.Sort(out)
	return out
}

// Name is the grammar's name, as Lookup takes it.
func (l *Language) Name() string { return l.name }

// SymbolCount is the number of symbol ids; every node's Symbol is below it.
func (l *Language) SymbolCount() int { return len(l.names) }

// SymbolName is the node type of symbol s, as C tree-sitter's ts_node_type
// gives it, or "" for an id out of range.
func (l *Language) SymbolName(s uint16) string {
	if int(s) < len(l.names) {
		return l.names[s]
	}
	return ""
}

// FieldCount is the number of grammar fields; field ids run from 1 to it.
func (l *Language) FieldCount() int { return len(l.fields) - 1 }

// FieldName is the name of field f, as C tree-sitter's
// ts_language_field_name_for_id gives it, or "" for 0 or an id out of
// range.
func (l *Language) FieldName(f uint8) string {
	if int(f) < len(l.fields) {
		return l.fields[f]
	}
	return ""
}

// Parser holds all the state of a parse. One parser serves one Parse at a
// time; use one per goroutine. Close frees it.
type Parser struct {
	p *C.TsasmParser
}

// NewParser makes a parser with the library's default memory limit (1 GiB)
// and retention (32 MiB).
func NewParser() (*Parser, error) {
	p := C.tsasm_parser_new()
	if p == nil {
		return nil, ErrNoMem
	}
	return &Parser{p: p}, nil
}

// Close frees the parser and the nodes of its last parse.
func (p *Parser) Close() {
	C.tsasm_parser_free(p.p)
	p.p = nil
}

// SetMemoryLimit caps the bytes the parser may hold, its buffers and its
// last tree included. A parse that would pass it fails with ErrLimit.
func (p *Parser) SetMemoryLimit(bytes uint64) {
	C.tsasm_parser_set_memory_limit(p.p, C.uint64_t(bytes))
}

// SetRetain caps the bytes of buffers the parser keeps between parses.
// Kept buffers make the next big parse faster.
func (p *Parser) SetRetain(bytes uint64) { C.tsasm_parser_set_retain(p.p, C.uint64_t(bytes)) }

// Parse parses src with l and returns the tree's nodes in preorder, the
// root first. They are the parser's: they stay valid until its next Parse
// or Close, so keep the parser alive while you use them.
func (p *Parser) Parse(l *Language, src []byte) ([]Node, error) {
	var ptr *C.uint8_t
	if len(src) > 0 {
		ptr = (*C.uint8_t)(unsafe.Pointer(&src[0]))
	}
	var out *C.TsasmNode
	n := int64(C.tsasm_parse(p.p, l.l, ptr, C.uint64_t(len(src)), &out))
	switch {
	case n > 0:
		return unsafe.Slice((*Node)(unsafe.Pointer(out)), n), nil
	case n == C.TSASM_ERR_SYNTAX:
		return nil, ErrSyntax
	case n == C.TSASM_ERR_LIMIT:
		return nil, ErrLimit
	}
	return nil, ErrNoMem
}
