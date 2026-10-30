// OrderedLogStore — see ordered_log_store.h.
#include "session/ordered_log_store.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace kvcache::node::session {

namespace {

uint32_t Crc32(const uint8_t* p, std::size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void PutU32(std::string* out, uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    out->append(b, 4);
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string EncodeRecord(const std::vector<Mutation>& batch) {
    std::string rec;
    PutU32(&rec, 0);  // record_len, patched below
    PutU32(&rec, static_cast<uint32_t>(batch.size()));
    for (const auto& m : batch) {
        rec.push_back(static_cast<char>(m.op));
        PutU32(&rec, static_cast<uint32_t>(m.key.size()));
        rec.append(m.key);
        const std::size_t vlen = m.op == Mutation::kPut ? m.value.size() : 0;
        PutU32(&rec, static_cast<uint32_t>(vlen));
        rec.append(m.value.data(), vlen);
    }
    const uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(rec.data()) + 4, rec.size() - 4);
    PutU32(&rec, crc);
    const uint32_t len = static_cast<uint32_t>(rec.size());
    for (int i = 0; i < 4; ++i) rec[i] = static_cast<char>((len >> (8 * i)) & 0xFF);
    return rec;
}

// Decode one record body [n .. last value byte]. False if malformed.
bool DecodeBody(const uint8_t* p, std::size_t n, std::vector<Mutation>* out) {
    if (n < 4) return false;
    const uint32_t count = GetU32(p);
    std::size_t off = 4;
    out->clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 1 + 4 > n) return false;
        Mutation m;
        const uint8_t op = p[off++];
        if (op != Mutation::kPut && op != Mutation::kDel) return false;
        m.op = static_cast<Mutation::Op>(op);
        const uint32_t klen = GetU32(p + off);
        off += 4;
        if (klen > n - off || n - off - klen < 4) return false;
        m.key.assign(reinterpret_cast<const char*>(p + off), klen);
        off += klen;
        const uint32_t vlen = GetU32(p + off);
        off += 4;
        if (vlen > n - off) return false;
        m.value.assign(reinterpret_cast<const char*>(p + off), vlen);
        off += vlen;
        out->push_back(std::move(m));
    }
    return off == n;
}

bool WriteAll(int fd, const char* p, std::size_t n, std::string* err) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (err) *err = std::string("write: ") + std::strerror(errno);
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Make a create or rename in `path`'s directory durable.
void FsyncParentDir(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash + 1);
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

bool PrefixMatches(const std::string& key, const std::string& prefix) {
    return key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

std::unique_ptr<OrderedLogStore> OrderedLogStore::Open(const std::string& path, std::string* err,
                                                       std::size_t compact_min_bytes) {
    std::unique_ptr<OrderedLogStore> s(new OrderedLogStore());
    s->compact_min_bytes_ = compact_min_bytes;
    if (path.empty()) return s;

    s->path_ = path;
    const bool existed = ::access(path.c_str(), F_OK) == 0;
    s->fd_   = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (s->fd_ < 0) {
        if (err) *err = "open " + path + ": " + std::strerror(errno);
        return nullptr;
    }
    // One writer per log: a second process appending at its own offset would
    // overwrite acknowledged records. The lock is released when fd_ closes.
    if (::flock(s->fd_, LOCK_EX | LOCK_NB) != 0) {
        if (err) {
            *err = errno == EWOULDBLOCK ? path + " is in use by another process"
                                        : "flock " + path + ": " + std::strerror(errno);
        }
        return nullptr;
    }
    if (!existed) FsyncParentDir(path);

    // Replay every complete record. A bad record at the end is a torn append
    // and is truncated; a bad record with more data after it is corruption,
    // and Open refuses rather than delete what follows.
    std::vector<uint8_t> buf;
    struct stat st {};
    if (::fstat(s->fd_, &st) != 0) {
        if (err) *err = std::string("fstat: ") + std::strerror(errno);
        return nullptr;
    }
    buf.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < buf.size()) {
        const ssize_t r =
            ::pread(s->fd_, buf.data() + got, buf.size() - got, static_cast<off_t>(got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    buf.resize(got);

    std::size_t off = 0;
    std::vector<Mutation> batch;
    while (buf.size() - off >= 12) {
        const std::size_t remaining = buf.size() - off;
        const uint32_t rec_len      = GetU32(buf.data() + off);
        bool ok = rec_len >= 12 && rec_len <= remaining;
        if (ok) {
            const uint8_t* body    = buf.data() + off + 4;
            const std::size_t blen = rec_len - 8;
            ok = Crc32(body, blen) == GetU32(buf.data() + off + rec_len - 4) &&
                 DecodeBody(body, blen, &batch);
        }
        if (!ok) {
            const bool torn_tail =
                rec_len > remaining || (rec_len >= 12 && rec_len == remaining) ||
                std::all_of(buf.begin() + static_cast<std::ptrdiff_t>(off), buf.end(),
                            [](uint8_t b) { return b == 0; });
            if (!torn_tail) {
                if (err) {
                    *err = path + " is corrupt at offset " + std::to_string(off) +
                           " with complete records after it; refusing to truncate";
                }
                return nullptr;
            }
            break;
        }
        s->ApplyToMapLocked(batch);
        off += rec_len;
    }
    if (off < buf.size()) {
        if (::ftruncate(s->fd_, static_cast<off_t>(off)) != 0) {
            if (err) *err = std::string("ftruncate: ") + std::strerror(errno);
            return nullptr;
        }
        ::fsync(s->fd_);
    }
    s->log_bytes_ = off;
    if (::lseek(s->fd_, static_cast<off_t>(off), SEEK_SET) < 0) {
        if (err) *err = std::string("lseek: ") + std::strerror(errno);
        return nullptr;
    }
    return s;
}

OrderedLogStore::~OrderedLogStore() {
    if (fd_ >= 0) ::close(fd_);
}

void OrderedLogStore::ApplyToMapLocked(const std::vector<Mutation>& batch) {
    for (const auto& m : batch) {
        auto it = map_.find(m.key);
        if (it != map_.end()) {
            live_bytes_ -= it->first.size() + it->second.size();
            if (m.op == Mutation::kDel) {
                map_.erase(it);
                continue;
            }
            it->second = m.value;
            live_bytes_ += it->first.size() + it->second.size();
        } else if (m.op == Mutation::kPut) {
            map_.emplace(m.key, m.value);
            live_bytes_ += m.key.size() + m.value.size();
        }
    }
}

bool OrderedLogStore::AppendLocked(const std::vector<Mutation>& batch, std::string* err) {
    if (fd_ < 0) return true;
    const std::string rec = EncodeRecord(batch);
    bool ok = WriteAll(fd_, rec.data(), rec.size(), err);
    if (ok && ::fsync(fd_) != 0) {
        if (err) *err = std::string("fsync: ") + std::strerror(errno);
        ok = false;
    }
    if (!ok) {
        // Drop whatever part of the record reached the file so the next
        // append does not land behind a torn record.
        if (::ftruncate(fd_, static_cast<off_t>(log_bytes_)) == 0) {
            ::lseek(fd_, static_cast<off_t>(log_bytes_), SEEK_SET);
        }
        return false;
    }
    log_bytes_ += rec.size();
    return true;
}

bool OrderedLogStore::MaybeCompactLocked(std::string* err) {
    if (fd_ < 0) return true;
    if (log_bytes_ <= std::max(compact_min_bytes_, 2 * live_bytes_)) return true;

    const std::string tmp = path_ + ".compact";
    const int tfd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (tfd < 0) {
        if (err) *err = "open " + tmp + ": " + std::strerror(errno);
        return false;
    }
    // Lock the new file before it takes the log's name, so no other process
    // can claim it between the rename and our switch to it.
    if (::flock(tfd, LOCK_EX | LOCK_NB) != 0) {
        if (err) *err = std::string("flock: ") + std::strerror(errno);
        ::close(tfd);
        ::unlink(tmp.c_str());
        return false;
    }
    std::size_t written = 0;
    bool ok = true;
    std::vector<Mutation> chunk;
    std::size_t chunk_bytes = 0;
    auto flush = [&]() {
        if (chunk.empty()) return;
        const std::string rec = EncodeRecord(chunk);
        ok = ok && WriteAll(tfd, rec.data(), rec.size(), err);
        written += rec.size();
        chunk.clear();
        chunk_bytes = 0;
    };
    for (const auto& [k, v] : map_) {
        chunk.push_back({Mutation::kPut, k, v});
        chunk_bytes += k.size() + v.size();
        if (chunk_bytes >= (1u << 20)) flush();
    }
    flush();
    if (ok && ::fsync(tfd) != 0) {
        if (err) *err = std::string("fsync: ") + std::strerror(errno);
        ok = false;
    }
    if (ok && ::rename(tmp.c_str(), path_.c_str()) != 0) {
        if (err) *err = std::string("rename: ") + std::strerror(errno);
        ok = false;
    }
    if (!ok) {
        ::close(tfd);
        ::unlink(tmp.c_str());
        return false;
    }
    FsyncParentDir(path_);
    // Keep appending through the compacted file's own descriptor: no reopen
    // that could fail and leave us writing to the unlinked old log.
    ::close(fd_);
    fd_        = tfd;
    log_bytes_ = written;
    ::lseek(fd_, static_cast<off_t>(log_bytes_), SEEK_SET);
    return true;
}

bool OrderedLogStore::Apply(const std::vector<Mutation>& batch, std::string* err) {
    if (batch.empty()) return true;
    std::lock_guard<std::mutex> lk(mu_);
    if (!AppendLocked(batch, err)) return false;
    ApplyToMapLocked(batch);
    // A failed compaction leaves the longer but valid log in place.
    std::string cerr;
    MaybeCompactLocked(&cerr);
    return true;
}

std::optional<std::string> OrderedLogStore::Get(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

std::vector<KV> OrderedLogStore::Scan(const ScanOptions& o, bool* has_more) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<KV> out;
    bool more = false;
    auto take = [&](const std::map<std::string, std::string>::const_iterator& it) {
        if (o.limit != 0 && out.size() >= o.limit) {
            more = true;
            return false;
        }
        out.push_back({it->first, it->second});
        return true;
    };
    if (!o.reverse) {
        auto it = o.start_after.empty() || o.start_after < o.prefix
                      ? map_.lower_bound(o.prefix)
                      : map_.upper_bound(o.start_after);
        for (; it != map_.end() && PrefixMatches(it->first, o.prefix); ++it) {
            if (!o.end_before.empty() && it->first >= o.end_before) break;
            if (!take(it)) break;
        }
    } else {
        // Position just past the last candidate, then walk down.
        auto it = map_.lower_bound(o.prefix);
        while (it != map_.end() && PrefixMatches(it->first, o.prefix)) ++it;
        if (!o.end_before.empty()) {
            // Use whichever bound is lower in key order (end() is highest).
            auto eb = map_.lower_bound(o.end_before);
            if (it == map_.end() || (eb != map_.end() && eb->first < it->first)) it = eb;
        }
        while (it != map_.begin()) {
            --it;
            if (!PrefixMatches(it->first, o.prefix)) break;
            if (!o.start_after.empty() && it->first <= o.start_after) break;
            if (!take(it)) break;
        }
    }
    if (has_more) *has_more = more;
    return out;
}

std::size_t OrderedLogStore::DeletePrefix(const std::string& prefix, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Mutation> batch;
    for (auto it = map_.lower_bound(prefix); it != map_.end() && PrefixMatches(it->first, prefix);
         ++it) {
        batch.push_back({Mutation::kDel, it->first, {}});
    }
    if (batch.empty()) return 0;
    if (!AppendLocked(batch, err)) return 0;
    ApplyToMapLocked(batch);
    std::string cerr;
    MaybeCompactLocked(&cerr);
    return batch.size();
}

std::size_t OrderedLogStore::LiveBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return live_bytes_;
}

std::size_t OrderedLogStore::LogBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_bytes_;
}

}  // namespace kvcache::node::session
