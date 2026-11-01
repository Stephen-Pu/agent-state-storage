// M1 — OrderedLogStore: ordered map + append-only batch log for agent state.
#include "session/ordered_log_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>

using kvcache::node::session::KV;
using kvcache::node::session::Mutation;
using kvcache::node::session::OrderedLogStore;
using kvcache::node::session::ScanOptions;

namespace {
std::string TempLogPath(const char* tag) {
    return std::string("ordered_log_store_test_") + tag + ".log";
}
Mutation Put(std::string k, std::string v) { return {Mutation::kPut, std::move(k), std::move(v)}; }
Mutation Del(std::string k) { return {Mutation::kDel, std::move(k), {}}; }
std::vector<std::string> Keys(const std::vector<KV>& kvs) {
    std::vector<std::string> out;
    for (const auto& kv : kvs) out.push_back(kv.key);
    return out;
}
}  // namespace

TEST(OrderedLogStore, PutGetDelete) {
    std::string err;
    auto s = OrderedLogStore::Open("", &err);
    ASSERT_NE(s, nullptr) << err;
    ASSERT_TRUE(s->Apply({Put("a", "1"), Put("b", "2")}, &err)) << err;
    EXPECT_EQ(s->Get("a").value_or("?"), "1");
    ASSERT_TRUE(s->Apply({Del("a")}, &err)) << err;
    EXPECT_FALSE(s->Get("a").has_value());
    EXPECT_EQ(s->Get("b").value_or("?"), "2");
}

TEST(OrderedLogStore, ScanPrefixForwardAndReverseWithLimit) {
    std::string err;
    auto s = OrderedLogStore::Open("", &err);
    ASSERT_NE(s, nullptr) << err;
    ASSERT_TRUE(s->Apply({Put("p/1", "a"), Put("p/2", "b"), Put("p/3", "c"), Put("q/1", "d")}, &err));

    bool more = true;
    ScanOptions o;
    o.prefix = "p/";
    EXPECT_EQ(Keys(s->Scan(o, &more)), (std::vector<std::string>{"p/1", "p/2", "p/3"}));
    EXPECT_FALSE(more);

    o.limit = 2;
    EXPECT_EQ(Keys(s->Scan(o, &more)), (std::vector<std::string>{"p/1", "p/2"}));
    EXPECT_TRUE(more);

    o.reverse = true;
    EXPECT_EQ(Keys(s->Scan(o, &more)), (std::vector<std::string>{"p/3", "p/2"}));
    EXPECT_TRUE(more);

    o.limit = 3;
    EXPECT_EQ(Keys(s->Scan(o, &more)), (std::vector<std::string>{"p/3", "p/2", "p/1"}));
    EXPECT_FALSE(more);
}

TEST(OrderedLogStore, ScanBoundsAreExclusive) {
    std::string err;
    auto s = OrderedLogStore::Open("", &err);
    ASSERT_NE(s, nullptr) << err;
    ASSERT_TRUE(s->Apply({Put("k1", ""), Put("k2", ""), Put("k3", ""), Put("k4", "")}, &err));
    bool more = false;
    ScanOptions o;
    o.prefix      = "k";
    o.start_after = "k1";
    o.end_before  = "k4";
    EXPECT_EQ(Keys(s->Scan(o, &more)), (std::vector<std::string>{"k2", "k3"}));
    o.reverse = true;
    EXPECT_EQ(Keys(s->Scan(o, &more)), (std::vector<std::string>{"k3", "k2"}));
}

TEST(OrderedLogStore, ReopenReplaysLog) {
    const std::string path = TempLogPath("replay");
    std::remove(path.c_str());
    std::string err;
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        ASSERT_TRUE(s->Apply({Put("x", "1"), Put("y", "2")}, &err));
        ASSERT_TRUE(s->Apply({Del("x"), Put("z", std::string("\0bin\xff", 5))}, &err));
    }
    auto s = OrderedLogStore::Open(path, &err);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_FALSE(s->Get("x").has_value());
    EXPECT_EQ(s->Get("y").value_or("?"), "2");
    EXPECT_EQ(s->Get("z").value_or("?"), std::string("\0bin\xff", 5));
    std::remove(path.c_str());
}

TEST(OrderedLogStore, BatchIsAtomicAcrossReopen) {
    const std::string path = TempLogPath("atomic");
    std::remove(path.c_str());
    std::string err;
    off_t first_batch_end = 0;
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        ASSERT_TRUE(s->Apply({Put("a", "1")}, &err));
        first_batch_end = static_cast<off_t>(s->LogBytes());
        ASSERT_TRUE(s->Apply({Put("b", "2"), Put("c", "3"), Put("d", "4")}, &err));
    }
    // Cut the file in the middle of the second batch: none of b/c/d may survive.
    ASSERT_EQ(::truncate(path.c_str(), first_batch_end + 20), 0);
    auto s = OrderedLogStore::Open(path, &err);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_EQ(s->Get("a").value_or("?"), "1");
    EXPECT_FALSE(s->Get("b").has_value());
    EXPECT_FALSE(s->Get("c").has_value());
    EXPECT_FALSE(s->Get("d").has_value());
    std::remove(path.c_str());
}

TEST(OrderedLogStore, TornTailIsTruncated) {
    const std::string path = TempLogPath("torn");
    std::remove(path.c_str());
    std::string err;
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        ASSERT_TRUE(s->Apply({Put("keep", "v")}, &err));
    }
    {
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        ASSERT_GE(fd, 0);
        const char junk[] = "\x40\x00\x00\x00\x01garbage";  // claims 64 bytes, has 12
        ASSERT_EQ(::write(fd, junk, sizeof(junk) - 1), static_cast<ssize_t>(sizeof(junk) - 1));
        ::close(fd);
    }
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        EXPECT_EQ(s->Get("keep").value_or("?"), "v");
        ASSERT_TRUE(s->Apply({Put("after", "w")}, &err)) << err;
    }
    auto s = OrderedLogStore::Open(path, &err);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_EQ(s->Get("keep").value_or("?"), "v");
    EXPECT_EQ(s->Get("after").value_or("?"), "w");
    std::remove(path.c_str());
}

TEST(OrderedLogStore, CompactionKeepsLiveData) {
    const std::string path = TempLogPath("compact");
    std::remove(path.c_str());
    std::string err;
    std::size_t peak = 0;
    {
        auto s = OrderedLogStore::Open(path, &err, /*compact_min_bytes=*/1024);
        ASSERT_NE(s, nullptr) << err;
        for (int i = 0; i < 500; ++i) {
            ASSERT_TRUE(s->Apply({Put("hot", std::string(100, static_cast<char>('a' + i % 26))),
                                  Put("k" + std::to_string(i % 5), std::to_string(i))},
                                 &err))
                << err;
            peak = std::max(peak, s->LogBytes());
        }
        EXPECT_LT(s->LogBytes(), 4096u);  // 500 overwrites would be ~60 KiB uncompacted
        EXPECT_LT(peak, 8192u);
    }
    auto s = OrderedLogStore::Open(path, &err, 1024);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_EQ(s->Get("hot").value_or("?"), std::string(100, static_cast<char>('a' + 499 % 26)));
    for (int j = 0; j < 5; ++j) {
        EXPECT_EQ(s->Get("k" + std::to_string(j)).value_or("?"), std::to_string(495 + j));
    }
    std::remove(path.c_str());
}

TEST(OrderedLogStore, InMemoryModeNeedsNoFile) {
    std::string err;
    auto s = OrderedLogStore::Open("", &err);
    ASSERT_NE(s, nullptr) << err;
    ASSERT_TRUE(s->Apply({Put("a", "1")}, &err));
    EXPECT_EQ(s->LogBytes(), 0u);
    EXPECT_EQ(s->LiveBytes(), 2u);
}

TEST(OrderedLogStore, DeletePrefixRemovesOnlyThatPrefix) {
    const std::string path = TempLogPath("delprefix");
    std::remove(path.c_str());
    std::string err;
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        ASSERT_TRUE(s->Apply({Put("t/a", "1"), Put("t/b", "2"), Put("t", "3"), Put("u/a", "4")}, &err));
        EXPECT_EQ(s->DeletePrefix("t/", &err), 2u);
    }
    auto s = OrderedLogStore::Open(path, &err);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_FALSE(s->Get("t/a").has_value());
    EXPECT_FALSE(s->Get("t/b").has_value());
    EXPECT_EQ(s->Get("t").value_or("?"), "3");
    EXPECT_EQ(s->Get("u/a").value_or("?"), "4");
    std::remove(path.c_str());
}

TEST(OrderedLogStore, OpenFailsForUnwritablePath) {
    std::string err;
    auto s = OrderedLogStore::Open("/nonexistent-dir-m1/x.log", &err);
    EXPECT_EQ(s, nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(OrderedLogStore, SecondOpenOfSameLogFails) {
    const std::string path = TempLogPath("locked");
    std::remove(path.c_str());
    std::string err;
    auto first = OrderedLogStore::Open(path, &err);
    ASSERT_NE(first, nullptr) << err;
    ASSERT_TRUE(first->Apply({Put("a", "1")}, &err));
    std::string err2;
    auto second = OrderedLogStore::Open(path, &err2);
    EXPECT_EQ(second, nullptr);
    EXPECT_NE(err2.find("in use"), std::string::npos) << err2;
    first.reset();
    auto third = OrderedLogStore::Open(path, &err);  // lock released on close
    ASSERT_NE(third, nullptr) << err;
    EXPECT_EQ(third->Get("a").value_or("?"), "1");
    std::remove(path.c_str());
}

TEST(OrderedLogStore, CorruptRecordInMiddleRefusesToOpen) {
    const std::string path = TempLogPath("midcorrupt");
    std::remove(path.c_str());
    std::string err;
    off_t size = 0;
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        ASSERT_TRUE(s->Apply({Put("a", "first-value")}, &err));
        ASSERT_TRUE(s->Apply({Put("b", "second")}, &err));
        ASSERT_TRUE(s->Apply({Put("c", "third")}, &err));
        size = static_cast<off_t>(s->LogBytes());
    }
    {
        // Flip a byte inside the FIRST record's value: complete records follow it.
        const int fd = ::open(path.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        char c = 0;
        ASSERT_EQ(::pread(fd, &c, 1, 20), 1);
        c ^= 0x5A;
        ASSERT_EQ(::pwrite(fd, &c, 1, 20), 1);
        ::close(fd);
    }
    std::string err2;
    auto s = OrderedLogStore::Open(path, &err2);
    EXPECT_EQ(s, nullptr);
    EXPECT_NE(err2.find("corrupt"), std::string::npos) << err2;
    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_size, size);  // nothing truncated
    std::remove(path.c_str());
}

TEST(OrderedLogStore, CorruptLastRecordIsTreatedAsTornTail) {
    const std::string path = TempLogPath("lastcorrupt");
    std::remove(path.c_str());
    std::string err;
    off_t first_end = 0;
    {
        auto s = OrderedLogStore::Open(path, &err);
        ASSERT_NE(s, nullptr) << err;
        ASSERT_TRUE(s->Apply({Put("keep", "v")}, &err));
        first_end = static_cast<off_t>(s->LogBytes());
        ASSERT_TRUE(s->Apply({Put("torn", "w")}, &err));
    }
    {
        const int fd = ::open(path.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        char c = 0;
        ASSERT_EQ(::pread(fd, &c, 1, first_end + 10), 1);
        c ^= 0x5A;
        ASSERT_EQ(::pwrite(fd, &c, 1, first_end + 10), 1);
        ::close(fd);
    }
    auto s = OrderedLogStore::Open(path, &err);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_EQ(s->Get("keep").value_or("?"), "v");
    EXPECT_FALSE(s->Get("torn").has_value());
    std::remove(path.c_str());
}

TEST(OrderedLogStore, LockSurvivesCompaction) {
    const std::string path = TempLogPath("lockcompact");
    std::remove(path.c_str());
    std::string err;
    auto s = OrderedLogStore::Open(path, &err, /*compact_min_bytes=*/1024);
    ASSERT_NE(s, nullptr) << err;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(s->Apply({Put("hot", std::string(100, 'x'))}, &err)) << err;
    }
    ASSERT_LT(s->LogBytes(), 2048u);  // compaction has run
    std::string err2;
    EXPECT_EQ(OrderedLogStore::Open(path, &err2, 1024), nullptr);
    ASSERT_TRUE(s->Apply({Put("after", "1")}, &err)) << err;
    s.reset();
    auto r = OrderedLogStore::Open(path, &err, 1024);
    ASSERT_NE(r, nullptr) << err;
    EXPECT_EQ(r->Get("after").value_or("?"), "1");
    std::remove(path.c_str());
}
