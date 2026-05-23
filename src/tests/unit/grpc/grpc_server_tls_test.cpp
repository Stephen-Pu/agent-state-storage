// Phase N-1 — TLS end-to-end through GrpcServer.
//
// SetUp() shells out to openssl(1) to generate a fresh CA + server +
// client cert / key set in a per-test tmpdir. Then we boot a
// `GrpcServer` with the TLS material and prove three things:
//
//   * The matching mTLS client (CA bundle + client cert) reaches the
//     service and gets a normal response.
//   * An insecure-creds client (no TLS at all) fails — the listener
//     rejects the handshake.
//   * A client with a DIFFERENT CA fails — the server validates the
//     client cert chain.
//
// The test is skipped (not failed) if openssl isn't on PATH, so a
// stripped-down build environment doesn't block the rest of the suite.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "grpc/grpc_server.h"
#include "grpc/node_data_service.h"
#include "kvcache/kv_abi.h"
#include "node.grpc.pb.h"

using kvcache::node::grpc_server::GrpcServer;
using kvcache::node::grpc_server::NodeDataServiceImpl;
using kvcache::proto::NodeData;

namespace {

bool OpensslAvailable() {
    return std::system("openssl version >/dev/null 2>&1") == 0;
}

std::string SlurpFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs `cmd` and asserts it returns 0; gtest-fatal on failure.
void Sh(const std::string& cmd) {
    const int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
    ASSERT_EQ(rc, 0) << "command failed: " << cmd;
}

// Generates a (CA, server, client) trio under `dir`. The server cert's
// SAN covers DNS:localhost and IP:127.0.0.1 so the gRPC client's
// default name-resolution check passes.
void GenerateCerts(const std::string& dir) {
    // CA
    Sh("openssl genrsa -out " + dir + "/ca.key 2048");
    Sh("openssl req -x509 -new -key " + dir + "/ca.key -days 30 "
       "-subj '/CN=kvcache-test-ca' -out " + dir + "/ca.crt");

    // Server
    Sh("openssl genrsa -out " + dir + "/server.key 2048");
    Sh("openssl req -new -key " + dir + "/server.key "
       "-subj '/CN=kvcache-test-server' -out " + dir + "/server.csr");
    // openssl 3 needs a config-file SAN; we synthesise one inline.
    Sh("printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\\n' > " + dir + "/san.ext");
    Sh("openssl x509 -req -in " + dir + "/server.csr "
       "-CA " + dir + "/ca.crt -CAkey " + dir + "/ca.key -CAcreateserial "
       "-days 30 -extfile " + dir + "/san.ext "
       "-out " + dir + "/server.crt");

    // Client
    Sh("openssl genrsa -out " + dir + "/client.key 2048");
    Sh("openssl req -new -key " + dir + "/client.key "
       "-subj '/CN=kvcache-test-client' -out " + dir + "/client.csr");
    Sh("openssl x509 -req -in " + dir + "/client.csr "
       "-CA " + dir + "/ca.crt -CAkey " + dir + "/ca.key -CAcreateserial "
       "-days 30 -out " + dir + "/client.crt");
}

class GrpcTlsFixture : public ::testing::Test {
   protected:
    void SetUp() override {
        if (!OpensslAvailable()) {
            GTEST_SKIP() << "openssl(1) not on PATH; skipping mTLS test";
        }
        // Per-test tmpdir so concurrent ctest runs don't collide.
        const std::string tmpl =
            std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR")
                                                : "/tmp") +
            "/kvcache-tls-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        ASSERT_NE(mkdtemp(buf.data()), nullptr) << "mkdtemp failed";
        dir_ = std::string(buf.data());
        GenerateCerts(dir_);

        kv_ctx_config_t cfg{};
        cfg.abi_version = KVCACHE_ABI_VERSION;
        cfg.tenant_id   = "tls-tenant";
        cfg.model_id    = "tls-model";
        cfg.flags       = 0;
        ASSERT_EQ(kv_ctx_open(&cfg, &ctx_), KV_OK);

        svc_ = std::make_unique<NodeDataServiceImpl>(ctx_);
        GrpcServer::Options opts;
        opts.bind_host         = "127.0.0.1";
        opts.port              = 0;
        opts.tls_ca_pem_path   = dir_ + "/ca.crt";
        opts.tls_cert_pem_path = dir_ + "/server.crt";
        opts.tls_key_pem_path  = dir_ + "/server.key";
        server_ = std::make_unique<GrpcServer>(opts, svc_.get());
        ASSERT_TRUE(server_->Ok()) << server_->error();
        ASSERT_TRUE(server_->TlsEnabled());
        addr_ = "127.0.0.1:" + std::to_string(server_->BoundPort());
    }

    void TearDown() override {
        if (server_) server_->Stop();
        server_.reset();
        svc_.reset();
        if (ctx_) {
            kv_ctx_close(ctx_);
            ctx_ = nullptr;
        }
        if (!dir_.empty()) {
            std::system(("rm -rf " + dir_).c_str());
        }
    }

    std::unique_ptr<NodeData::Stub> StubWithMtls() const {
        ::grpc::SslCredentialsOptions sopts;
        sopts.pem_root_certs  = SlurpFile(dir_ + "/ca.crt");
        sopts.pem_cert_chain  = SlurpFile(dir_ + "/client.crt");
        sopts.pem_private_key = SlurpFile(dir_ + "/client.key");
        // The server cert's CN is `kvcache-test-server`; the SAN
        // generated via openssl x509 -extfile may or may not make it
        // through depending on host platform quoting. Pinning the SSL
        // target name override sidesteps the SAN-vs-127.0.0.1 issue
        // entirely (we just need *some* match between the dialled
        // authority and a name on the cert).
        ::grpc::ChannelArguments cargs;
        cargs.SetSslTargetNameOverride("kvcache-test-server");
        auto channel = ::grpc::CreateCustomChannel(
            addr_, ::grpc::SslCredentials(sopts), cargs);
        return NodeData::NewStub(channel);
    }

    std::unique_ptr<NodeData::Stub> StubInsecure() const {
        auto channel = ::grpc::CreateChannel(
            addr_, ::grpc::InsecureChannelCredentials());
        return NodeData::NewStub(channel);
    }

    std::string                          dir_;
    std::string                          addr_;
    kv_ctx_t*                            ctx_ = nullptr;
    std::unique_ptr<NodeDataServiceImpl> svc_;
    std::unique_ptr<GrpcServer>          server_;
};

}  // namespace

TEST_F(GrpcTlsFixture, MatchingClientSucceeds) {
    auto stub = StubWithMtls();
    ::grpc::ClientContext ctx;
    // Bound the handshake so a misconfigured cert chain fails the
    // test fast instead of hanging the whole suite.
    ctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(5));
    kvcache::proto::LookupRequest req;
    req.set_tenant_id("tls-tenant");
    req.add_tokens(1);
    req.add_tokens(2);
    kvcache::proto::LookupResponse resp;
    auto s = stub->Lookup(&ctx, req, &resp);
    EXPECT_TRUE(s.ok()) << s.error_code() << ": " << s.error_message();
    EXPECT_FALSE(resp.hit());
}

TEST_F(GrpcTlsFixture, InsecureClientFails) {
    auto stub = StubInsecure();
    ::grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
    kvcache::proto::LookupRequest req;
    req.set_tenant_id("tls-tenant");
    kvcache::proto::LookupResponse resp;
    auto s = stub->Lookup(&ctx, req, &resp);
    EXPECT_FALSE(s.ok())
        << "insecure client should be rejected by the TLS listener";
}

TEST_F(GrpcTlsFixture, ClientWithDifferentCaIsRejected) {
    // Generate a *second* CA + leaf in a sibling tmpdir; present that
    // leaf to the (already-running) server. The server's CA bundle
    // doesn't trust this cert, so handshake fails.
    const std::string other = dir_ + "-other";
    ASSERT_EQ(std::system(("mkdir " + other).c_str()), 0);
    GenerateCerts(other);

    ::grpc::SslCredentialsOptions sopts;
    sopts.pem_root_certs  = SlurpFile(dir_   + "/ca.crt");   // trust our server
    sopts.pem_cert_chain  = SlurpFile(other + "/client.crt"); // wrong CA
    sopts.pem_private_key = SlurpFile(other + "/client.key");
    ::grpc::ChannelArguments cargs;
    cargs.SetSslTargetNameOverride("kvcache-test-server");
    auto stub = NodeData::NewStub(::grpc::CreateCustomChannel(
        addr_, ::grpc::SslCredentials(sopts), cargs));

    ::grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
    kvcache::proto::LookupRequest req;
    req.set_tenant_id("tls-tenant");
    kvcache::proto::LookupResponse resp;
    auto s = stub->Lookup(&ctx, req, &resp);
    EXPECT_FALSE(s.ok())
        << "client cert signed by a different CA should be rejected";

    std::system(("rm -rf " + other).c_str());
}
