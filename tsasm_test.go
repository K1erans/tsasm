//go:build linux && amd64 && cgo

package tsasm

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"sync"
	"testing"
)

func parser(t testing.TB) *Parser {
	t.Helper()
	p, err := NewParser()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(p.Close)
	return p
}

// shape writes the tree as field:type[start,end) {children}, quoting
// anonymous nodes.
func shape(l *Language, nodes []Node) string {
	var b strings.Builder
	var write func(i int32) int32
	write = func(i int32) int32 {
		n := nodes[i]
		if n.Field != 0 {
			fmt.Fprintf(&b, "%s:", l.FieldName(n.Field))
		}
		if n.Named != 0 {
			fmt.Fprintf(&b, "%s[%d,%d)", l.SymbolName(n.Symbol), n.Start, n.End)
		} else {
			fmt.Fprintf(&b, "%q[%d,%d)", l.SymbolName(n.Symbol), n.Start, n.End)
		}
		j := i + 1
		if j < int32(len(nodes)) && nodes[j].Parent == i {
			b.WriteString(" {")
			for first := true; j < int32(len(nodes)) && nodes[j].Parent == i; first = false {
				if !first {
					b.WriteString(" ")
				}
				j = write(j)
			}
			b.WriteString("}")
		}
		return j
	}
	write(0)
	return b.String()
}

func TestLanguages(t *testing.T) {
	if got := Languages(); !slices.Equal(got, []string{"c", "go", "java", "json"}) {
		t.Errorf("Languages() = %v", got)
	}
	if Lookup("python") != nil {
		t.Error("Lookup(python) isn't nil")
	}
	fields := map[string]int{"go": 35, "c": 39, "java": 40, "json": 2}
	for _, name := range Languages() {
		l := Lookup(name)
		if l.Name() != name || l.SymbolCount() < 10 || l.SymbolName(uint16(l.SymbolCount())) != "" {
			t.Errorf("%s: name %q, %d symbols, out-of-range name %q", name, l.Name(), l.SymbolCount(), l.SymbolName(uint16(l.SymbolCount())))
		}
		if n := l.FieldCount(); n != fields[name] || l.FieldName(0) != "" || l.FieldName(1) == "" || l.FieldName(uint8(n)) == "" || l.FieldName(uint8(n+1)) != "" {
			t.Errorf("%s: %d fields, names %q %q %q %q", name, n, l.FieldName(0), l.FieldName(1), l.FieldName(uint8(n)), l.FieldName(uint8(n+1)))
		}
	}
}

func TestParseGivesTheTree(t *testing.T) {
	p := parser(t)
	for _, tc := range []struct{ lang, src, want string }{
		{"json", `{"a": [1, 2]}`, `document[0,13) {object[0,13) {"{"[0,1) pair[1,12) {key:string[1,4) {"\""[1,2) string_content[2,3) "\""[3,4)} ":"[4,5) value:array[6,12) {"["[6,7) number[7,8) ","[8,9) number[10,11) "]"[11,12)}} "}"[12,13)}}`},
		{"go", "package p\nfunc f(a int) {}\n", `source_file[0,27) {package_clause[0,9) {"package"[0,7) package_identifier[8,9)} function_declaration[10,26) {"func"[10,14) name:identifier[15,16) parameters:parameter_list[16,23) {"("[16,17) parameter_declaration[17,22) {name:identifier[17,18) type:type_identifier[19,22)} ")"[22,23)} body:block[24,26) {"{"[24,25) "}"[25,26)}}}`},
		{"c", "int x;\n", `translation_unit[0,7) {declaration[0,6) {type:primitive_type[0,3) declarator:identifier[4,5) ";"[5,6)}}`},
		{"java", "class A {}\n", `program[0,11) {class_declaration[0,10) {"class"[0,5) name:identifier[6,7) body:class_body[8,10) {"{"[8,9) "}"[9,10)}}}`},
	} {
		l := Lookup(tc.lang)
		nodes, err := p.Parse(l, []byte(tc.src))
		if err != nil {
			t.Errorf("%s: %v", tc.lang, err)
			continue
		}
		if got := shape(l, nodes); got != tc.want {
			t.Errorf("%s: tree\n%s\nwant\n%s", tc.lang, got, tc.want)
		}
	}
}

func TestParseSaysWhyThereIsNoTree(t *testing.T) {
	p := parser(t)
	if _, err := p.Parse(Lookup("go"), []byte("package p\nfunc f( {\n")); !errors.Is(err, ErrSyntax) {
		t.Errorf("syntax error: err %v", err)
	}
	p.SetMemoryLimit(1 << 10)
	if _, err := p.Parse(Lookup("go"), []byte("package p\n"+strings.Repeat("var x = 1\n", 1000))); !errors.Is(err, ErrLimit) {
		t.Errorf("memory limit: err %v", err)
	}
	p.SetMemoryLimit(1 << 30)
	// The parser that failed parses again.
	if _, err := p.Parse(Lookup("go"), []byte("package p\n")); err != nil {
		t.Error(err)
	}
}

// Every Go file of the standard library's fmt, go/ast and sort packages
// parses, and each tree is well formed: parents come before their
// children, which lie inside them in order.
func TestTreesAreWellFormed(t *testing.T) {
	files, _ := filepath.Glob("*.go")
	for _, pkg := range []string{"fmt", "go/ast", "sort"} {
		more, _ := filepath.Glob(filepath.Join(runtime.GOROOT(), "src", pkg, "*.go"))
		files = append(files, more...)
	}
	if len(files) < 20 {
		t.Fatalf("found %d files", len(files))
	}
	p := parser(t)
	p.SetRetain(0)
	l := Lookup("go")
	for _, f := range files {
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		nodes, err := p.Parse(l, src)
		if err != nil {
			t.Errorf("%s: %v", f, err)
			continue
		}
		if r := nodes[0]; r.Parent != -1 || l.SymbolName(r.Symbol) != "source_file" || r.End != uint32(len(src)) {
			t.Errorf("%s: root %q [%d,%d) parent %d", f, l.SymbolName(r.Symbol), r.Start, r.End, r.Parent)
		}
		last := make([]uint32, len(nodes)) // end of each node's latest child
		for i, n := range nodes[1:] {
			i++
			par := n.Parent
			if par < 0 || int(par) >= i || n.Start > n.End || n.Start < nodes[par].Start || n.End > nodes[par].End ||
				n.Start < last[par] || int(n.Symbol) >= l.SymbolCount() {
				t.Errorf("%s: node %d %q [%d,%d) parent %d is out of place", f, i, l.SymbolName(n.Symbol), n.Start, n.End, par)
				break
			}
			last[par] = n.End
		}
	}
}

// Parsers on many goroutines don't share state.
func TestParsersRunTogether(t *testing.T) {
	src, err := os.ReadFile("tsasm.go")
	if err != nil {
		t.Fatal(err)
	}
	l := Lookup("go")
	nodes, err := parser(t).Parse(l, src)
	if err != nil {
		t.Fatal(err)
	}
	want := shape(l, nodes)
	var wg sync.WaitGroup
	for range 8 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			p, err := NewParser()
			if err != nil {
				t.Error(err)
				return
			}
			defer p.Close()
			for range 20 {
				nodes, err := p.Parse(l, src)
				if err != nil || shape(l, nodes) != want {
					t.Errorf("tree differs, err %v", err)
					return
				}
			}
		}()
	}
	wg.Wait()
}

func BenchmarkParseGo(b *testing.B) {
	src, err := os.ReadFile(filepath.Join(runtime.GOROOT(), "src", "fmt", "print.go"))
	if err != nil {
		b.Fatal(err)
	}
	p, l := parser(b), Lookup("go")
	b.SetBytes(int64(len(src)))
	for range b.N {
		if _, err := p.Parse(l, src); err != nil {
			b.Fatal(err)
		}
	}
}
