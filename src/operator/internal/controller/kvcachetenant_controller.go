// LLD §8.2 — KVCacheTenant controller.
//
// Each KVCacheTenant CR declares one tenant against a parent
// KVCacheCluster. The operator's job here is *validation only* — the
// real "push quota to nodes" path goes
//
//   KVCacheTenant CR  →  control-plane (watches the K8s API)
//                     →  etcd  /quota/<tenant_id>
//                     →  kvstore-node (etcd watch)
//
// The CP-side watcher lands in Phase H-4. Today the controller checks:
//
//   * the spec is internally consistent (tenant_id is 32 hex chars; the
//     resource.Quantity strings parse; the default priority is one of the
//     three allowed values),
//   * the referenced KVCacheCluster exists in the same namespace.
//
// It updates Status.Conditions with a single `Validated` condition (True
// / False with `Reason`). Callers / users can `kubectl get
// kvcachetenants` and see at a glance which tenants are well-formed.
package controller

import (
	"context"
	"encoding/hex"
	"fmt"
	"strings"
	"time"

	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"

	kvcachev1alpha1 "github.com/Stephen-Pu/kvcache/operator/api/v1alpha1"
)

// KVCacheTenantReconciler reconciles a KVCacheTenant object.
type KVCacheTenantReconciler struct {
	client.Client
	Scheme *runtime.Scheme

	// Publisher pushes validated tenant CRs into the per-cluster etcd
	// (Phase H-4). Production binding is EtcdTenantPublisher; tests
	// inject a fake. nil = skip the publish step entirely (no-op),
	// useful for environments where etcd is wired separately.
	Publisher TenantPublisher

	// IdentityPublisher pushes the tenant's mTLS identity allow-list into
	// etcd (Phase B8.4) for the node-side IdentityWatcher to consume.
	// Production binding is EtcdIdentityPublisher; tests inject a fake.
	// nil = skip identity publishing.
	IdentityPublisher IdentityPublisher
}

// +kubebuilder:rbac:groups=kvcache.io,resources=kvcachetenants,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=kvcache.io,resources=kvcachetenants/status,verbs=get;update;patch

const (
	validatedConditionType           = "Validated"
	publishedConditionType           = "Published"
	identitiesPublishedConditionType = "IdentitiesPublished"
)

func (r *KVCacheTenantReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	var tenant kvcachev1alpha1.KVCacheTenant
	if err := r.Get(ctx, req.NamespacedName, &tenant); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	reason, msg := validateTenant(ctx, r.Client, &tenant)
	validated := metav1.Condition{
		Type:               validatedConditionType,
		LastTransitionTime: metav1.Now(),
	}
	if reason == "" {
		validated.Status = metav1.ConditionTrue
		validated.Reason = "Accepted"
	} else {
		validated.Status = metav1.ConditionFalse
		validated.Reason = reason
		validated.Message = msg
	}
	changed := replaceCondition(&tenant.Status.Conditions, validated)

	// Publish step: only when validation succeeded and at least one
	// publisher is wired in. Failures surface as a False condition with
	// the underlying error — we requeue so the next reconcile retries
	// against a possibly-now-reachable etcd.
	requeue := false
	if validated.Status == metav1.ConditionTrue &&
		(r.Publisher != nil || r.IdentityPublisher != nil) {
		// Resolve the parent cluster — already verified to exist by
		// validateTenant, but we need its spec for endpoint discovery.
		var cluster kvcachev1alpha1.KVCacheCluster
		if err := r.Get(ctx, types.NamespacedName{
			Name:      tenant.Spec.ClusterRef,
			Namespace: tenant.Namespace,
		}, &cluster); err == nil {
			if r.Publisher != nil {
				pubCond := metav1.Condition{
					Type:               publishedConditionType,
					LastTransitionTime: metav1.Now(),
				}
				if err := r.Publisher.Publish(ctx, &cluster, &tenant); err != nil {
					pubCond.Status  = metav1.ConditionFalse
					pubCond.Reason  = "EtcdPutFailed"
					pubCond.Message = err.Error()
					requeue = true
				} else {
					pubCond.Status = metav1.ConditionTrue
					pubCond.Reason = "Published"
				}
				if replaceCondition(&tenant.Status.Conditions, pubCond) {
					changed = true
				}
			}

			if r.IdentityPublisher != nil {
				idCond := metav1.Condition{
					Type:               identitiesPublishedConditionType,
					LastTransitionTime: metav1.Now(),
				}
				if err := r.IdentityPublisher.PublishIdentities(ctx, &cluster, &tenant); err != nil {
					idCond.Status  = metav1.ConditionFalse
					idCond.Reason  = "EtcdPutFailed"
					idCond.Message = err.Error()
					requeue = true
				} else {
					idCond.Status  = metav1.ConditionTrue
					idCond.Reason  = "Published"
					idCond.Message = fmt.Sprintf("%d identities", len(tenant.Spec.AllowedIdentities))
				}
				if replaceCondition(&tenant.Status.Conditions, idCond) {
					changed = true
				}
			}
		}
	}

	if !changed {
		// No status drift; nothing to write back.
		if requeue {
			return ctrl.Result{RequeueAfter: 30 * time.Second}, nil
		}
		return ctrl.Result{}, nil
	}
	if err := r.Status().Update(ctx, &tenant); err != nil {
		return ctrl.Result{}, err
	}
	if requeue {
		return ctrl.Result{RequeueAfter: 30 * time.Second}, nil
	}
	return ctrl.Result{}, nil
}

func (r *KVCacheTenantReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&kvcachev1alpha1.KVCacheTenant{}).
		Complete(r)
}

// validateTenant returns ("", "") on success; otherwise (reason, message).
func validateTenant(ctx context.Context, c client.Client,
	tenant *kvcachev1alpha1.KVCacheTenant) (string, string) {
	spec := tenant.Spec

	// 1. tenant_id: exactly 32 lower-or-upper hex chars (16 bytes).
	if len(spec.TenantID) != 32 {
		return "InvalidTenantID",
			fmt.Sprintf("tenantID must be 32 hex chars; got %d", len(spec.TenantID))
	}
	if _, err := hex.DecodeString(spec.TenantID); err != nil {
		return "InvalidTenantID", fmt.Sprintf("tenantID not hex: %v", err)
	}

	// 2. quotas parse cleanly and are non-zero.
	if _, err := resource.ParseQuantity(spec.Quota.CapacityBytes); err != nil {
		return "InvalidQuota", fmt.Sprintf("quota.capacityBytes: %v", err)
	}
	if spec.Quota.QPS == 0 {
		return "InvalidQuota", "quota.qps must be > 0"
	}
	if _, err := resource.ParseQuantity(spec.Quota.BandwidthBytesPerSecond); err != nil {
		return "InvalidQuota", fmt.Sprintf("quota.bandwidthBytesPerSecond: %v", err)
	}

	// 3. defaultPriority — empty is treated as P1 per LLD §5.1, no error.
	switch spec.DefaultPriority {
	case "", "P0", "P1", "P2":
	default:
		return "InvalidPriority",
			fmt.Sprintf("defaultPriority must be P0|P1|P2; got %q", spec.DefaultPriority)
	}

	// 4. clusterRef must point to an existing KVCacheCluster in the same
	// namespace. Cross-namespace refs are deliberately rejected so a
	// tenant can't bind to a cluster a different team owns.
	if spec.ClusterRef == "" {
		return "MissingClusterRef", "spec.clusterRef is required"
	}
	var cluster kvcachev1alpha1.KVCacheCluster
	err := c.Get(ctx, types.NamespacedName{
		Name: spec.ClusterRef, Namespace: tenant.Namespace,
	}, &cluster)
	if apierrors.IsNotFound(err) {
		return "ClusterNotFound",
			fmt.Sprintf("KVCacheCluster %q not found in namespace %q",
				spec.ClusterRef, tenant.Namespace)
	}
	if err != nil {
		return "ClusterLookupFailed", err.Error()
	}

	// 5. allowedIdentities (Phase B8.4): each entry must carry at least one
	// of spiffeID / cn (an entry with neither is unkeyable and the node
	// watcher would reject it); a SPIFFE id must be a spiffe:// URI; kind
	// is one of the three classes (empty defaults to "tenant" at publish).
	for i, id := range spec.AllowedIdentities {
		if id.SpiffeID == "" && id.CN == "" {
			return "InvalidIdentity",
				fmt.Sprintf("allowedIdentities[%d]: spiffeID or cn is required", i)
		}
		if id.SpiffeID != "" && !strings.HasPrefix(id.SpiffeID, "spiffe://") {
			return "InvalidIdentity",
				fmt.Sprintf("allowedIdentities[%d].spiffeID must be a spiffe:// URI", i)
		}
		switch id.Kind {
		case "", "tenant", "internal", "admin":
		default:
			return "InvalidIdentity",
				fmt.Sprintf("allowedIdentities[%d].kind must be tenant|internal|admin; got %q",
					i, id.Kind)
		}
	}

	return "", ""
}

// replaceCondition writes `next` into `conds`. Returns true iff the
// effective status / reason / message changed (and an update should be
// issued).
func replaceCondition(conds *[]metav1.Condition, next metav1.Condition) bool {
	for i, c := range *conds {
		if c.Type != next.Type {
			continue
		}
		if c.Status == next.Status && c.Reason == next.Reason && c.Message == next.Message {
			return false
		}
		(*conds)[i] = next
		return true
	}
	*conds = append(*conds, next)
	return true
}
