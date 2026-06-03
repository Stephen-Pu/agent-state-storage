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

	// AllowedIdentities is the mTLS identity allow-list for this tenant
	// (Phase B8.4). The operator publishes one entry per identity to
	// etcd under /kvcache/identities/<cluster>/<tenantID>/<id>, where the
	// node-side IdentityWatcher (B8.3) loads them into the MtlsRegistry so
	// the B8.2 tenant-cert binding can resolve a client cert → this tenant.
	// Each entry must carry at least one of spiffeID / cn.
	AllowedIdentities []TenantIdentity `json:"allowedIdentities,omitempty"`
}

// TenantIdentity is one mTLS principal allowed to act as this tenant.
type TenantIdentity struct {
	// SPIFFE ID bound into the client cert's URI SAN, e.g.
	// "spiffe://example.org/tenant/acme". Preferred over CN.
	// +kubebuilder:validation:Pattern=`^spiffe://.+`
	SpiffeID string `json:"spiffeID,omitempty"`

	// Certificate Common Name, used when no SPIFFE ID is issued.
	CN string `json:"cn,omitempty"`

	// Identity class consumed by node-side authz. Empty defaults to
	// "tenant" at publish time.
	// +kubebuilder:validation:Enum=tenant;internal;admin
	Kind string `json:"kind,omitempty"`
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
