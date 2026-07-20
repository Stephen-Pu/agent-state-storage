#include "persist/state_wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace kvcache::node::persist {
namespace {

// CRC32 (IEEE 802.3), software fallback — local copy of the helper in
// prefix/art_wal.cpp. Kept local to keep this unit self-contained; a shared
// checksum header is a future consolidation, out of scope for this slice.
uint32_t Crc32(const uint8_t* p, std::size_t n) {
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
            table[i] = c;
        }
        inited = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

constexpr uint8_t  kOpPut     = 1;
constexpr std::size_t kKeyLen = 16;
constexpr std::size_t kIdLen  = sizeof(common::StateIdentity);  // 128

// Little-endian append helpers.
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::unique_ptr<StateWal> StateWal::Open(const std::string& path, std::string* err) {
    auto self = std::unique_ptr<StateWal>(new StateWal());

    // Replay existing records first (read-only pass over the whole file).
    int rfd = ::open(path.c_str(), O_RDONLY);
    if (rfd >= 0) {
        std::vector<uint8_t> buf;
        // Read the whole file into memory (v0 — bounded-memory replay deferred).
        uint8_t chunk[65536];
        ssize_t r;
        while ((r = ::read(rfd, chunk, sizeof(chunk))) > 0)
            buf.insert(buf.end(), chunk, chunk + r);
        ::close(rfd);

        std::size_t off = 0;
        std::size_t good_end = 0;  // byte offset past the last fully-valid record
        while (off + 4 <= buf.size()) {
            const uint32_t rec_len = GetU32(buf.data() + off);
            if (rec_len < 4 + 1 + kKeyLen + kIdLen + 4 + 4) break;  // impossibly short → torn
            if (off + rec_len > buf.size()) break;                   // short tail → torn
            const uint8_t* body     = buf.data() + off + 4;          // [op .. blob]
            const std::size_t body_len = rec_len - 4 - 4;            // exclude len + crc
            const uint32_t want_crc = GetU32(buf.data() + off + rec_len - 4);
            if (Crc32(body, body_len) != want_crc) break;            // corrupt → torn

            const uint8_t op = body[0];
            if (op == kOpPut) {
                const uint8_t* kp = body + 1;
                const uint8_t* bl = kp + kKeyLen + kIdLen;
                const uint32_t blob_len = GetU32(bl);
                const uint8_t* blob = bl + 4;
                // blob_len must exactly account for the rest of the record body;
                // a mismatch means a corrupt-but-CRC-valid record (or a torn
                // write that happened to land on a CRC collision) — treat it
                // the same as a CRC mismatch and stop replay here.
                if (blob_len != body_len - (1 + kKeyLen + kIdLen + 4)) break;
                tier::DramKey key{};
                std::memcpy(key.bytes.data(), kp, kKeyLen);
                self->map_[key].assign(blob, blob + blob_len);
            }
            off      += rec_len;
            good_end  = off;
        }
        // Truncate any torn tail so future appends start from a clean boundary.
        if (good_end < buf.size()) {
            if (::truncate(path.c_str(), static_cast<off_t>(good_end)) != 0) {
                if (err) *err = std::string("state_wal truncate torn tail: ") + std::strerror(errno);
                return nullptr;
            }
        }
    } else if (errno != ENOENT) {
        if (err) *err = std::string("state_wal open(read) ") + path + ": " + std::strerror(errno);
        return nullptr;
    }

    // Open the append fd for subsequent writes.
    self->fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (self->fd_ < 0) {
        if (err) *err = std::string("state_wal open(append) ") + path + ": " + std::strerror(errno);
        return nullptr;
    }
    return self;
}

StateWal::~StateWal() {
    if (fd_ >= 0) ::close(fd_);
}

// On a failed write/fsync below, Append returns false without touching the
// in-memory map — the caller must treat the entry as not persisted. Any
// stray partial bytes left on disk are not cleaned up mid-process; they
// self-heal via torn-tail truncation the next time Open() replays the file.
// Note that after an fsync failure the on-disk state is undefined per
// standard WAL semantics (the OS/filesystem gives no guarantee about which
// bytes made it to stable storage), so a subsequent Open() may or may not
// end up replaying this particular record.
bool StateWal::Append(const tier::DramKey& key, const common::StateIdentity& id,
                      const uint8_t* data, std::size_t n, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);

    const std::size_t rec_len_wide = 4 + 1 + kKeyLen + kIdLen + 4 + n + 4;
    if (rec_len_wide > UINT32_MAX) {
        if (err) *err = "state_wal append: record too large (blob length overflows rec_len)";
        return false;
    }
    const uint32_t rec_len = static_cast<uint32_t>(rec_len_wide);
    std::vector<uint8_t> rec;
    rec.reserve(rec_len);
    PutU32(rec, rec_len);
    rec.push_back(kOpPut);
    rec.insert(rec.end(), key.bytes.begin(), key.bytes.end());
    const uint8_t* idp = reinterpret_cast<const uint8_t*>(&id);
    rec.insert(rec.end(), idp, idp + kIdLen);
    PutU32(rec, static_cast<uint32_t>(n));
    rec.insert(rec.end(), data, data + n);
    const uint32_t crc = Crc32(rec.data() + 4, rec.size() - 4);  // [op .. blob]
    PutU32(rec, crc);

    const ssize_t w = ::write(fd_, rec.data(), rec.size());
    if (w != static_cast<ssize_t>(rec.size())) {
        if (err) *err = std::string("state_wal write: ") + std::strerror(errno);
        return false;
    }
    if (::fsync(fd_) != 0) {
        if (err) *err = std::string("state_wal fsync: ") + std::strerror(errno);
        return false;
    }
    map_[key].assign(data, data + n);  // durable now → safe to index
    return true;
}

bool StateWal::Get(const tier::DramKey& key, std::vector<uint8_t>* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    *out = it->second;
    return true;
}

std::size_t StateWal::Size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

}  // namespace kvcache::node::persist
