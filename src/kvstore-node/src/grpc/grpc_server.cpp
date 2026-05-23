// Phase M-1 / N-1 — grpc::Server wrapper. See grpc_server.h.
#include "grpc/grpc_server.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/security/server_credentials.h>

#include <fstream>
#include <sstream>
#include <string>

namespace kvcache::node::grpc_server {

namespace {

// Slurp a whole PEM file into memory. Returns empty string + sets err
// when the file is missing / unreadable, so the caller can distinguish
// "not configured" (empty path) from "configured but broken".
std::string ReadFile(const std::string& path, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "tls: cannot open " + path;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// True iff all three TLS paths are set. We require the full trio so a
// half-configured server doesn't silently fall back to insecure.
bool AllTlsPathsPresent(const GrpcServer::Options& o) {
    return !o.tls_ca_pem_path.empty()   &&
           !o.tls_cert_pem_path.empty() &&
           !o.tls_key_pem_path.empty();
}

std::shared_ptr<::grpc::ServerCredentials>
BuildCredentials(const GrpcServer::Options& opts, std::string* err) {
    if (!AllTlsPathsPresent(opts)) {
        return ::grpc::InsecureServerCredentials();
    }
    std::string ca   = ReadFile(opts.tls_ca_pem_path,   err);
    if (!err->empty()) return nullptr;
    std::string cert = ReadFile(opts.tls_cert_pem_path, err);
    if (!err->empty()) return nullptr;
    std::string key  = ReadFile(opts.tls_key_pem_path,  err);
    if (!err->empty()) return nullptr;

    ::grpc::SslServerCredentialsOptions ssl_opts(
        // Require AND verify the client cert. The operator emits one
        // CA + leaf set for the whole cluster, so node, agent, and CP
        // all trust the same root and present client certs signed by
        // it. Anything else lands as UNAUTHENTICATED on the wire.
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
    ssl_opts.pem_root_certs = std::move(ca);
    ::grpc::SslServerCredentialsOptions::PemKeyCertPair pair;
    pair.private_key = std::move(key);
    pair.cert_chain  = std::move(cert);
    ssl_opts.pem_key_cert_pairs.push_back(std::move(pair));
    return ::grpc::SslServerCredentials(ssl_opts);
}

}  // namespace

GrpcServer::GrpcServer(const Options& opts, ::grpc::Service* service) {
    auto creds = BuildCredentials(opts, &error_);
    if (!creds) {
        return;  // error_ already set by BuildCredentials
    }
    tls_enabled_ = AllTlsPathsPresent(opts);

    ::grpc::ServerBuilder builder;
    const std::string addr = opts.bind_host + ":" + std::to_string(opts.port);
    int selected_port = 0;
    // AddListeningPort writes the actually-bound port through the
    // optional out-param. For port=0 this is how we learn the OS-picked
    // value.
    builder.AddListeningPort(addr, std::move(creds), &selected_port);
    builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (!server_) {
        error_ = "grpc::ServerBuilder::BuildAndStart returned null";
        return;
    }
    if (selected_port <= 0) {
        error_ = "grpc::ServerBuilder failed to bind " + addr;
        server_.reset();
        return;
    }
    bound_port_ = static_cast<uint16_t>(selected_port);
}

GrpcServer::~GrpcServer() { Stop(); }

void GrpcServer::Stop() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
    }
}

}  // namespace kvcache::node::grpc_server
