// Task 1 — StateWal: append-only B-state persistence (⑬ v0).
#include "persist/state_wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>

#include "state_identity.h"
#include "tier/dram_tier.h"

using kvcache::common::SK_MEMORY;
using kvcache::common::StateIdentity;
using kvcache::node::persist::StateWal;
using kvcache::node::tier::DramKey;

namespace {
// Unique temp path per test; removed in the fixture teardown.
std::string TempWalPath(const char* tag) {
    return std::string("state_wal_test_") + tag + ".wal";
}
DramKey MakeKey(uint8_t seed) {
    DramKey k{};
    k.bytes[0] = seed;
    return k;
}
StateIdentity MakeId(uint8_t seed) {
    StateIdentity id{};
    id.version    = 2;
    id.state_kind = SK_MEMORY;
    id.content_hash[0] = seed;
    return id;
}
}  // namespace

TEST(StateWal, AppendThenGetRoundTrips) {
    const std::string path = TempWalPath("roundtrip");
    std::remove(path.c_str());

    std::string err;
    auto wal = StateWal::Open(path, &err);
    ASSERT_NE(wal, nullptr) << err;

    std::vector<uint8_t> blob(64, 0xB1);
    ASSERT_TRUE(wal->Append(MakeKey(1), MakeId(1), blob.data(), blob.size(), &err)) << err;

    std::vector<uint8_t> out;
    ASSERT_TRUE(wal->Get(MakeKey(1), &out));
    EXPECT_EQ(out, blob);
    EXPECT_FALSE(wal->Get(MakeKey(2), &out));  // absent key
    EXPECT_EQ(wal->Size(), 1u);

    std::remove(path.c_str());
}

TEST(StateWal, ReopenReplaysCommittedRecords) {
    const std::string path = TempWalPath("replay");
    std::remove(path.c_str());
    std::vector<uint8_t> a(16, 0xAA), b(32, 0xBB);
    {
        std::string err;
        auto wal = StateWal::Open(path, &err);
        ASSERT_NE(wal, nullptr) << err;
        ASSERT_TRUE(wal->Append(MakeKey(1), MakeId(1), a.data(), a.size(), &err)) << err;
        ASSERT_TRUE(wal->Append(MakeKey(2), MakeId(2), b.data(), b.size(), &err)) << err;
    }  // wal closed → simulates process exit
    std::string err;
    auto wal2 = StateWal::Open(path, &err);
    ASSERT_NE(wal2, nullptr) << err;
    EXPECT_EQ(wal2->Size(), 2u);
    std::vector<uint8_t> out;
    ASSERT_TRUE(wal2->Get(MakeKey(1), &out)); EXPECT_EQ(out, a);
    ASSERT_TRUE(wal2->Get(MakeKey(2), &out)); EXPECT_EQ(out, b);
    std::remove(path.c_str());
}

TEST(StateWal, TornTailRecordIsTruncatedOnReplay) {
    const std::string path = TempWalPath("torn");
    std::remove(path.c_str());
    std::vector<uint8_t> a(16, 0xAA);
    {
        std::string err;
        auto wal = StateWal::Open(path, &err);
        ASSERT_NE(wal, nullptr) << err;
        ASSERT_TRUE(wal->Append(MakeKey(1), MakeId(1), a.data(), a.size(), &err)) << err;
    }
    // Corrupt the file by appending a garbage partial record (a bogus length
    // prefix claiming more bytes than follow).
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    const uint8_t junk[8] = {0xFF, 0xFF, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
    ASSERT_EQ(::write(fd, junk, sizeof(junk)), static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);

    std::string err;
    auto wal2 = StateWal::Open(path, &err);
    ASSERT_NE(wal2, nullptr) << err;
    EXPECT_EQ(wal2->Size(), 1u);              // torn tail dropped, first record kept
    std::vector<uint8_t> out;
    ASSERT_TRUE(wal2->Get(MakeKey(1), &out)); EXPECT_EQ(out, a);
    // A fresh append after truncation must still round-trip.
    std::vector<uint8_t> c(8, 0xCC);
    ASSERT_TRUE(wal2->Append(MakeKey(3), MakeId(3), c.data(), c.size(), &err)) << err;
    ASSERT_TRUE(wal2->Get(MakeKey(3), &out)); EXPECT_EQ(out, c);
    std::remove(path.c_str());
}
