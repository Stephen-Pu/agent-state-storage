// Integration test against a *real* embedded etcd. Verifies:
//
//   * Register + lease keepalive keeps the node visible.
//   * Letting the lease expire removes the node from ListNodes within ~TTL.
//   * Watch delivers ADD events on register and DELETE events on expiry.
//
// The embedded etcd is started in-process so the test has no external
// dependency. This is genuinely the etcd server code (`go.etcd.io/etcd/server/v3/embed`).
package membership_test

import (
	"context"
	"net/url"
	"path/filepath"
	"testing"
	"time"

	"go.etcd.io/etcd/server/v3/embed"

	myetcd "github.com/alluxio/kvcache/control-plane/internal/etcd"
	"github.com/alluxio/kvcache/control-plane/internal/membership"
)

func startEmbeddedEtcd(t *testing.T) (string, func()) {
	t.Helper()
	dir := t.TempDir()
	cfg := embed.NewConfig()
	cfg.Dir = filepath.Join(dir, "etcd")
	// Pick free local URLs; using port 0 makes etcd choose ephemeral ports.
	clientURL, _ := url.Parse("http://127.0.0.1:0")
	peerURL, _ := url.Parse("http://127.0.0.1:0")
	cfg.ListenClientUrls = []url.URL{*clientURL}
	cfg.AdvertiseClientUrls = []url.URL{*clientURL}
	cfg.ListenPeerUrls = []url.URL{*peerURL}
	cfg.AdvertisePeerUrls = []url.URL{*peerURL}
	cfg.InitialCluster = "default=" + peerURL.String()
	cfg.LogLevel = "error"

	e, err := embed.StartEtcd(cfg)
	if err != nil {
		t.Fatalf("embed etcd: %v", err)
	}
	select {
	case <-e.Server.ReadyNotify():
	case <-time.After(20 * time.Second):
		e.Close()
		t.Fatal("embed etcd: not ready in 20s")
	}
	endpoint := e.Clients[0].Addr().String()
	return endpoint, func() { e.Close() }
}

func TestRegistry_RegisterAndExpire(t *testing.T) {
	ep, stop := startEmbeddedEtcd(t)
	defer stop()

	cli, err := myetcd.New(myetcd.Config{Endpoints: []string{ep}})
	if err != nil {
		t.Fatalf("dial etcd: %v", err)
	}
	defer cli.Close()

	reg := membership.NewRegistry(cli)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	n := membership.NodeDescriptor{NodeID: "node-1", Host: "10.0.0.1", Vcpus: 4}
	lease, err := reg.RegisterNode(ctx, n, 2 /*ttl seconds*/)
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	if lease == 0 {
		t.Fatal("expected non-zero lease")
	}

	// Immediately visible.
	list, err := reg.ListNodes(ctx)
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	if len(list) != 1 || list[0].NodeID != "node-1" {
		t.Fatalf("expected node-1; got %+v", list)
	}

	// Don't KeepAlive — let the lease expire.
	time.Sleep(3 * time.Second)
	list, err = reg.ListNodes(ctx)
	if err != nil {
		t.Fatalf("list after expiry: %v", err)
	}
	if len(list) != 0 {
		t.Fatalf("expected empty list after lease expiry; got %+v", list)
	}
}

func TestRegistry_WatchDeliversEvents(t *testing.T) {
	ep, stop := startEmbeddedEtcd(t)
	defer stop()

	cli, err := myetcd.New(myetcd.Config{Endpoints: []string{ep}})
	if err != nil {
		t.Fatalf("dial etcd: %v", err)
	}
	defer cli.Close()

	reg := membership.NewRegistry(cli)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	events, err := reg.Watch(ctx)
	if err != nil {
		t.Fatalf("watch: %v", err)
	}

	n := membership.NodeDescriptor{NodeID: "node-A", Host: "h1"}
	if _, err := reg.RegisterNode(ctx, n, 2); err != nil {
		t.Fatalf("register: %v", err)
	}

	select {
	case ev := <-events:
		if ev.Type != membership.EventAdd || ev.Node.NodeID != "node-A" {
			t.Fatalf("unexpected first event: %+v", ev)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no add event in 5s")
	}

	// Wait for lease to expire → expect a delete event.
	select {
	case ev := <-events:
		if ev.Type != membership.EventDelete {
			t.Fatalf("expected delete, got %v", ev)
		}
	case <-time.After(6 * time.Second):
		t.Fatal("no delete event in 6s")
	}
}
