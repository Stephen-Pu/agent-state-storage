// BlobStore — named, tenant-scoped byte streams behind agentstate.v1 (M1).
//
// Holds payloads too large for a record, such as a sandbox workspace tarball.
// On disk each blob is one file, `<dir>/<hex BLAKE3-256(tenant \0 name)>`, so
// tenant and name never appear in a path. A Writer streams into a temp file
// in the same directory; Commit() fsyncs it and renames it over the old blob,
// so readers see either the whole old blob or the whole new one. A Writer
// destroyed without Commit() deletes its temp file. An empty dir keeps blobs
// in memory only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace kvcache::node::session {

class BlobStore {
   public:
    static constexpr uint64_t kMaxBlobBytes = 4ull << 30;

    class Writer {
       public:
        virtual ~Writer() = default;
        // False with *err set on an I/O error or once the blob would exceed
        // kMaxBlobBytes; the writer is then unusable.
        virtual bool Write(const char* data, std::size_t n, std::string* err) = 0;
        virtual bool Commit(std::string* err) = 0;
    };

    static std::unique_ptr<BlobStore> Open(const std::string& dir, std::string* err);

    std::unique_ptr<Writer> Create(const std::string& tenant, const std::string& name,
                                   std::string* err);

    // Streams the blob to `sink` in chunks; `sink` returning false stops the
    // read (and Read returns false with *err set). Returns false with *err
    // empty when the blob does not exist.
    bool Read(const std::string& tenant, const std::string& name,
              const std::function<bool(const char*, std::size_t)>& sink, std::string* err) const;

    std::optional<uint64_t> Size(const std::string& tenant, const std::string& name) const;

    // True if a blob was deleted.
    bool Delete(const std::string& tenant, const std::string& name);

   private:
    friend class DiskWriter;
    friend class MemWriter;
    BlobStore() = default;

    std::string PathFor(const std::string& tenant, const std::string& name) const;
    static std::string MemKey(const std::string& tenant, const std::string& name);

    std::string                                      dir_;  // empty = in memory
    mutable std::mutex                               mu_;
    std::map<std::string, std::shared_ptr<const std::string>> mem_;
};

}  // namespace kvcache::node::session
