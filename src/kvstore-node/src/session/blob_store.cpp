// BlobStore — see blob_store.h.
#include "session/blob_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <random>

#include "b3_facade.h"

namespace kvcache::node::session {

namespace {

constexpr const char* kTmpPrefix = ".tmp-";

std::string Errno(const std::string& what) { return what + ": " + std::strerror(errno); }

// Lets a reader keep its own snapshot of an in-memory blob while a writer
// replaces it.
using MemBlob = std::shared_ptr<const std::string>;

}  // namespace

class DiskWriter final : public BlobStore::Writer {
   public:
    DiskWriter(int fd, std::string tmp, std::string final_path)
        : fd_(fd), tmp_(std::move(tmp)), final_(std::move(final_path)) {}
    ~DiskWriter() override {
        if (fd_ >= 0) ::close(fd_);
        if (!committed_) ::unlink(tmp_.c_str());
    }
    bool Write(const char* data, std::size_t n, std::string* err) override {
        if (fd_ < 0) {
            if (err) *err = "writer is closed";
            return false;
        }
        if (size_ + n > BlobStore::kMaxBlobBytes) {
            if (err) *err = "blob exceeds the 4 GiB limit";
            Fail();
            return false;
        }
        while (n > 0) {
            const ssize_t w = ::write(fd_, data, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (err) *err = Errno("write");
                Fail();
                return false;
            }
            data += w;
            n -= static_cast<std::size_t>(w);
            size_ += static_cast<uint64_t>(w);
        }
        return true;
    }
    bool Commit(std::string* err) override {
        if (fd_ < 0) {
            if (err) *err = "writer is closed";
            return false;
        }
        if (::fsync(fd_) != 0) {
            if (err) *err = Errno("fsync");
            Fail();
            return false;
        }
        ::close(fd_);
        fd_ = -1;
        if (::rename(tmp_.c_str(), final_.c_str()) != 0) {
            if (err) *err = Errno("rename");
            return false;
        }
        committed_ = true;
        // Make the rename itself durable.
        const std::string dir = std::filesystem::path(final_).parent_path().string();
        const int dfd = ::open(dir.c_str(), O_RDONLY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
        return true;
    }

   private:
    void Fail() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int         fd_;
    std::string tmp_;
    std::string final_;
    uint64_t    size_      = 0;
    bool        committed_ = false;
};

class MemWriter final : public BlobStore::Writer {
   public:
    MemWriter(BlobStore* store, std::string key) : store_(store), key_(std::move(key)) {}
    bool Write(const char* data, std::size_t n, std::string* err) override {
        if (done_) {
            if (err) *err = "writer is closed";
            return false;
        }
        if (buf_.size() + n > BlobStore::kMaxBlobBytes) {
            if (err) *err = "blob exceeds the 4 GiB limit";
            done_ = true;
            return false;
        }
        buf_.append(data, n);
        return true;
    }
    bool Commit(std::string* err) override {
        if (done_) {
            if (err) *err = "writer is closed";
            return false;
        }
        done_ = true;
        std::lock_guard<std::mutex> lk(store_->mu_);
        store_->mem_[key_] = std::make_shared<const std::string>(std::move(buf_));
        return true;
    }

   private:
    BlobStore*  store_;
    std::string key_;
    std::string buf_;
    bool        done_ = false;
};

std::unique_ptr<BlobStore> BlobStore::Open(const std::string& dir, std::string* err) {
    std::unique_ptr<BlobStore> s(new BlobStore());
    s->dir_ = dir;
    if (dir.empty()) return s;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (err) *err = "create " + dir + ": " + ec.message();
        return nullptr;
    }
    // Remove temp files a crash left behind.
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string fname = e.path().filename().string();
        if (fname.rfind(kTmpPrefix, 0) == 0) std::filesystem::remove(e.path(), ec);
    }
    return s;
}

std::string BlobStore::MemKey(const std::string& tenant, const std::string& name) {
    std::string k;
    const uint32_t n = static_cast<uint32_t>(tenant.size());
    k.append(reinterpret_cast<const char*>(&n), sizeof(n));
    k.append(tenant);
    k.append(name);
    return k;
}

std::string BlobStore::PathFor(const std::string& tenant, const std::string& name) const {
    hash::Hasher h;
    const std::string key = MemKey(tenant, name);
    h.Update({reinterpret_cast<const uint8_t*>(key.data()), key.size()});
    const auto d = h.Finalize256();
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : d) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 15]);
    }
    return (std::filesystem::path(dir_) / hex).string();
}

std::unique_ptr<BlobStore::Writer> BlobStore::Create(const std::string& tenant,
                                                     const std::string& name, std::string* err) {
    if (dir_.empty()) return std::make_unique<MemWriter>(this, MemKey(tenant, name));
    static std::atomic<uint64_t> seq{0};
    thread_local std::mt19937_64 rng{std::random_device{}()};
    const std::string tmp = (std::filesystem::path(dir_) /
                             (std::string(kTmpPrefix) + std::to_string(seq.fetch_add(1)) + "-" +
                              std::to_string(rng())))
                                .string();
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (err) *err = Errno("open " + tmp);
        return nullptr;
    }
    return std::make_unique<DiskWriter>(fd, tmp, PathFor(tenant, name));
}

bool BlobStore::Read(const std::string& tenant, const std::string& name,
                     const std::function<bool(const char*, std::size_t)>& sink,
                     std::string* err) const {
    constexpr std::size_t kChunk = 1u << 20;
    if (dir_.empty()) {
        MemBlob blob;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = mem_.find(MemKey(tenant, name));
            if (it == mem_.end()) return false;
            blob = it->second;
        }
        for (std::size_t off = 0; off < blob->size() || off == 0; off += kChunk) {
            const std::size_t n = std::min(kChunk, blob->size() - off);
            if (n == 0) break;
            if (!sink(blob->data() + off, n)) {
                if (err) *err = "read cancelled";
                return false;
            }
        }
        return true;
    }
    const std::string path = PathFor(tenant, name);
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT && err) *err = Errno("open");
        return false;
    }
    std::string buf(kChunk, '\0');
    bool ok = true;
    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            if (err) *err = Errno("read");
            ok = false;
            break;
        }
        if (r == 0) break;
        if (!sink(buf.data(), static_cast<std::size_t>(r))) {
            if (err) *err = "read cancelled";
            ok = false;
            break;
        }
    }
    ::close(fd);
    return ok;
}

std::optional<uint64_t> BlobStore::Size(const std::string& tenant, const std::string& name) const {
    if (dir_.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = mem_.find(MemKey(tenant, name));
        if (it == mem_.end()) return std::nullopt;
        return it->second->size();
    }
    struct stat st {};
    if (::stat(PathFor(tenant, name).c_str(), &st) != 0) return std::nullopt;
    return static_cast<uint64_t>(st.st_size);
}

bool BlobStore::Delete(const std::string& tenant, const std::string& name) {
    if (dir_.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        return mem_.erase(MemKey(tenant, name)) > 0;
    }
    return ::unlink(PathFor(tenant, name).c_str()) == 0;
}

}  // namespace kvcache::node::session
