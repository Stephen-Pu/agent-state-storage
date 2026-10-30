# Security

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub: **Security → Report a vulnerability** on this repository. Do not open a public issue.

We aim to acknowledge a report within 3 working days and to agree on a fix and disclosure date with you.

## What the code does today

This table describes the current code, including the gaps. It is updated with the code.

| Area | Today | Where |
|---|---|---|
| Node gRPC transport | **mTLS when TLS material is configured.** The Helm chart and the operator turn it on by default (cert-manager issues the certificates). | `kvstore-node/src/grpc/grpc_server.*`, `deploy/helm/kvcache/values.yaml` (`mtls.enabled: true`) |
| Session API tenant (`agentstate.v1`) | With mTLS the tenant is resolved **from the client certificate**; a request naming another tenant gets `PERMISSION_DENIED`. Without TLS the request's tenant field is trusted (local development only). Every stored key starts with the tenant. | `kvstore-node/src/session/agent_state_service.*`, `main.cpp` (`EnableCertTenant`) |
| Tenant identity (KV `NodeData`) | The code can resolve the tenant **from the client certificate** and reject a request that names another tenant, but the node binary does **not** turn this on yet (see Known gaps). | `kvstore-node/src/security/mtls.*`, `grpc/node_data_service.cpp` (`EnableTenantCertBinding`) |
| Cross-tenant isolation of cached state | Every state identity includes the tenant, so one tenant cannot look up or probe another tenant's prefixes. Quotas are per tenant. | `core/common/state_identity.h`, `core/common/locator.h` |
| Encryption at rest | AES-256-GCM, authenticated, for the cold tier (`EncryptingColdTier`). Not yet for the DRAM or NVMe tiers. | `kvstore-node/src/tier/encrypting_cold_tier.*` |
| Deserialization | No Python `pickle` or other object deserialization on any wire path. The wire formats are protobuf / gRPC and the C ABI. | — |
| Regulated mode | Optional fail-closed startup gate: refuses to start unless every configured audit sink and requirement check passes. | `kvstore-node/src/security/regulated_mode.*`, `audit.*` |

## Known gaps

- ⚠️ **Tenant-to-certificate binding is off in the node binary.** With mTLS on, any client holding a certificate from the cluster CA can name any tenant in its requests. The check exists (`NodeDataServiceImpl::EnableTenantCertBinding`) but `kvstore-node` never enables it.
- ⚠️ **A node started without TLS material listens in plaintext on `0.0.0.0`.** That is meant for local development only. In this mode the tenant is whatever the client claims. Always configure mTLS outside a single machine.
- ⚠️ **The in-cluster etcd still serves clients over plain HTTP.** TLS material is mounted, but the client listener switches to HTTPS only once every caller speaks TLS (operator phase H-4).
- The DRAM and NVMe tiers are not encrypted at rest.
