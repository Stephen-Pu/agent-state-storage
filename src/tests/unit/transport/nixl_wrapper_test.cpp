#include "transport/nixl_wrapper.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace kvcache::node::transport;

namespace {
std::unique_ptr<INixlBackend> Loopback() {
    BackendOptions o; o.name = "loopback";
    std::string err;
    auto b = CreateBackend(o, &err);
    EXPECT_TRUE(b) << err;
    return b;
}
}  // namespace

TEST(NixlBackendTest, UnknownBackendFails) {
    BackendOptions o; o.name = "ucx";
    std::string err;
    auto b = CreateBackend(o, &err);
    EXPECT_EQ(b, nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(NixlBackendTest, RegisterResolveUnregister) {
    auto b = Loopback();
    std::vector<uint8_t> buf(1024, 0xAB);
    std::string err;
    auto key = b->RegisterRegion(buf.data(), buf.size(), &err);
    ASSERT_NE(key, kInvalidMrKey) << err;

    void* addr = nullptr;
    std::size_t bytes = 0;
    ASSERT_TRUE(b->ResolveRegion(key, &addr, &bytes));
    EXPECT_EQ(addr, buf.data());
    EXPECT_EQ(bytes, buf.size());

    b->UnregisterRegion(key);
    EXPECT_FALSE(b->ResolveRegion(key, &addr, &bytes));
}

TEST(NixlBackendTest, PullCopiesBytes) {
    auto b = Loopback();
    std::vector<uint8_t> src(1024, 0xCD);
    std::vector<uint8_t> dst(1024, 0x00);
    std::string err;
    auto sk = b->RegisterRegion(src.data(), src.size(), &err);
    auto dk = b->RegisterRegion(dst.data(), dst.size(), &err);

    PullRequest req{dk, 0, sk, 0, 1024};
    auto cid = b->Pull(req, &err);
    ASSERT_NE(cid, kInvalidCompletionId) << err;
    EXPECT_TRUE(b->Wait(cid, 1000, &err));
    EXPECT_EQ(std::memcmp(src.data(), dst.data(), 1024), 0);
}

TEST(NixlBackendTest, PullRejectsOutOfBounds) {
    auto b = Loopback();
    std::vector<uint8_t> src(100), dst(100);
    std::string err;
    auto sk = b->RegisterRegion(src.data(), src.size(), &err);
    auto dk = b->RegisterRegion(dst.data(), dst.size(), &err);
    PullRequest req{dk, 0, sk, 50, 100};  // src_off + bytes > 100
    EXPECT_EQ(b->Pull(req, &err), kInvalidCompletionId);
    EXPECT_FALSE(err.empty());
}

TEST(NixlWrapperTest, PullSyncRoundTrip) {
    NixlWrapper w(Loopback());
    std::vector<uint8_t> src(64, 0x55), dst(64, 0x00);
    std::string err;
    auto sk = w.Register(src.data(), src.size(), &err);
    auto dk = w.Register(dst.data(), dst.size(), &err);
    PullRequest r{dk, 0, sk, 0, 64};
    EXPECT_TRUE(w.PullSync(r, 1000, &err)) << err;
    EXPECT_EQ(std::memcmp(src.data(), dst.data(), 64), 0);
}
