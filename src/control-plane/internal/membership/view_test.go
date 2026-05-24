// Phase K-2 — integration tests for the ClusterView publisher.
//
// Runs against a real embedded etcd (same fixture as registry_test.go)
// so the Watch + lease + Get round-trips exercise the actual etcd code
// rather than a mock.
package membership_test

import (
	"context"
	"encoding/json"
	"sort"
	"testing"
	"time"

	myetcd "github.com/Stephen-Pu/kvcache/control-plane/internal/etcd"
	"github.com/Stephen-Pu/kvcache/control-plane/internal/membership"
)

// readView pulls the published snapshot off etcd directly (i.e. via
// our own Get helper) so the assertions are independent of the
// publisher's atomic counter.
func readView(t *testing.T, ctx context.Context, cli *myetcd.Client) (membership.ClusterView, bool) {
	t.Helper()
	body, err := cli.Get(ctx, membership.ViewKey)
	if err != nil {
		t.Fatalf("read view: %v", err)
	}
	if len(body) == 0 {
		return membership.ClusterView{}, false
	}
	var v membership.ClusterView
	if err := json.Unmarshal(body, &v); err != nil {
		t.Fatalf("unmarshal view: %v", err)
	}
	return v, true
}

func waitForView(t *testing.T, ctx context.Context, cli *myetcd.Client,
	pred func(membership.ClusterView) bool, budget time.Duration) membership.ClusterView {
	t.Helper()
	deadline := time.Now().Add(budget)
	for time.Now().Before(deadline) {
		v, ok := readView(t, ctx, cli)
		if ok && pred(v) {
			return v
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("view never satisfied predicate within %s", budget)
	return membership.ClusterView{}
}

func TestClusterView_PublishesOnMembershipChange(t *testing.T) {
	ep, stop := startEmbeddedEtcd(t)
	defer stop()

	cli, err := myetcd.New(myetcd.Config{Endpoints: []string{ep}})
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer cli.Close()

	reg := membership.NewRegistry(cli)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// Run the publisher in the background.
	pub := &membership.ViewPublisher{
		Registry: reg,
		LeaderID: "test-leader",
		// Tight debounce so the test doesn't wait long.
		Debounce: 30 * time.Millisecond,
	}
	pubCtx, cancelPub := context.WithCancel(ctx)
	defer cancelPub()
	go func() { _ = pub.Run(pubCtx) }()

	// Bootstrap publish: empty membership but a view should still
	// appear so consumers can read SOMETHING immediately.
	v0 := waitForView(t, ctx, cli,
		func(v membership.ClusterView) bool {
			return v.LeaderID == "test-leader" && v.Epoch >= 1
		}, 2*time.Second)
	if len(v0.Nodes) != 0 {
		t.Errorf("bootstrap view should be empty, got %d nodes", len(v0.Nodes))
	}

	// Add two nodes; expect a view with 2 entries, epoch > v0.Epoch.
	for _, id := range []string{"node-a", "node-b"} {
		if _, err := reg.RegisterNode(ctx,
			membership.NodeDescriptor{
				NodeID: id, Host: "10.0.0." + id[len(id)-1:], GrpcPort: 7000,
			}, 5); err != nil {
			t.Fatalf("register %s: %v", id, err)
		}
	}
	v2 := waitForView(t, ctx, cli,
		func(v membership.ClusterView) bool { return len(v.Nodes) == 2 },
		3*time.Second)
	if v2.Epoch <= v0.Epoch {
		t.Errorf("epoch not monotonic: v0=%d v2=%d", v0.Epoch, v2.Epoch)
	}
	// Sorted by node_id deterministically.
	ids := []string{v2.Nodes[0].NodeID, v2.Nodes[1].NodeID}
	if !sort.StringsAreSorted(ids) {
		t.Errorf("nodes not sorted: %v", ids)
	}
	if v2.Nodes[0].GrpcPort != 7000 {
		t.Errorf("GrpcPort lost in round-trip: %+v", v2.Nodes[0])
	}
}

func TestClusterView_DebouncesBurstOfChanges(t *testing.T) {
	ep, stop := startEmbeddedEtcd(t)
	defer stop()

	cli, err := myetcd.New(myetcd.Config{Endpoints: []string{ep}})
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer cli.Close()

	reg := membership.NewRegistry(cli)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	pub := &membership.ViewPublisher{
		Registry: reg,
		LeaderID: "debounce-leader",
		Debounce: 200 * time.Millisecond,
	}
	pubCtx, cancelPub := context.WithCancel(ctx)
	defer cancelPub()
	go func() { _ = pub.Run(pubCtx) }()

	// Wait for bootstrap publish to settle.
	waitForView(t, ctx, cli,
		func(v membership.ClusterView) bool { return v.LeaderID == "debounce-leader" },
		2*time.Second)
	v0, _ := readView(t, ctx, cli)

	// Register 5 nodes in rapid succession (well under the debounce
	// window). After the window expires we expect ONE additional
	// publish carrying all 5 — not 5 separate publishes.
	for i := 0; i < 5; i++ {
		if _, err := reg.RegisterNode(ctx,
			membership.NodeDescriptor{
				NodeID: string('a' + rune(i)),
				Host:   "10.0.0.1",
			}, 5); err != nil {
			t.Fatalf("register: %v", err)
		}
	}
	// Wait long enough for at least the debounce window + a publish.
	v1 := waitForView(t, ctx, cli,
		func(v membership.ClusterView) bool { return len(v.Nodes) == 5 },
		3*time.Second)
	// At most a few extra epochs (the bootstrap-vs-first-event ordering
	// can yield 1-2 intermediate publishes legitimately). 5 separate
	// publishes for 5 events is the failure mode we're guarding
	// against.
	delta := v1.Epoch - v0.Epoch
	if delta > 2 {
		t.Errorf("debounce broken: %d publishes for 5 burst events (expected ≤ 2)",
			delta)
	}
}
