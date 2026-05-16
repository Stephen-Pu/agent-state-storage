// LLD §8.2 — KVCacheCluster CRD types.
package v1alpha1

import (
	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// KVCacheClusterSpec declares one logical cluster.
type KVCacheClusterSpec struct {
	// Number of kvstore-node replicas (DaemonSet on labeled GPU hosts).
	// +kubebuilder:validation:Minimum=1
	NodeReplicas int32 `json:"nodeReplicas"`

	// Image used for kvstore-node and kvagent.
	Image string `json:"image"`

	// Tier capacities — apply per node.
	Tier TierSpec `json:"tier"`

	// NIXL backend selection (LLD §3.5, D-NET-1).
	// +kubebuilder:validation:Enum=ucx;gdr;gds;tcp;nvlink;loopback
	NixlBackend string `json:"nixlBackend,omitempty"`

	// Bring-your-own etcd. When false (default) the operator creates an
	// internal 3-replica etcd StatefulSet (LLD §7.2).
	ByoEtcd       bool     `json:"byoEtcd,omitempty"`
	EtcdEndpoints []string `json:"etcdEndpoints,omitempty"`

	// Alluxio binding for the T4 cold tier.
	AlluxioBinding *AlluxioBinding `json:"alluxioBinding,omitempty"`

	// Extra resource requests / limits applied to node pods.
	NodeResources corev1.ResourceRequirements `json:"nodeResources,omitempty"`
}

type TierSpec struct {
	PinnedBytes  string `json:"pinnedBytes"`            // e.g. "32Gi"
	DramBytes    string `json:"dramBytes"`              // e.g. "128Gi"
	NvmePath     string `json:"nvmePath,omitempty"`     // e.g. "/var/lib/kvcache/nvme.bin"
	NvmeBytes    string `json:"nvmeBytes,omitempty"`    // e.g. "1Ti"
	EnableCold   bool   `json:"enableCold,omitempty"`
}

type AlluxioBinding struct {
	// Mount path of the alluxio-fuse mount on each node host.
	MountPath string `json:"mountPath"`
	// Subdirectory under the mount for KV cold-tier data.
	Subdir string `json:"subdir,omitempty"`
}

// KVCacheClusterStatus is reported by the operator.
type KVCacheClusterStatus struct {
	// Conditions follow the standard meta.Condition convention.
	Conditions []metav1.Condition `json:"conditions,omitempty"`
	// Counts of nodes in each membership state.
	NodesActive      int32 `json:"nodesActive"`
	NodesJoining     int32 `json:"nodesJoining"`
	NodesUnreachable int32 `json:"nodesUnreachable"`
	NodesDraining    int32 `json:"nodesDraining"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:printcolumn:name="Node Replicas",type="integer",JSONPath=".spec.nodeReplicas"
// +kubebuilder:printcolumn:name="Active",type="integer",JSONPath=".status.nodesActive"
// +kubebuilder:printcolumn:name="Age",type="date",JSONPath=".metadata.creationTimestamp"
type KVCacheCluster struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   KVCacheClusterSpec   `json:"spec,omitempty"`
	Status KVCacheClusterStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true
type KVCacheClusterList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []KVCacheCluster `json:"items"`
}

func init() {
	SchemeBuilder.Register(&KVCacheCluster{}, &KVCacheClusterList{})
}
