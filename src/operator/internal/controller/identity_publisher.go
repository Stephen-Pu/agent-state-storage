// LLD §8.2 / §5.2 — Operator-side KVCacheTenant → etcd identity publisher.
//
// Phase B8.4 is the producer half of the mTLS identity-table pipeline whose
// consumer (the node-side IdentityWatcher) shipped in B8.3. The operator
// translates a tenant's `spec.allowedIdentities` allow-list into one etcd
// entry per identity under
//
//   /kvcache/identities/<cluster>/<tenantID>/<safe-id>
//
// The node-side IdentityWatcher does a GetPrefix on "/kvcache/identities/"
// (it ignores the key suffix and keys the MtlsRegistry off the JSON value's
// spiffe_id / cn fields), so the deeper per-tenant nesting here is invisible
// to it — it exists only so the publisher can scope a GetPrefix to *this
// tenant's* entries and prune the ones a reconcile removed.
//
// Why operator-side: same rationale as TenantPublisher — the operator is
// already the one K8s-API-aware process dialing etcd, so the publish path
// stays in one binary and the CP/node stay pure etcd watchers.
//
// Revocation correctness: dropping an identity from the spec MUST stop
// granting it access, so Publish does a put-all-then-prune: it Puts every
// desired entry, then GetPrefixes this tenant's subtree and Deletes any key
// not in the desired set. A pure Put-only publisher would leave a removed
// identity authorized forever — a security bug, not just stale data.
package controller

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	clientv3 "go.etcd.io/etcd/client/v3"

	kvcachev1alpha1 "github.com/Stephen-Pu/kvcache/operator/api/v1alpha1"
)

// IdentityPublisher writes a tenant's allowed mTLS identities into the
// per-cluster etcd that the node-side IdentityWatcher (B8.3) consumes.
//
//   * EtcdIdentityPublisher — production; dials the cluster's etcd.
//   * Tests inject a fake to drive the IdentitiesPublished condition path.
type IdentityPublisher interface {
	PublishIdentities(ctx context.Context,
		cluster *kvcachev1alpha1.KVCacheCluster,
		tenant  *kvcachev1alpha1.KVCacheTenant) error
}

// IdentityEtcdPayload is the JSON document written per identity. The field
// names MUST match what the node-side IdentityWatcher::ParseEntry reads
// (spiffe_id / cn / tenant / kind); extra fields (schema_version) are
// ignored by the watcher's permissive parser.
type IdentityEtcdPayload struct {
	SpiffeID      string `json:"spiffe_id,omitempty"`
	CN            string `json:"cn,omitempty"`
	Tenant        string `json:"tenant"`
	Kind          string `json:"kind,omitempty"`
	SchemaVersion int    `json:"schema_version"`
}

const identityEtcdSchemaVersion = 1

// IdentitiesPrefixFor returns the per-(cluster,tenant) etcd subtree that
// holds this tenant's identity entries (trailing slash for GetPrefix).
func IdentitiesPrefixFor(clusterName, tenantID string) string {
	return fmt.Sprintf("/kvcache/identities/%s/%s/", clusterName, tenantID)
}

// safeIdentityKey derives a stable, etcd-safe key segment for one identity.
// The SPIFFE id (preferred) or CN is sanitised to [A-Za-z0-9._-] for
// readability, then a short hash of the raw string is appended so two
// identities that sanitise to the same prefix never collide. Deterministic:
// the same identity always maps to the same key, so a re-publish overwrites
// in place rather than orphaning.
func safeIdentityKey(id kvcachev1alpha1.TenantIdentity) string {
	raw := id.SpiffeID
	if raw == "" {
		raw = "cn:" + id.CN
	}
	var b strings.Builder
	for _, r := range raw {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z',
			r >= '0' && r <= '9', r == '.', r == '_', r == '-':
			b.WriteRune(r)
		default:
			b.WriteRune('-')
		}
	}
	sum := sha256.Sum256([]byte(raw))
	short := hex.EncodeToString(sum[:])[:8]
	san := b.String()
	if len(san) > 80 { // keep keys bounded
		san = san[:80]
	}
	return san + "-" + short
}

// desiredEntries builds the {etcd-key → JSON-body} map the publisher wants
// present for this tenant. Identities with neither a SPIFFE id nor a CN are
// skipped (the watcher would reject them anyway); validation should have
// caught those upstream, but skipping here keeps the publish total-correct.
func desiredEntries(cluster *kvcachev1alpha1.KVCacheCluster,
	tenant *kvcachev1alpha1.KVCacheTenant) (map[string]string, error) {
	prefix := IdentitiesPrefixFor(cluster.Name, tenant.Spec.TenantID)
	out := make(map[string]string, len(tenant.Spec.AllowedIdentities))
	for _, id := range tenant.Spec.AllowedIdentities {
		if id.SpiffeID == "" && id.CN == "" {
			continue
		}
		kind := id.Kind
		if kind == "" {
			kind = "tenant"
		}
		body, err := json.Marshal(IdentityEtcdPayload{
			SpiffeID:      id.SpiffeID,
			CN:            id.CN,
			Tenant:        tenant.Spec.TenantID,
			Kind:          kind,
			SchemaVersion: identityEtcdSchemaVersion,
		})
		if err != nil {
			return nil, fmt.Errorf("marshal identity: %w", err)
		}
		out[prefix+safeIdentityKey(id)] = string(body)
	}
	return out, nil
}

// EtcdIdentityPublisher dials the cluster's etcd (same endpoint discovery as
// EtcdTenantPublisher) and reconciles this tenant's identity subtree.
type EtcdIdentityPublisher struct {
	DialTimeout time.Duration
}

var _ IdentityPublisher = (*EtcdIdentityPublisher)(nil)

func (p *EtcdIdentityPublisher) PublishIdentities(ctx context.Context,
	cluster *kvcachev1alpha1.KVCacheCluster,
	tenant  *kvcachev1alpha1.KVCacheTenant) error {

	timeout := p.DialTimeout
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	endpoints := EtcdEndpointsFor(cluster)
	if len(endpoints) == 0 {
		return fmt.Errorf("etcd endpoints unavailable for cluster %q", cluster.Name)
	}
	bare := make([]string, 0, len(endpoints))
	for _, e := range endpoints {
		bare = append(bare, stripScheme(e))
	}

	cli, err := clientv3.New(clientv3.Config{
		Endpoints:   bare,
		DialTimeout: timeout,
	})
	if err != nil {
		return fmt.Errorf("etcd dial: %w", err)
	}
	defer cli.Close()

	desired, err := desiredEntries(cluster, tenant)
	if err != nil {
		return err
	}

	cctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	// 1. Put every desired entry.
	for key, body := range desired {
		if _, err := cli.Put(cctx, key, body); err != nil {
			return fmt.Errorf("etcd put %q: %w", key, err)
		}
	}

	// 2. Prune: list this tenant's subtree, delete keys no longer desired.
	prefix := IdentitiesPrefixFor(cluster.Name, tenant.Spec.TenantID)
	resp, err := cli.Get(cctx, prefix, clientv3.WithPrefix())
	if err != nil {
		return fmt.Errorf("etcd list %q: %w", prefix, err)
	}
	for _, kv := range resp.Kvs {
		key := string(kv.Key)
		if _, keep := desired[key]; keep {
			continue
		}
		if _, err := cli.Delete(cctx, key); err != nil {
			return fmt.Errorf("etcd prune %q: %w", key, err)
		}
	}
	return nil
}
