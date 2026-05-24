// Package etcd wraps the official `go.etcd.io/etcd/client/v3` client with
// helpers tailored to the Control Plane's idioms: TTL-bound leases, prefix
// reads, transactional CAS, and prefix watches with reconnection.
//
// LLD §4.1: every node registers under /nodes/<node_id> with a lease (default
// 10s TTL); CP leader-election uses an etcd lease on /cp/leader.
package etcd

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"os"
	"time"

	clientv3 "go.etcd.io/etcd/client/v3"
	"google.golang.org/grpc"
)

// Config is the minimum needed to dial a real etcd cluster.
type Config struct {
	Endpoints       []string
	DialTimeout     time.Duration
	CAPath          string
	ClientCertPath  string
	ClientKeyPath   string
	// Optional gRPC dial options (interceptors, etc.).
	DialOptions []grpc.DialOption
}

// Client is the wrapper. It owns the lifetime of the underlying clientv3.Client.
type Client struct {
	cli *clientv3.Client
}

func New(cfg Config) (*Client, error) {
	if len(cfg.Endpoints) == 0 {
		return nil, errors.New("etcd: at least one endpoint required")
	}
	tlsCfg, err := buildTLSConfig(cfg)
	if err != nil {
		return nil, err
	}
	c, err := clientv3.New(clientv3.Config{
		Endpoints:   cfg.Endpoints,
		DialTimeout: defaultDialTimeout(cfg.DialTimeout),
		TLS:         tlsCfg,
		DialOptions: cfg.DialOptions,
	})
	if err != nil {
		return nil, fmt.Errorf("etcd: dial: %w", err)
	}
	return &Client{cli: c}, nil
}

func defaultDialTimeout(d time.Duration) time.Duration {
	if d <= 0 {
		return 5 * time.Second
	}
	return d
}

func buildTLSConfig(cfg Config) (*tls.Config, error) {
	// Insecure if no material supplied — useful for dev and embedded-etcd
	// integration tests. Production CP always supplies mTLS material per
	// LLD §5.2.
	if cfg.CAPath == "" && cfg.ClientCertPath == "" && cfg.ClientKeyPath == "" {
		return nil, nil
	}
	if cfg.CAPath == "" || cfg.ClientCertPath == "" || cfg.ClientKeyPath == "" {
		return nil, errors.New("etcd: TLS material must include CA + cert + key")
	}
	caPEM, err := os.ReadFile(cfg.CAPath)
	if err != nil {
		return nil, fmt.Errorf("etcd: read CA: %w", err)
	}
	cert, err := tls.LoadX509KeyPair(cfg.ClientCertPath, cfg.ClientKeyPath)
	if err != nil {
		return nil, fmt.Errorf("etcd: load client cert: %w", err)
	}
	pool := x509.NewCertPool()
	if ok := pool.AppendCertsFromPEM(caPEM); !ok {
		return nil, errors.New("etcd: CA PEM had no certs")
	}
	return &tls.Config{
		RootCAs:      pool,
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	}, nil
}

// Close releases the underlying etcd client.
func (c *Client) Close() error { return c.cli.Close() }

// Raw exposes the underlying clientv3.Client for code that needs primitives
// we don't wrap (watches, txn). Treat this as an escape hatch.
func (c *Client) Raw() *clientv3.Client { return c.cli }

// LeaseGrant grants a lease with the given TTL.
func (c *Client) LeaseGrant(ctx context.Context, ttlSec int64) (clientv3.LeaseID, error) {
	r, err := c.cli.Grant(ctx, ttlSec)
	if err != nil {
		return 0, fmt.Errorf("etcd: lease grant: %w", err)
	}
	return r.ID, nil
}

// LeaseKeepAlive starts a KeepAlive stream. The returned channel receives one
// response per server-sent keepalive; the caller is responsible for cancelling
// `ctx` when the keepalive should stop.
func (c *Client) LeaseKeepAlive(ctx context.Context, id clientv3.LeaseID) (<-chan *clientv3.LeaseKeepAliveResponse, error) {
	ch, err := c.cli.KeepAlive(ctx, id)
	if err != nil {
		return nil, fmt.Errorf("etcd: keepalive: %w", err)
	}
	return ch, nil
}

// Put writes a key, optionally bound to a lease (0 = no lease).
func (c *Client) Put(ctx context.Context, key, value string, lease clientv3.LeaseID) error {
	opts := []clientv3.OpOption{}
	if lease != 0 {
		opts = append(opts, clientv3.WithLease(lease))
	}
	if _, err := c.cli.Put(ctx, key, value, opts...); err != nil {
		return fmt.Errorf("etcd: put: %w", err)
	}
	return nil
}

// Get returns the value at `key`, or nil bytes if the key is absent.
// Phase K-2 — used by ClusterView consumers that read a single
// versioned snapshot key.
func (c *Client) Get(ctx context.Context, key string) ([]byte, error) {
	resp, err := c.cli.Get(ctx, key)
	if err != nil {
		return nil, fmt.Errorf("etcd: get: %w", err)
	}
	if len(resp.Kvs) == 0 {
		return nil, nil
	}
	return resp.Kvs[0].Value, nil
}

// GetPrefix returns all KVs whose keys start with `prefix`, sorted by key.
func (c *Client) GetPrefix(ctx context.Context, prefix string) ([]KV, error) {
	resp, err := c.cli.Get(ctx, prefix,
		clientv3.WithPrefix(),
		clientv3.WithSort(clientv3.SortByKey, clientv3.SortAscend))
	if err != nil {
		return nil, fmt.Errorf("etcd: get prefix: %w", err)
	}
	out := make([]KV, 0, len(resp.Kvs))
	for _, kv := range resp.Kvs {
		out = append(out, KV{
			Key:         string(kv.Key),
			Value:       string(kv.Value),
			ModRevision: kv.ModRevision,
			LeaseID:     clientv3.LeaseID(kv.Lease),
		})
	}
	return out, nil
}

// Delete removes a key. Returns true if the key existed.
func (c *Client) Delete(ctx context.Context, key string) (bool, error) {
	resp, err := c.cli.Delete(ctx, key)
	if err != nil {
		return false, fmt.Errorf("etcd: delete: %w", err)
	}
	return resp.Deleted > 0, nil
}

// KV is the wire-friendly representation we hand back to callers.
type KV struct {
	Key         string
	Value       string
	ModRevision int64
	LeaseID     clientv3.LeaseID
}
