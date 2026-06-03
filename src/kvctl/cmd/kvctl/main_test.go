// Phase A2 — kvctl smoke tests.
//
// These cover the pure-function helpers (no etcd required), the
// stub-command exit messaging, and the /metrics parser. They don't
// stand up an embedded etcd — the CP-side server tests already do
// that and the kvctl members command is the same Get() call shape.
package main

import (
	"bytes"
	"strings"
	"testing"

	nodepb "github.com/Stephen-Pu/kvcache/kvctl/internal/nodepb"
)

func TestSummariseMetrics_BasicShape(t *testing.T) {
	lines := []string{
		"kv_pinned_used_bytes 1024",
		`kv_tier_hits_total{tier="dram"} 42`,
		"kv_invalid_line_no_value",          // skipped
		`kv_floaty 3.14`,
	}
	m := summariseMetrics(lines)
	if got := m["kv_pinned_used_bytes"]; got != 1024 {
		t.Errorf("kv_pinned_used_bytes = %g, want 1024", got)
	}
	if got := m[`kv_tier_hits_total{tier="dram"}`]; got != 42 {
		t.Errorf("kv_tier_hits_total = %g, want 42", got)
	}
	if got := m["kv_floaty"]; got != 3.14 {
		t.Errorf("kv_floaty = %g, want 3.14", got)
	}
	if _, ok := m["kv_invalid_line_no_value"]; ok {
		t.Error("malformed line should have been skipped")
	}
}

func TestFilterMetricsByPrefix(t *testing.T) {
	lines := []string{
		"kv_pinned_used_bytes 1",
		"kv_tier_x 2",
		"kv_dram_x 3",
		"kv_unrelated 4",
	}
	got := filterMetricsByPrefix(lines, "kv_tier_", "kv_dram_", "kv_pinned_")
	if len(got) != 3 {
		t.Fatalf("got %d entries, want 3: %v", len(got), got)
	}
	if _, ok := got["kv_unrelated"]; ok {
		t.Error("kv_unrelated should have been filtered out")
	}
}

// Phase A2.1 — drainKey must match the CP's membership.DrainKey format
// byte-for-byte, or kvctl writes a marker the CP never reads. This
// guards that contract without standing up etcd.
func TestDrainKeyMatchesCpFormat(t *testing.T) {
	if got := drainKey("prod", "node-7"); got != "/kvcache/drain/prod/node-7" {
		t.Errorf("drainKey = %q, want /kvcache/drain/prod/node-7", got)
	}
	if got := drainKey(defaultClusterID, "n"); got != "/kvcache/drain/kvcache/n" {
		t.Errorf("drainKey default cluster = %q", got)
	}
}

func TestVersionCommand(t *testing.T) {
	c := newVersionCmd()
	var out bytes.Buffer
	c.SetOut(&out)
	if err := c.Execute(); err != nil {
		t.Fatalf("version: %v", err)
	}
	if !strings.Contains(out.String(), "kvctl ") {
		t.Errorf("version output missing 'kvctl '; got: %q", out.String())
	}
}

func TestIndentJSON_PrettyPrintsAndDegradesGracefully(t *testing.T) {
	pretty := indentJSON([]byte(`{"a":1,"b":[2,3]}`))
	if !strings.Contains(pretty, "\"a\":") {
		t.Errorf("indentJSON didn't reformat: %s", pretty)
	}
	// Garbage in → garbage back out, no crash.
	got := indentJSON([]byte(`not json at all`))
	if got != "not json at all" {
		t.Errorf("indentJSON should pass garbage through verbatim; got %q", got)
	}
}

func TestDefaultStr(t *testing.T) {
	if defaultStr("", "fb") != "fb" {
		t.Error("empty -> fallback")
	}
	if defaultStr("v", "fb") != "v" {
		t.Error("non-empty -> value")
	}
}

func TestEnvOr_FallsBack(t *testing.T) {
	// Unlikely-to-exist env var.
	if got := envOr("KVCTL_UNLIKELY_VAR_xyz123", "fb"); got != "fb" {
		t.Errorf("envOr missing var should fall back; got %q", got)
	}
	t.Setenv("KVCTL_SET_VAR", "real")
	if got := envOr("KVCTL_SET_VAR", "fb"); got != "real" {
		t.Errorf("envOr set var; got %q want real", got)
	}
	// Empty-string env var should also fall back.
	t.Setenv("KVCTL_EMPTY_VAR", "")
	if got := envOr("KVCTL_EMPTY_VAR", "fb"); got != "fb" {
		t.Errorf("envOr empty var should fall back; got %q", got)
	}
}

// ===== Phase A2.2-trace — kvctl trace =======================================

func TestFormatEvent(t *testing.T) {
	ev := &nodepb.Event{
		Type:     nodepb.EventType_EVENT_ADD,
		Tier:     nodepb.Tier_TIER_PINNED,
		Epoch:    42,
		NodeId:   "node-a",
		UnixNano: 0, // → "—" timestamp, deterministic
		Locator: &nodepb.Locator{
			TenantId:   []byte("0123456789abcdef"),
			PrefixHash: []byte("fedcba9876543210"),
		},
	}
	line := formatEvent(ev)
	for _, want := range []string{"ADD", "PINNED", "epoch=42", "node=node-a"} {
		if !strings.Contains(line, want) {
			t.Errorf("formatEvent missing %q in: %s", want, line)
		}
	}
	// EVENT_/TIER_ prefixes must be stripped for readability.
	if strings.Contains(line, "EVENT_") || strings.Contains(line, "TIER_") {
		t.Errorf("enum prefixes should be stripped: %s", line)
	}
}

func TestShortHex(t *testing.T) {
	if shortHex("abcd") != "abcd" {
		t.Error("short string unchanged")
	}
	long := "0123456789abcdef0123"
	got := shortHex(long)
	if !strings.HasPrefix(got, "0123456789ab") || !strings.HasSuffix(got, "…") {
		t.Errorf("shortHex truncation wrong: %q", got)
	}
}

func TestTraceRequiresNodeAndTenant(t *testing.T) {
	g := &globalFlags{}
	// No --node → error.
	c := newTraceCmd(g)
	c.SetArgs([]string{})
	var e1 bytes.Buffer
	c.SetErr(&e1)
	c.SetOut(&e1)
	if err := c.Execute(); err == nil {
		t.Fatal("trace without --node must error")
	}

	// --node but no --tenant → error.
	c2 := newTraceCmd(g)
	c2.SetArgs([]string{"--node", "127.0.0.1:7100"})
	var e2 bytes.Buffer
	c2.SetErr(&e2)
	c2.SetOut(&e2)
	if err := c2.Execute(); err == nil {
		t.Fatal("trace without --tenant must error")
	}

	// bad hex tenant → error (before any dial).
	c3 := newTraceCmd(g)
	c3.SetArgs([]string{"--node", "127.0.0.1:7100", "--tenant", "nothex!!"})
	var e3 bytes.Buffer
	c3.SetErr(&e3)
	c3.SetOut(&e3)
	if err := c3.Execute(); err == nil {
		t.Fatal("trace with non-hex tenant must error")
	}
}
