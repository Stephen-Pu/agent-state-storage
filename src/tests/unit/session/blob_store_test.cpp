// M1 — BlobStore: named, tenant-scoped byte streams for agent state.
#include "session/blob_store.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>

using kvcache::node::session::BlobStore;

namespace {
class BlobStoreDisk : public ::testing::TestWithParam<bool> {
   protected:
    void SetUp() override {
        if (GetParam()) {
            dir_ = (std::filesystem::temp_directory_path() /
                    ("blob_store_test_" + std::to_string(::getpid()) + "_" +
                     ::testing::UnitTest::GetInstance()->current_test_info()->name()))
                       .string();
            std::filesystem::remove_all(dir_);
        }
        Reopen();
    }
    void TearDown() override {
        store_.reset();
        if (!dir_.empty()) std::filesystem::remove_all(dir_);
    }
    void Reopen() {
        store_.reset();
        std::string err;
        store_ = BlobStore::Open(dir_, &err);
        ASSERT_NE(store_, nullptr) << err;
    }
    bool Put(const std::string& tenant, const std::string& name, const std::string& data) {
        std::string err;
        auto w = store_->Create(tenant, name, &err);
        if (!w) return false;
        // Write in uneven pieces to exercise chunk boundaries.
        for (std::size_t off = 0; off < data.size(); off += 70001) {
            const std::size_t n = std::min<std::size_t>(70001, data.size() - off);
            if (!w->Write(data.data() + off, n, &err)) return false;
        }
        return w->Commit(&err);
    }
    std::optional<std::string> Read(const std::string& tenant, const std::string& name) {
        std::string out, err;
        const bool ok = store_->Read(
            tenant, name,
            [&](const char* p, std::size_t n) {
                out.append(p, n);
                return true;
            },
            &err);
        if (!ok) return std::nullopt;
        return out;
    }
    std::string dir_;
    std::unique_ptr<BlobStore> store_;
};

std::string Pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131) ^ (i >> 7));
    return s;
}
}  // namespace

TEST_P(BlobStoreDisk, RoundTripLargeBlob) {
    const std::string data = Pattern(5u << 20);
    ASSERT_TRUE(Put("t1", "ws/snap-1.tar", data));
    EXPECT_EQ(Read("t1", "ws/snap-1.tar").value_or(""), data);
    EXPECT_EQ(store_->Size("t1", "ws/snap-1.tar").value_or(0), data.size());
}

TEST_P(BlobStoreDisk, UncommittedWriterLeavesNoBlob) {
    std::string err;
    {
        auto w = store_->Create("t1", "half", &err);
        ASSERT_NE(w, nullptr) << err;
        ASSERT_TRUE(w->Write("abc", 3, &err));
    }  // destroyed without Commit
    EXPECT_FALSE(Read("t1", "half").has_value());
    EXPECT_FALSE(store_->Size("t1", "half").has_value());
    if (!dir_.empty()) {
        // No temp files left behind.
        std::size_t files = 0;
        for (auto& e : std::filesystem::recursive_directory_iterator(dir_)) {
            if (e.is_regular_file()) ++files;
        }
        EXPECT_EQ(files, 0u);
    }
}

TEST_P(BlobStoreDisk, OverwriteReplacesAtomically) {
    ASSERT_TRUE(Put("t1", "n", "old-content"));
    std::string err;
    auto w = store_->Create("t1", "n", &err);
    ASSERT_NE(w, nullptr) << err;
    ASSERT_TRUE(w->Write("new", 3, &err));
    // Before Commit, readers still see the old blob.
    EXPECT_EQ(Read("t1", "n").value_or(""), "old-content");
    ASSERT_TRUE(w->Commit(&err)) << err;
    EXPECT_EQ(Read("t1", "n").value_or(""), "new");
}

TEST_P(BlobStoreDisk, TenantsDoNotShareNames) {
    ASSERT_TRUE(Put("alice", "same", "A"));
    ASSERT_TRUE(Put("bob", "same", "B"));
    EXPECT_EQ(Read("alice", "same").value_or(""), "A");
    EXPECT_EQ(Read("bob", "same").value_or(""), "B");
    // A tenant/name split must not collide with a different split.
    ASSERT_TRUE(Put("a", std::string("lice\0same", 9), "X"));
    EXPECT_EQ(Read("alice", "same").value_or(""), "A");
}

TEST_P(BlobStoreDisk, SurvivesReopen) {
    if (!GetParam()) GTEST_SKIP() << "in-memory store is not durable";
    ASSERT_TRUE(Put("t1", "keep", "durable"));
    Reopen();
    EXPECT_EQ(Read("t1", "keep").value_or(""), "durable");
}

TEST_P(BlobStoreDisk, DeleteThenMissing) {
    ASSERT_TRUE(Put("t1", "gone", "x"));
    EXPECT_TRUE(store_->Delete("t1", "gone"));
    EXPECT_FALSE(store_->Delete("t1", "gone"));
    std::string err;
    EXPECT_FALSE(store_->Read("t1", "gone", [](const char*, std::size_t) { return true; }, &err));
    EXPECT_TRUE(err.empty());  // not found is not an error
}

TEST_P(BlobStoreDisk, EmptyBlobIsAllowed) {
    ASSERT_TRUE(Put("t1", "empty", ""));
    EXPECT_EQ(Read("t1", "empty").value_or("missing"), "");
    EXPECT_EQ(store_->Size("t1", "empty").value_or(99), 0u);
}

INSTANTIATE_TEST_SUITE_P(Modes, BlobStoreDisk, ::testing::Values(false, true),
                         [](const auto& i) { return i.param ? "Disk" : "Memory"; });
