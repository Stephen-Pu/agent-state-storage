// M1 — agentstate.v1 service over a real in-process gRPC server.
#include "session/agent_state_service.h"

#include <gtest/gtest.h>
#include <openssl/crypto.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "grpc/grpc_server.h"
#include "grpc/node_data_service.h"
#include "kvcache/kv_abi.h"
#include "node.grpc.pb.h"

namespace asv1 = ::agentstate::v1;
using kvcache::node::grpc_server::GrpcServer;
using kvcache::node::session::AgentStateServiceImpl;
using kvcache::node::session::BlobStore;
using kvcache::node::session::OrderedLogStore;

namespace {

// gRPC's background threads can still be exiting (and running OpenSSL's
// per-thread cleanup) when exit() runs OpenSSL's global atexit cleanup; the
// two race and the process can crash after every test has passed. Skip the
// global cleanup: the process is ending and the OS reclaims the memory.
const bool kNoOpensslAtexit = OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT, nullptr) == 1;

class AgentStateServiceTest : public ::testing::Test {
   protected:
    void SetUp() override {
        std::string err;
        store_ = OrderedLogStore::Open("", &err);
        blobs_ = BlobStore::Open("", &err);
        ASSERT_TRUE(store_ && blobs_) << err;
        svc_ = std::make_unique<AgentStateServiceImpl>(store_.get(), blobs_.get());
        ::grpc::ServerBuilder b;
        int port = 0;
        b.AddListeningPort("127.0.0.1:0", ::grpc::InsecureServerCredentials(), &port);
        b.RegisterService(svc_.get());
        server_ = b.BuildAndStart();
        ASSERT_TRUE(server_);
        stub_ = asv1::AgentState::NewStub(::grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), ::grpc::InsecureChannelCredentials()));
    }
    void TearDown() override {
        if (server_) server_->Shutdown();
    }

    ::grpc::Status Append(const std::string& tenant, const std::string& session,
                          std::vector<std::string> items, uint64_t* next = nullptr) {
        ::grpc::ClientContext ctx;
        asv1::AppendItemsRequest req;
        req.set_tenant(tenant);
        req.set_session(session);
        for (auto& i : items) req.add_items(i);
        asv1::AppendItemsResponse resp;
        auto st = stub_->AppendItems(&ctx, req, &resp);
        if (next) *next = resp.next_seq();
        return st;
    }
    std::vector<std::string> Items(const std::string& tenant, const std::string& session,
                                   uint32_t limit = 0) {
        ::grpc::ClientContext ctx;
        asv1::GetItemsRequest req;
        req.set_tenant(tenant);
        req.set_session(session);
        req.set_limit(limit);
        asv1::GetItemsResponse resp;
        EXPECT_TRUE(stub_->GetItems(&ctx, req, &resp).ok());
        return {resp.items().begin(), resp.items().end()};
    }
    ::grpc::Status Write(const std::string& tenant,
                         std::vector<std::pair<std::string, std::string>> puts,
                         std::vector<std::string> deletes = {}) {
        ::grpc::ClientContext ctx;
        asv1::WriteRecordsRequest req;
        req.set_tenant(tenant);
        for (auto& [k, v] : puts) {
            auto* r = req.add_puts();
            r->set_key(k);
            r->set_value(v);
        }
        for (auto& d : deletes) req.add_deletes(d);
        asv1::WriteRecordsResponse resp;
        return stub_->WriteRecords(&ctx, req, &resp);
    }
    asv1::ScanRecordsResponse Scan(const std::string& tenant, const std::string& prefix,
                                 uint32_t limit = 0, bool reverse = false,
                                 const std::string& start_after = "") {
        ::grpc::ClientContext ctx;
        asv1::ScanRecordsRequest req;
        req.set_tenant(tenant);
        req.set_prefix(prefix);
        req.set_limit(limit);
        req.set_reverse(reverse);
        req.set_start_after(start_after);
        asv1::ScanRecordsResponse resp;
        EXPECT_TRUE(stub_->ScanRecords(&ctx, req, &resp).ok());
        return resp;
    }
    static std::vector<std::string> Keys(const asv1::ScanRecordsResponse& r) {
        std::vector<std::string> out;
        for (const auto& rec : r.records()) out.push_back(rec.key());
        return out;
    }

    std::unique_ptr<OrderedLogStore>             store_;
    std::unique_ptr<BlobStore>                   blobs_;
    std::unique_ptr<AgentStateServiceImpl>       svc_;
    std::unique_ptr<::grpc::Server>              server_;
    std::unique_ptr<asv1::AgentState::Stub>        stub_;
};

}  // namespace

TEST_F(AgentStateServiceTest, AppendGetPopClear) {
    uint64_t next = 0;
    ASSERT_TRUE(Append("t", "s1", {"a", "b"}, &next).ok());
    EXPECT_EQ(next, 3u);
    ASSERT_TRUE(Append("t", "s1", {"c"}, &next).ok());
    EXPECT_EQ(next, 4u);
    EXPECT_EQ(Items("t", "s1"), (std::vector<std::string>{"a", "b", "c"}));

    {
        ::grpc::ClientContext ctx;
        asv1::PopItemRequest req;
        req.set_tenant("t");
        req.set_session("s1");
        asv1::PopItemResponse resp;
        ASSERT_TRUE(stub_->PopItem(&ctx, req, &resp).ok());
        EXPECT_TRUE(resp.found());
        EXPECT_EQ(resp.item(), "c");
    }
    EXPECT_EQ(Items("t", "s1"), (std::vector<std::string>{"a", "b"}));
    {
        ::grpc::ClientContext ctx;
        asv1::ClearSessionRequest req;
        req.set_tenant("t");
        req.set_session("s1");
        asv1::ClearSessionResponse resp;
        ASSERT_TRUE(stub_->ClearSession(&ctx, req, &resp).ok());
        EXPECT_EQ(resp.removed(), 2u);
    }
    EXPECT_TRUE(Items("t", "s1").empty());
    {
        ::grpc::ClientContext ctx;
        asv1::PopItemRequest req;
        req.set_tenant("t");
        req.set_session("s1");
        asv1::PopItemResponse resp;
        ASSERT_TRUE(stub_->PopItem(&ctx, req, &resp).ok());
        EXPECT_FALSE(resp.found());
    }
}

TEST_F(AgentStateServiceTest, GetItemsLimitReturnsLatestInOrder) {
    ASSERT_TRUE(Append("t", "s", {"1", "2", "3", "4", "5"}).ok());
    EXPECT_EQ(Items("t", "s", 2), (std::vector<std::string>{"4", "5"}));
    EXPECT_EQ(Items("t", "s", 9), (std::vector<std::string>{"1", "2", "3", "4", "5"}));
}

TEST_F(AgentStateServiceTest, SessionNamesThatArePrefixesDoNotMix) {
    ASSERT_TRUE(Append("t", "s", {"short"}).ok());
    ASSERT_TRUE(Append("t", "s1", {"long"}).ok());
    EXPECT_EQ(Items("t", "s"), (std::vector<std::string>{"short"}));
    EXPECT_EQ(Items("t", "s1"), (std::vector<std::string>{"long"}));
}

TEST_F(AgentStateServiceTest, ForkCopiesAndRefusesNonEmptyTarget) {
    ASSERT_TRUE(Append("t", "src", {"a", "b"}).ok());
    {
        ::grpc::ClientContext ctx;
        asv1::ForkSessionRequest req;
        req.set_tenant("t");
        req.set_source("src");
        req.set_target("dst");
        asv1::ForkSessionResponse resp;
        ASSERT_TRUE(stub_->ForkSession(&ctx, req, &resp).ok());
        EXPECT_EQ(resp.copied(), 2u);
    }
    ASSERT_TRUE(Append("t", "dst", {"c"}).ok());
    EXPECT_EQ(Items("t", "src"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(Items("t", "dst"), (std::vector<std::string>{"a", "b", "c"}));
    {
        ::grpc::ClientContext ctx;
        asv1::ForkSessionRequest req;
        req.set_tenant("t");
        req.set_source("src");
        req.set_target("dst");
        asv1::ForkSessionResponse resp;
        EXPECT_EQ(stub_->ForkSession(&ctx, req, &resp).error_code(),
                  ::grpc::StatusCode::FAILED_PRECONDITION);
    }
}

TEST_F(AgentStateServiceTest, RecordsBatchScanAndDeletePrefix) {
    ASSERT_TRUE(Write("t", {{"cp/1", "a"}, {"cp/2", "b"}, {"cp/3", "c"}, {"w/1", "d"}}).ok());
    ASSERT_TRUE(Write("t", {{"cp/4", "e"}}, {"cp/1"}).ok());

    auto all = Scan("t", "cp/");
    EXPECT_EQ(Keys(all), (std::vector<std::string>{"cp/2", "cp/3", "cp/4"}));
    EXPECT_FALSE(all.has_more());

    auto page = Scan("t", "cp/", 2, /*reverse=*/true);
    EXPECT_EQ(Keys(page), (std::vector<std::string>{"cp/4", "cp/3"}));
    EXPECT_TRUE(page.has_more());

    EXPECT_EQ(Keys(Scan("t", "cp/", 0, false, "cp/2")), (std::vector<std::string>{"cp/3", "cp/4"}));

    {
        ::grpc::ClientContext ctx;
        asv1::GetRecordsRequest req;
        req.set_tenant("t");
        req.add_keys("cp/3");
        req.add_keys("cp/1");
        asv1::GetRecordsResponse resp;
        ASSERT_TRUE(stub_->GetRecords(&ctx, req, &resp).ok());
        ASSERT_EQ(resp.found_size(), 1);
        EXPECT_EQ(resp.found(0).key(), "cp/3");
        EXPECT_EQ(resp.found(0).value(), "c");
    }
    {
        ::grpc::ClientContext ctx;
        asv1::DeleteRecordPrefixRequest req;
        req.set_tenant("t");
        req.set_prefix("cp/");
        asv1::DeleteRecordPrefixResponse resp;
        ASSERT_TRUE(stub_->DeleteRecordPrefix(&ctx, req, &resp).ok());
        EXPECT_EQ(resp.removed(), 3u);
    }
    EXPECT_EQ(Keys(Scan("t", "")), (std::vector<std::string>{"w/1"}));
}

TEST_F(AgentStateServiceTest, ScanIsCappedPerPage) {
    std::vector<std::pair<std::string, std::string>> puts;
    for (int i = 0; i < 1200; ++i) {
        char k[16];
        std::snprintf(k, sizeof(k), "k%05d", i);
        puts.emplace_back(k, "");
    }
    ASSERT_TRUE(Write("t", puts).ok());
    auto first = Scan("t", "k");
    EXPECT_EQ(first.records_size(), 1000);
    EXPECT_TRUE(first.has_more());
    auto rest = Scan("t", "k", 0, false, first.records(999).key());
    EXPECT_EQ(rest.records_size(), 200);
    EXPECT_FALSE(rest.has_more());
}

TEST_F(AgentStateServiceTest, BlobStreamRoundTrip) {
    std::string data(3u << 20, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7);
    {
        ::grpc::ClientContext ctx;
        asv1::PutBlobResponse resp;
        auto w = stub_->PutBlob(&ctx, &resp);
        for (std::size_t off = 0; off < data.size(); off += (1u << 20)) {
            asv1::PutBlobChunk c;
            if (off == 0) {
                c.set_tenant("t");
                c.set_name("snap/1.tar");
            }
            c.set_data(data.substr(off, 1u << 20));
            ASSERT_TRUE(w->Write(c));
        }
        w->WritesDone();
        ASSERT_TRUE(w->Finish().ok());
        EXPECT_EQ(resp.size(), data.size());
    }
    {
        ::grpc::ClientContext ctx;
        asv1::GetBlobRequest req;
        req.set_tenant("t");
        req.set_name("snap/1.tar");
        auto r = stub_->GetBlob(&ctx, req);
        std::string got;
        asv1::BlobChunk c;
        while (r->Read(&c)) got += c.data();
        ASSERT_TRUE(r->Finish().ok());
        EXPECT_EQ(got, data);
    }
    {
        ::grpc::ClientContext ctx;
        asv1::StatBlobRequest req;
        req.set_tenant("t");
        req.set_name("snap/1.tar");
        asv1::StatBlobResponse resp;
        ASSERT_TRUE(stub_->StatBlob(&ctx, req, &resp).ok());
        EXPECT_TRUE(resp.exists());
        EXPECT_EQ(resp.size(), data.size());
    }
    {
        ::grpc::ClientContext ctx;
        asv1::DeleteBlobRequest req;
        req.set_tenant("t");
        req.set_name("snap/1.tar");
        asv1::DeleteBlobResponse resp;
        ASSERT_TRUE(stub_->DeleteBlob(&ctx, req, &resp).ok());
        EXPECT_TRUE(resp.existed());
    }
    {
        ::grpc::ClientContext ctx;
        asv1::GetBlobRequest req;
        req.set_tenant("t");
        req.set_name("snap/1.tar");
        auto r = stub_->GetBlob(&ctx, req);
        asv1::BlobChunk c;
        while (r->Read(&c)) {
        }
        EXPECT_EQ(r->Finish().error_code(), ::grpc::StatusCode::NOT_FOUND);
    }
}

TEST_F(AgentStateServiceTest, MissingTenantIsInvalidArgument) {
    EXPECT_EQ(Append("", "s", {"x"}).error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(Write("", {{"k", "v"}}).error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(Append("t", "", {"x"}).error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(AgentStateServiceTest, TenantsAreIsolated) {
    ASSERT_TRUE(Append("alice", "s", {"a-secret"}).ok());
    ASSERT_TRUE(Append("bob", "s", {"b"}).ok());
    ASSERT_TRUE(Write("alice", {{"key", "alice-value"}}).ok());
    EXPECT_EQ(Items("bob", "s"), (std::vector<std::string>{"b"}));
    EXPECT_TRUE(Keys(Scan("bob", "")).empty());
    // A tenant whose name extends another's must not see its records either.
    EXPECT_TRUE(Keys(Scan("alic", "")).empty());
    EXPECT_TRUE(Keys(Scan("alicee", "")).empty());
    ::grpc::ClientContext ctx;
    asv1::GetRecordsRequest req;
    req.set_tenant("bob");
    req.add_keys("key");
    asv1::GetRecordsResponse resp;
    ASSERT_TRUE(stub_->GetRecords(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.found_size(), 0);
}

TEST_F(AgentStateServiceTest, ConcurrentAppendsKeepEveryItem) {
    constexpr int kThreads = 8, kPer = 200;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                ASSERT_TRUE(
                    Append("t", "shared", {std::to_string(t) + ":" + std::to_string(i)}).ok());
            }
        });
    }
    for (auto& th : ts) th.join();
    const auto items = Items("t", "shared");
    ASSERT_EQ(items.size(), static_cast<std::size_t>(kThreads * kPer));
    std::set<std::string> uniq(items.begin(), items.end());
    EXPECT_EQ(uniq.size(), items.size());
    // Each thread's items appear in the order it appended them.
    std::vector<int> last(kThreads, -1);
    for (const auto& it : items) {
        const int t = std::stoi(it.substr(0, it.find(':')));
        const int i = std::stoi(it.substr(it.find(':') + 1));
        EXPECT_GT(i, last[t]);
        last[t] = i;
    }
}

TEST_F(AgentStateServiceTest, OversizeValueIsRejected) {
    // Just over the per-value limit but under gRPC's default receive limit
    // would be ideal; the limit and gRPC's default are both 4 MiB, so accept
    // either rejection path.
    const std::string big(AgentStateServiceImpl::kMaxValueBytes + 1, 'x');
    const auto code = Write("t", {{"k", big}}).error_code();
    EXPECT_TRUE(code == ::grpc::StatusCode::INVALID_ARGUMENT ||
                code == ::grpc::StatusCode::RESOURCE_EXHAUSTED)
        << code;
    EXPECT_TRUE(Keys(Scan("t", "")).empty());
}

// ---- TLS: tenant comes from the client certificate ------------------------

namespace {

bool OpensslAvailable() { return std::system("openssl version >/dev/null 2>&1") == 0; }

std::string Slurp(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool Sh(const std::string& cmd) { return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0; }

}  // namespace

TEST(AgentStateServiceTls, TlsTenantComesFromCertificate) {
    if (!OpensslAvailable()) GTEST_SKIP() << "openssl(1) not on PATH";
    const std::string tmpl =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/agentstate-tls-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    ASSERT_NE(mkdtemp(buf.data()), nullptr);
    const std::string d(buf.data());
    ASSERT_TRUE(Sh("openssl genrsa -out " + d + "/ca.key 2048"));
    ASSERT_TRUE(Sh("openssl req -x509 -new -key " + d + "/ca.key -days 30 -subj '/CN=as-ca' -out " +
                   d + "/ca.crt"));
    ASSERT_TRUE(Sh("openssl genrsa -out " + d + "/server.key 2048"));
    ASSERT_TRUE(Sh("openssl req -new -key " + d + "/server.key -subj '/CN=as-server' -out " + d +
                   "/server.csr"));
    ASSERT_TRUE(Sh("printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\\n' > " + d + "/san.ext"));
    ASSERT_TRUE(Sh("openssl x509 -req -in " + d + "/server.csr -CA " + d + "/ca.crt -CAkey " + d +
                   "/ca.key -CAcreateserial -days 30 -extfile " + d + "/san.ext -out " + d +
                   "/server.crt"));
    ASSERT_TRUE(Sh("openssl genrsa -out " + d + "/client.key 2048"));
    ASSERT_TRUE(Sh("openssl req -new -key " + d + "/client.key -subj '/CN=acme' -out " + d +
                   "/client.csr"));
    ASSERT_TRUE(Sh("openssl x509 -req -in " + d + "/client.csr -CA " + d + "/ca.crt -CAkey " + d +
                   "/ca.key -CAcreateserial -days 30 -out " + d + "/client.crt"));

    std::string err;
    auto store = OrderedLogStore::Open("", &err);
    auto blobs = BlobStore::Open("", &err);
    AgentStateServiceImpl svc(store.get(), blobs.get());
    svc.EnableCertTenant(nullptr);
    GrpcServer::Options o;
    o.bind_host         = "127.0.0.1";
    o.port              = 0;
    o.tls_ca_pem_path   = d + "/ca.crt";
    o.tls_cert_pem_path = d + "/server.crt";
    o.tls_key_pem_path  = d + "/server.key";
    GrpcServer server(o, &svc);
    ASSERT_TRUE(server.Ok()) << server.error();

    ::grpc::SslCredentialsOptions so;
    so.pem_root_certs  = Slurp(d + "/ca.crt");
    so.pem_cert_chain  = Slurp(d + "/client.crt");
    so.pem_private_key = Slurp(d + "/client.key");
    ::grpc::ChannelArguments args;
    // Match the server CN; the SAN from -extfile does not survive every host's quoting.
    args.SetSslTargetNameOverride("as-server");
    auto stub = asv1::AgentState::NewStub(::grpc::CreateCustomChannel(
        "127.0.0.1:" + std::to_string(server.BoundPort()), ::grpc::SslCredentials(so), args));

    auto append = [&](const std::string& tenant) {
        ::grpc::ClientContext ctx;
        asv1::AppendItemsRequest req;
        req.set_tenant(tenant);
        req.set_session("s");
        req.add_items("x");
        asv1::AppendItemsResponse resp;
        return stub->AppendItems(&ctx, req, &resp);
    };
    EXPECT_TRUE(append("acme").ok());
    EXPECT_TRUE(append("").ok());  // empty = the certificate's tenant
    EXPECT_EQ(append("other").error_code(), ::grpc::StatusCode::PERMISSION_DENIED);

    // Both successful appends landed under the cert's tenant.
    ::grpc::ClientContext ctx;
    asv1::GetItemsRequest req;
    req.set_session("s");
    asv1::GetItemsResponse resp;
    ASSERT_TRUE(stub->GetItems(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.items_size(), 2);
    server.Stop();
    Sh("rm -rf " + d);
}

// ---- Wiring: NodeData and AgentState on one GrpcServer --------------------

TEST(AgentStateServiceWiring, TwoServicesShareOnePort) {
    kv_ctx_config_t cfg{};
    cfg.abi_version = KVCACHE_ABI_VERSION;
    cfg.tenant_id   = "wiring-tenant";
    cfg.model_id    = "wiring-model";
    kv_ctx_t* kctx  = nullptr;
    ASSERT_EQ(kv_ctx_open(&cfg, &kctx), KV_OK);
    kvcache::node::grpc_server::NodeDataServiceImpl node_data(kctx);

    std::string err;
    auto store = OrderedLogStore::Open("", &err);
    auto blobs = BlobStore::Open("", &err);
    AgentStateServiceImpl agent_state(store.get(), blobs.get());

    GrpcServer::Options o;
    o.bind_host = "127.0.0.1";
    o.port      = 0;
    GrpcServer server(o, std::vector<::grpc::Service*>{&node_data, &agent_state});
    ASSERT_TRUE(server.Ok()) << server.error();
    auto ch = ::grpc::CreateChannel("127.0.0.1:" + std::to_string(server.BoundPort()),
                                    ::grpc::InsecureChannelCredentials());
    {
        auto stub = asv1::AgentState::NewStub(ch);
        ::grpc::ClientContext ctx;
        asv1::AppendItemsRequest req;
        req.set_tenant("t");
        req.set_session("s");
        req.add_items("x");
        asv1::AppendItemsResponse resp;
        EXPECT_TRUE(stub->AppendItems(&ctx, req, &resp).ok());
    }
    {
        auto stub = kvcache::proto::NodeData::NewStub(ch);
        ::grpc::ClientContext ctx;
        kvcache::proto::LookupRequest req;
        kvcache::proto::LookupResponse resp;
        EXPECT_NE(stub->Lookup(&ctx, req, &resp).error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
    }
    server.Stop();
    kv_ctx_close(kctx);
}
