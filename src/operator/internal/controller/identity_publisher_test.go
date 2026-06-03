// KVCacheTenant identity publisher — unit tests (Phase B8.4).
package controller

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"sync"
	"testing"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"

	kvcachev1alpha1 "github.com/Stephen-Pu/kvcache/operator/api/v1alpha1"
)

func ident(spiffe, cn, kind string) kvcachev1alpha1.TenantIdentity {
	return kvcachev1alpha1.TenantIdentity{SpiffeID: spiffe, CN: cn, Kind: kind}
}

// ---- pure helpers ---------------------------------------------------------

func TestIdentitiesPrefixShape(t *testing.T) {
	got := IdentitiesPrefixFor("demo", "deadbeefcafebabe1234567890abcdef")
	want := "/kvcache/identities/demo/deadbeefcafebabe1234567890abcdef/"
	if got != want {
		t.Errorf("IdentitiesPrefixFor = %q, want %q", got, want)
	}
	// Must live under the node-side watcher's GetPrefix root.
	if !strings.HasPrefix(got, "/kvcache/identities/") {
		t.Errorf("prefix %q not under watcher root", got)
	}
}

func TestSafeIdentityKeyIsDeterministicAndCollisionResistant(t *testing.T) {
	a := ident("spiffe://td/tenant/acme", "", "")
	// Same identity → same key (re-publish overwrites in place).
	if safeIdentityKey(a) != safeIdentityKey(a) {
		t.Errorf("safeIdentityKey not deterministic")
	}
	// Sanitised segment contains no '/' or ':' from the raw SPIFFE URI.
	k := safeIdentityKey(a)
	if strings.ContainsAny(k, "/:") {
		t.Errorf("key %q contains unsafe chars", k)
	}
	// Two distinct identities that sanitise to the same prefix still differ
	// (the appended hash disambiguates).
	b := ident("spiffe://td/tenant/acm/e", "", "") // sanitises to same shape as 'a'
	if safeIdentityKey(a) == safeIdentityKey(b) {
		t.Errorf("distinct identities collided: %q", safeIdentityKey(a))
	}
	// CN-only identity keys off the CN.
	c := ident("", "legacy.client", "")
	if safeIdentityKey(c) == safeIdentityKey(a) {
		t.Errorf("cn identity collided with spiffe identity")
	}
}

func TestDesiredEntriesMatchesWatcherContract(t *testing.T) {
	cluster := sampleCluster()
	tenant := sampleTenant("t-id", cluster.Name)
	tenant.Spec.AllowedIdentities = []kvcachev1alpha1.TenantIdentity{
		ident("spiffe://td/tenant/acme", "", "tenant"),
		ident("", "legacy.client", ""), // kind defaults to "tenant"
		ident("", "", ""),              // unkeyable → skipped
	}

	entries, err := desiredEntries(cluster, tenant)
	if err != nil {
		t.Fatalf("desiredEntries: %v", err)
	}
	if len(entries) != 2 {
		t.Fatalf("expected 2 entries (1 skipped), got %d", len(entries))
	}

	// Every body must parse into the exact fields the node watcher reads.
	sawSpiffe, sawCN := false, false
	for key, body := range entries {
		if !strings.HasPrefix(key, IdentitiesPrefixFor(cluster.Name, tenant.Spec.TenantID)) {
			t.Errorf("key %q outside tenant subtree", key)
		}
		var p IdentityEtcdPayload
		if err := json.Unmarshal([]byte(body), &p); err != nil {
			t.Fatalf("body %q not JSON: %v", body, err)
		}
		if p.Tenant != tenant.Spec.TenantID {
			t.Errorf("payload tenant = %q, want %q", p.Tenant, tenant.Spec.TenantID)
		}
		if p.Kind != "tenant" {
			t.Errorf("payload kind = %q, want defaulted 'tenant'", p.Kind)
		}
		if p.SpiffeID == "spiffe://td/tenant/acme" {
			sawSpiffe = true
		}
		if p.CN == "legacy.client" {
			sawCN = true
		}
		// Field names must be snake_case for the C++ nlohmann parser.
		if !strings.Contains(body, `"tenant"`) {
			t.Errorf("body missing snake_case tenant field: %s", body)
		}
	}
	if !sawSpiffe || !sawCN {
		t.Errorf("expected both spiffe + cn entries; spiffe=%v cn=%v", sawSpiffe, sawCN)
	}
}

// ---- controller integration via fake publisher ---------------------------

// fakeIdentityPublisher records calls; optional injected error for one call.
type fakeIdentityPublisher struct {
	mu      sync.Mutex
	calls   int
	lastN   int
	nextErr error
}

func (f *fakeIdentityPublisher) PublishIdentities(_ context.Context,
	_ *kvcachev1alpha1.KVCacheCluster,
	tenant *kvcachev1alpha1.KVCacheTenant) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if err := f.nextErr; err != nil {
		f.nextErr = nil
		return err
	}
	f.calls++
	f.lastN = len(tenant.Spec.AllowedIdentities)
	return nil
}

func TestValidTenantPublishesIdentities(t *testing.T) {
	cluster := sampleCluster()
	tenant := sampleTenant("t-id", cluster.Name)
	tenant.Spec.AllowedIdentities = []kvcachev1alpha1.TenantIdentity{
		ident("spiffe://td/tenant/acme", "", "tenant"),
	}
	pub := &fakeIdentityPublisher{}
	r, cli := newTenantReconciler(t, cluster, tenant)
	r.IdentityPublisher = pub
	reconcileTenant(t, r, tenant)

	if pub.calls != 1 {
		t.Fatalf("expected 1 PublishIdentities call, got %d", pub.calls)
	}
	cond := getCondition(t, cli, tenant.Name, tenant.Namespace, identitiesPublishedConditionType)
	if cond == nil || cond.Status != metav1.ConditionTrue {
		t.Fatalf("expected IdentitiesPublished=True, got %+v", cond)
	}
	if !strings.Contains(cond.Message, "1 identities") {
		t.Errorf("condition message = %q, want count", cond.Message)
	}
}

func TestInvalidIdentityFailsValidationAndDoesNotPublish(t *testing.T) {
	cases := []struct {
		name string
		id   kvcachev1alpha1.TenantIdentity
	}{
		{"neither-spiffe-nor-cn", ident("", "", "tenant")},
		{"bad-spiffe-scheme", ident("https://td/x", "", "")},
		{"bad-kind", ident("", "cn1", "superuser")},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cluster := sampleCluster()
			tenant := sampleTenant("t-id", cluster.Name)
			tenant.Spec.AllowedIdentities = []kvcachev1alpha1.TenantIdentity{tc.id}
			pub := &fakeIdentityPublisher{}
			r, cli := newTenantReconciler(t, cluster, tenant)
			r.IdentityPublisher = pub
			reconcileTenant(t, r, tenant)

			v := getValidatedCondition(t, cli, tenant.Name, tenant.Namespace)
			if v == nil || v.Status != metav1.ConditionFalse || v.Reason != "InvalidIdentity" {
				t.Fatalf("expected Validated=False/InvalidIdentity, got %+v", v)
			}
			if pub.calls != 0 {
				t.Errorf("invalid identity must not publish, got %d calls", pub.calls)
			}
		})
	}
}

func TestIdentityPublishFailureSurfacesConditionAndRequeues(t *testing.T) {
	cluster := sampleCluster()
	tenant := sampleTenant("t-id", cluster.Name)
	tenant.Spec.AllowedIdentities = []kvcachev1alpha1.TenantIdentity{
		ident("spiffe://td/tenant/acme", "", ""),
	}
	pub := &fakeIdentityPublisher{nextErr: errors.New("etcd unreachable")}
	r, cli := newTenantReconciler(t, cluster, tenant)
	r.IdentityPublisher = pub

	res, err := r.Reconcile(context.Background(), ctrl.Request{
		NamespacedName: types.NamespacedName{Name: tenant.Name, Namespace: tenant.Namespace},
	})
	if err != nil {
		t.Fatalf("reconcile returned hard error: %v", err)
	}
	if res.RequeueAfter <= 0 {
		t.Errorf("expected RequeueAfter on publish failure, got %+v", res)
	}
	cond := getCondition(t, cli, tenant.Name, tenant.Namespace, identitiesPublishedConditionType)
	if cond == nil || cond.Status != metav1.ConditionFalse {
		t.Fatalf("expected IdentitiesPublished=False, got %+v", cond)
	}
	if !strings.Contains(cond.Message, "etcd unreachable") {
		t.Errorf("message = %q, want underlying error", cond.Message)
	}
}

func TestTenantWithNoIdentitiesStillPublishesEmpty(t *testing.T) {
	// A tenant with no allow-list is valid; the publisher is still called
	// (so a previously-published identity gets pruned), condition reports 0.
	cluster := sampleCluster()
	tenant := sampleTenant("t-id", cluster.Name) // no AllowedIdentities
	pub := &fakeIdentityPublisher{}
	r, cli := newTenantReconciler(t, cluster, tenant)
	r.IdentityPublisher = pub
	reconcileTenant(t, r, tenant)

	if pub.calls != 1 {
		t.Fatalf("expected 1 call even with empty allow-list, got %d", pub.calls)
	}
	cond := getCondition(t, cli, tenant.Name, tenant.Namespace, identitiesPublishedConditionType)
	if cond == nil || cond.Status != metav1.ConditionTrue {
		t.Fatalf("expected IdentitiesPublished=True, got %+v", cond)
	}
	if !strings.Contains(cond.Message, "0 identities") {
		t.Errorf("message = %q, want '0 identities'", cond.Message)
	}
}
