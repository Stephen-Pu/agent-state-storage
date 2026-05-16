// LLD §8.2 — KVCacheCluster controller.
//
// Reconcile responsibilities (each one declarative; controller-runtime handles
// re-queueing on drift):
//
//   1. Ensure ServiceAccount + RBAC for kvstore-node and CP.
//   2. Ensure StatefulSet/DaemonSet for kvstore-node and CP (CP optional if
//      `byoEtcd` and external CP).
//   3. Ensure ConfigMap holding cluster identity + NIXL backend selection.
//   4. Ensure Service for the gRPC endpoints.
//   5. Update status with active/joining/unreachable counts pulled from etcd.
//
// Step-8 scope: controller scaffolding compiles and registers; the reconcile
// implementation walks the desired-state tree but returns NoRequeue. Real
// resource emission lands in Step-9 once Helm chart templates stabilize the
// reference YAML.
package controller

import (
	"context"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	kvcachev1alpha1 "github.com/alluxio/kvcache/operator/api/v1alpha1"
)

// KVCacheClusterReconciler reconciles a KVCacheCluster object.
type KVCacheClusterReconciler struct {
	client.Client
	Scheme *runtime.Scheme
}

// +kubebuilder:rbac:groups=kvcache.alluxio.io,resources=kvcacheclusters,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=kvcache.alluxio.io,resources=kvcacheclusters/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=apps,resources=statefulsets;daemonsets,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups="",resources=services;configmaps;secrets;serviceaccounts,verbs=get;list;watch;create;update;patch;delete

func (r *KVCacheClusterReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	var cluster kvcachev1alpha1.KVCacheCluster
	if err := r.Get(ctx, req.NamespacedName, &cluster); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}
	logger.Info("reconciling KVCacheCluster",
		"name", cluster.Name,
		"node_replicas", cluster.Spec.NodeReplicas,
		"nixl_backend", cluster.Spec.NixlBackend)

	// TODO(stephen): ensure the dependent resources listed in the docblock.
	// For now we just acknowledge the object.
	return ctrl.Result{}, nil
}

func (r *KVCacheClusterReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&kvcachev1alpha1.KVCacheCluster{}).
		Complete(r)
}
