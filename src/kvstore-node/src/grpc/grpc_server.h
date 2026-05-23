// Phase M-1 / N-1 — thin RAII wrapper around grpc::Server for the
// kvstore-node binary. Hides the grpc++ headers from main.cpp /
// NodeRuntime so they don't have to pull the dep transitively.
//
// Phase N-1 added mTLS — when the caller hands in a (ca, cert, key)
// trio the server uses `grpc::SslServerCredentials` with
// REQUEST_AND_REQUIRE_CLIENT_CERT_AND_VERIFY (i.e. mutual TLS, not
// just server-auth). With paths empty the server falls back to
// `grpc::InsecureServerCredentials` — the dev / unit-test default.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

namespace kvcache::node::grpc_server {

class GrpcServer {
   public:
    struct Options {
        std::string bind_host = "0.0.0.0";
        uint16_t    port      = 7000;

        // mTLS material on disk. Provide ALL THREE to enable mTLS;
        // leaving any one empty keeps the listener insecure. The
        // operator-emitted StatefulSet mounts these from a Secret at
        // /etc/kvcache/tls/{ca.crt,tls.crt,tls.key}.
        std::string tls_ca_pem_path;
        std::string tls_cert_pem_path;
        std::string tls_key_pem_path;
    };

    // Construct + bind + start serving. On success Ok() returns true
    // and BoundPort() reports the actually-bound port (resolves port=0).
    // On TLS misconfig (paths set but unreadable / malformed) Ok()
    // returns false and error() carries the diagnostic.
    // The service ptr is borrowed, not owned — the caller (main.cpp /
    // tests) keeps it alive for the server's lifetime.
    GrpcServer(const Options& opts, ::grpc::Service* service);
    ~GrpcServer();

    GrpcServer(const GrpcServer&)            = delete;
    GrpcServer& operator=(const GrpcServer&) = delete;

    bool                Ok()         const noexcept { return server_ != nullptr; }
    uint16_t            BoundPort()  const noexcept { return bound_port_; }
    bool                TlsEnabled() const noexcept { return tls_enabled_; }
    const std::string&  error()      const noexcept { return error_; }

    // Stops the server, draining in-flight RPCs first. Idempotent.
    void Stop();

   private:
    std::unique_ptr<::grpc::Server> server_;
    uint16_t                        bound_port_ = 0;
    bool                            tls_enabled_ = false;
    std::string                     error_;
};

}  // namespace kvcache::node::grpc_server
