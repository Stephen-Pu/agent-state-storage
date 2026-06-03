// LLD §3.3 T4 — RestColdTier implementation (Phase B3).
#include "tier/rest_cold_tier.h"

#include <curl/curl.h>

#include <cstring>
#include <mutex>

namespace kvcache::node::tier {

namespace {

std::string HexOf(const DramKey& k) {
    static const char* kHex = "0123456789abcdef";
    std::string s(k.bytes.size() * 2, '0');
    for (std::size_t i = 0; i < k.bytes.size(); ++i) {
        s[2 * i]     = kHex[(k.bytes[i] >> 4) & 0xf];
        s[2 * i + 1] = kHex[(k.bytes[i]     ) & 0xf];
    }
    return s;
}

// Strip exactly one trailing '/' so "{base}/{key}" never doubles the slash.
std::string TrimTrailingSlash(std::string s) {
    if (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// ---- libcurl transport ----------------------------------------------------
std::once_flag g_curl_once;
void CurlGlobalInit() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteToString(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* out = static_cast<std::string*>(user);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// libcurl read callback for PUT bodies.
struct ReadCtx {
    const uint8_t* p = nullptr;
    std::size_t    left = 0;
};
size_t ReadFromBuf(char* buf, size_t size, size_t nmemb, void* user) {
    auto* rc = static_cast<ReadCtx*>(user);
    const std::size_t want = size * nmemb;
    const std::size_t n = want < rc->left ? want : rc->left;
    if (n) {
        std::memcpy(buf, rc->p, n);
        rc->p += n;
        rc->left -= n;
    }
    return n;
}

class CurlHttpTransport final : public IHttpTransport {
   public:
    CurlHttpTransport() { CurlGlobalInit(); }

    HttpResult Request(const std::string&              method,
                       const std::string&              url,
                       const std::vector<std::string>& headers,
                       const uint8_t*                  body,
                       std::size_t                     n) override {
        HttpResult res;
        CURL* c = curl_easy_init();
        if (!c) {
            res.transport_err = "curl_easy_init failed";
            return res;
        }
        char errbuf[CURL_ERROR_SIZE] = {0};
        std::string resp;
        ReadCtx rctx;

        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

        if (method == "PUT") {
            rctx.p = body;
            rctx.left = n;
            curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(c, CURLOPT_READFUNCTION, ReadFromBuf);
            curl_easy_setopt(c, CURLOPT_READDATA, &rctx);
            curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(n));
        } else if (method == "HEAD") {
            curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        } else if (method != "GET") {
            curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        if (timeout_ms_ > 0) {
            curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms_);
        }
        if (!ca_.empty())   curl_easy_setopt(c, CURLOPT_CAINFO,  ca_.c_str());
        if (!cert_.empty()) curl_easy_setopt(c, CURLOPT_SSLCERT, cert_.c_str());
        if (!key_.empty())  curl_easy_setopt(c, CURLOPT_SSLKEY,  key_.c_str());

        struct curl_slist* hdrs = nullptr;
        for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
        if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

        const CURLcode rc = curl_easy_perform(c);
        if (rc != CURLE_OK) {
            res.transport_err = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        } else {
            long status = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
            res.status = status;
            res.body   = std::move(resp);
        }
        if (hdrs) curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return res;
    }

    // The TLS/timeout knobs are per-tier, set once by RestColdTier::Create.
    long        timeout_ms_ = 30000;
    std::string ca_, cert_, key_;
};

}  // namespace

std::shared_ptr<IHttpTransport> MakeCurlHttpTransport() {
    return std::make_shared<CurlHttpTransport>();
}

// ---------------------------------------------------------------------------
// RestColdTier
// ---------------------------------------------------------------------------

std::unique_ptr<RestColdTier> RestColdTier::Create(const Options& opts,
                                                   std::string*   err) {
    auto t = std::make_shared<CurlHttpTransport>();
    t->timeout_ms_ = opts.timeout_ms;
    t->ca_   = opts.ca_pem_path;
    t->cert_ = opts.client_cert_pem_path;
    t->key_  = opts.client_key_pem_path;
    return CreateWithTransport(opts, std::move(t), err);
}

std::unique_ptr<RestColdTier> RestColdTier::CreateWithTransport(
    const Options& opts, std::shared_ptr<IHttpTransport> http,
    std::string* err) {
    if (opts.base_url.empty()) {
        if (err) *err = "cold_tier/native-rest: base_url is empty";
        return nullptr;
    }
    if (!http) {
        if (err) *err = "cold_tier/native-rest: null transport";
        return nullptr;
    }
    auto t = std::unique_ptr<RestColdTier>(new RestColdTier());
    t->opts_ = opts;
    t->opts_.base_url = TrimTrailingSlash(opts.base_url);
    t->http_ = std::move(http);
    return t;
}

std::string RestColdTier::ObjectKeyFor(const DramKey& key) const {
    const std::string hex = HexOf(key);
    return opts_.key_prefix + hex.substr(0, 2) + "/" + hex.substr(2) + ".kv";
}

std::string RestColdTier::UrlFor(const DramKey& key) const {
    return opts_.base_url + "/" + ObjectKeyFor(key);
}

std::vector<std::string> RestColdTier::Headers() const {
    std::vector<std::string> h;
    if (!opts_.bearer_token.empty())
        h.push_back("Authorization: Bearer " + opts_.bearer_token);
    return h;
}

bool RestColdTier::Put(const DramKey& key, const uint8_t* data, std::size_t n,
                       std::string* err) {
    auto h = Headers();
    h.push_back("Content-Type: application/octet-stream");
    const HttpResult r = http_->Request("PUT", UrlFor(key), h, data, n);
    if (!r.transport_err.empty()) {
        if (err) *err = "native-rest PUT: " + r.transport_err;
        return false;
    }
    if (r.status < 200 || r.status >= 300) {
        if (err) *err = "native-rest PUT status " + std::to_string(r.status) +
                        ": " + r.body;
        return false;
    }
    return true;
}

bool RestColdTier::Get(const DramKey& key, std::vector<uint8_t>* out,
                       std::string* err) {
    const HttpResult r = http_->Request("GET", UrlFor(key), Headers(), nullptr, 0);
    if (!r.transport_err.empty()) {
        if (err) *err = "native-rest GET: " + r.transport_err;
        return false;
    }
    if (r.status == 404) {
        if (err) err->clear();  // not-found is not an error condition
        return false;
    }
    if (r.status < 200 || r.status >= 300) {
        if (err) *err = "native-rest GET status " + std::to_string(r.status) +
                        ": " + r.body;
        return false;
    }
    if (out) out->assign(r.body.begin(), r.body.end());
    return true;
}

bool RestColdTier::Delete(const DramKey& key, std::string* err) {
    const HttpResult r = http_->Request("DELETE", UrlFor(key), Headers(), nullptr, 0);
    if (!r.transport_err.empty()) {
        if (err) *err = "native-rest DELETE: " + r.transport_err;
        return false;
    }
    // 404 == already gone; treat as success (idempotent delete).
    if (r.status == 404 || (r.status >= 200 && r.status < 300)) return true;
    if (err) *err = "native-rest DELETE status " + std::to_string(r.status) +
                    ": " + r.body;
    return false;
}

bool RestColdTier::Exists(const DramKey& key) const {
    const HttpResult r = http_->Request("HEAD", UrlFor(key), Headers(), nullptr, 0);
    return r.transport_err.empty() && r.status >= 200 && r.status < 300;
}

}  // namespace kvcache::node::tier
