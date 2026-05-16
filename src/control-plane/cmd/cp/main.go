// Control Plane entrypoint. LLD §4.1.
//
// Boot sequence:
//   1. Read config from env / flags (etcd endpoints, TLS material, listen addr).
//   2. Dial etcd.
//   3. Campaign for leadership via etcd Election (lease TTL 10 s by default).
//   4. As leader: start the membership reconciler + bloom fan-out + config push.
//   5. As follower: serve read-only RPCs (delegate writes to leader).
//
// The gRPC server surface mapping the cp.proto schema lives in
// internal/server/ (TODO(stephen) — Step-8 once the full proto codegen is wired).
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	clientv3 "go.etcd.io/etcd/client/v3"
	"go.etcd.io/etcd/client/v3/concurrency"

	myetcd "github.com/alluxio/kvcache/control-plane/internal/etcd"
	"github.com/alluxio/kvcache/control-plane/internal/membership"
)

func main() {
	endpoints := flag.String("etcd-endpoints", env("KVCACHE_ETCD_ENDPOINTS", "127.0.0.1:2379"),
		"comma-separated list of etcd endpoints")
	caPath := flag.String("etcd-ca", env("KVCACHE_ETCD_CA", ""), "path to etcd CA cert")
	certPath := flag.String("etcd-cert", env("KVCACHE_ETCD_CERT", ""), "client cert")
	keyPath := flag.String("etcd-key", env("KVCACHE_ETCD_KEY", ""), "client key")
	leaderElectionPath := flag.String("election", "/cp/leader", "etcd path for leader election")
	leaseTTL := flag.Int("lease-ttl", 10, "leader-election lease TTL in seconds")
	flag.Parse()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	cli, err := myetcd.New(myetcd.Config{
		Endpoints:      strings.Split(*endpoints, ","),
		DialTimeout:    5 * time.Second,
		CAPath:         *caPath,
		ClientCertPath: *certPath,
		ClientKeyPath:  *keyPath,
	})
	if err != nil {
		log.Fatalf("control-plane: connect etcd: %v", err)
	}
	defer cli.Close()

	log.Printf("control-plane: connected to etcd at %s", *endpoints)

	registry := membership.NewRegistry(cli)

	// Leader election. Followers still serve read-only queries (list nodes,
	// stream watches) but writes (quota updates, bloom fan-out) are leader-only.
	go runElection(ctx, cli.Raw(), *leaderElectionPath, int64(*leaseTTL), registry)

	<-ctx.Done()
	log.Println("control-plane: shutting down")
}

func env(name, fallback string) string {
	if v, ok := os.LookupEnv(name); ok && v != "" {
		return v
	}
	return fallback
}

func runElection(ctx context.Context, cli *clientv3.Client, electionPath string,
	leaseTTL int64, registry *membership.Registry) {
	for ctx.Err() == nil {
		sess, err := concurrency.NewSession(cli, concurrency.WithTTL(int(leaseTTL)))
		if err != nil {
			log.Printf("control-plane: election session: %v", err)
			time.Sleep(time.Second)
			continue
		}
		e := concurrency.NewElection(sess, electionPath)
		hostname, _ := os.Hostname()
		ident := fmt.Sprintf("cp-%s-%d", hostname, time.Now().UnixNano())
		log.Printf("control-plane: campaigning for leadership as %s", ident)
		if err := e.Campaign(ctx, ident); err != nil {
			log.Printf("control-plane: campaign failed: %v", err)
			sess.Close()
			continue
		}
		log.Printf("control-plane: ELECTED leader as %s", ident)
		runLeaderDuties(ctx, registry)
		_ = e.Resign(context.Background())
		sess.Close()
	}
}

// runLeaderDuties is the work loop that only the leader executes. Today it
// just streams membership events as a sanity log. Step-8 will add quota
// reconciliation and bloom-sketch fan-out.
func runLeaderDuties(ctx context.Context, registry *membership.Registry) {
	events, err := registry.Watch(ctx)
	if err != nil {
		log.Printf("control-plane: membership watch: %v", err)
		return
	}
	for ev := range events {
		switch ev.Type {
		case membership.EventAdd:
			log.Printf("[leader] node ADD    id=%s host=%s vcpus=%d",
				ev.Node.NodeID, ev.Node.Host, ev.Node.Vcpus)
		case membership.EventUpdate:
			log.Printf("[leader] node UPDATE id=%s", ev.Node.NodeID)
		case membership.EventDelete:
			log.Printf("[leader] node DELETE id=%s (lease lost or graceful exit)",
				ev.Node.NodeID)
		}
	}
}
