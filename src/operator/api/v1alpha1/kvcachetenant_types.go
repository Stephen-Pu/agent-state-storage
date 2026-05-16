// LLD §8.2 — KVCacheTenant CRD types.
package v1alpha1

import (
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// KVCacheTenantSpec declares one tenant against a KVCacheCluster.
type KVCacheTenantSpec struct {
	// Reference to the parent KVCacheCluster.
	ClusterRef string `json:"clusterRef"`

	// 16-byte tenant UUID, base16 / hex-encoded.
	// +kubebuilder:validation:Pattern=`^[0-9a-fA-F]{32}$`
	TenantID string `json:"tenantID"`

	// Three-dimensional quota (LLD §5.1).
	Quota QuotaSpec `json:"quota"`

	// Priority class for newly admitted requests when no explicit class is
	// attached. P0 = latency-critical, P1 = default, P2 = best-effort.
	// +kubebuilder:validation:Enum=P0;P1;P2
	DefaultPriority string `json:"defaultPriority,omitempty"`

	// When set, the operator will start a right-to-erase reconcile loop
	// (LLD §5.2 — 24h SLA) and refuse new reserves on this tenant.
	DeletionPending bool `json:"deletionPending,omitempty"`
}

type QuotaSpec struct {
	CapacityBytes   string `json:"capacityBytes"`            // resource.Quantity, e.g. "100Gi"
	QPS             uint32 `json:"qps"`
	BandwidthBytesPerSecond string `json:"bandwidthBytesPerSecond"`  // e.g. "10Gi"
}

type KVCacheTenantStatus struct {
	Conditions       []metav1.Condition `json:"conditions,omitempty"`
	CapacityUsedBytes string             `json:"capacityUsedBytes,omitempty"`
	QPSWindow        uint32             `json:"qpsWindow,omitempty"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
type KVCacheTenant struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   KVCacheTenantSpec   `json:"spec,omitempty"`
	Status KVCacheTenantStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true
type KVCacheTenantList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []KVCacheTenant `json:"items"`
}

func init() {
	SchemeBuilder.Register(&KVCacheTenant{}, &KVCacheTenantList{})
}
