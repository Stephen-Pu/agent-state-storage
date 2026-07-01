// LLD §3.3 T4 — SigV4 signing decorator implementation (Phase B5).
#include "tier/sigv4_transport.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#if KVCACHE_HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace kvcache::node::tier {

namespace {

#if KVCACHE_HAVE_OPENSSL

std::string ToHex(const unsigned char* p, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0x0F]);
    }
    return s;
}

std::vector<unsigned char> HmacSha256(const unsigned char* key,
                                      std::size_t key_len,
                                      const unsigned char* data,
                                      std::size_t data_len) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(), key, static_cast<int>(key_len), data, data_len, out,
         &out_len);
    return std::vector<unsigned char>(out, out + out_len);
}

std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    const auto b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// RFC 3986 percent-encoding. Unreserved (A-Za-z0-9-._~) pass through; '/' is
// preserved when `keep_slash` (canonical URI path segments), encoded otherwise
// (query components).
std::string UriEncode(const std::string& s, bool keep_slash) {
    static const char* kHex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            o.push_back(static_cast<char>(c));
        } else if (c == '/' && keep_slash) {
            o.push_back('/');
        } else {
            o.push_back('%');
            o.push_back(kHex[c >> 4]);
            o.push_back(kHex[c & 0x0F]);
        }
    }
    return o;
}

struct ParsedUrl {
    std::string host;   // host[:port] — the value signed as the Host header
    std::string path;   // raw path, "/" if none
    std::string query;  // raw query (after '?'), empty if none
};

ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl p;
    const auto scheme = url.find("://");
    const std::size_t hstart = (scheme == std::string::npos) ? 0 : scheme + 3;
    const std::size_t pstart = url.find('/', hstart);
    if (pstart == std::string::npos) {
        p.host = url.substr(hstart);
        p.path = "/";
        return p;
    }
    p.host = url.substr(hstart, pstart - hstart);
    const std::string rest = url.substr(pstart);
    const auto q = rest.find('?');
    if (q == std::string::npos) {
        p.path = rest;
    } else {
        p.path = rest.substr(0, q);
        p.query = rest.substr(q + 1);
    }
    if (p.path.empty()) p.path = "/";
    return p;
}

std::string CanonicalQuery(const std::string& query) {
    if (query.empty()) return "";
    std::vector<std::pair<std::string, std::string>> kv;
    std::size_t i = 0;
    while (i <= query.size()) {
        const auto amp = query.find('&', i);
        const std::string tok = query.substr(
            i, amp == std::string::npos ? std::string::npos : amp - i);
        if (!tok.empty()) {
            const auto eq = tok.find('=');
            const std::string k = (eq == std::string::npos) ? tok : tok.substr(0, eq);
            const std::string v = (eq == std::string::npos) ? "" : tok.substr(eq + 1);
            kv.emplace_back(UriEncode(k, false), UriEncode(v, false));
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    std::sort(kv.begin(), kv.end());
    std::string o;
    for (const auto& p : kv) {
        if (!o.empty()) o += "&";
        o += p.first + "=" + p.second;
    }
    return o;
}

std::string UtcNowAmzDate() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return std::string(buf);
}

// The signing decorator.
class SigV4Transport final : public IHttpTransport {
   public:
    SigV4Transport(std::shared_ptr<IHttpTransport> inner, SigV4Options opts)
        : inner_(std::move(inner)), opts_(std::move(opts)) {}

    HttpResult Request(const std::string& method, const std::string& url,
                       const std::vector<std::string>& headers,
                       const uint8_t* body, std::size_t n) override {
        HttpResult r;
        const std::string amz =
            opts_.amz_date_fn ? opts_.amz_date_fn() : UtcNowAmzDate();
        const ParsedUrl u = ParseUrl(url);

        static const uint8_t kEmpty = 0;
        const std::string payload_hash =
            Sha256Hex(body ? body : &kEmpty, body ? n : 0);

        // Signed header set (S3 style): host + payload hash + date [+ token].
        std::vector<std::pair<std::string, std::string>> sh;
        sh.emplace_back("host", u.host);
        sh.emplace_back("x-amz-content-sha256", payload_hash);
        sh.emplace_back("x-amz-date", amz);
        if (!opts_.session_token.empty())
            sh.emplace_back("x-amz-security-token", opts_.session_token);
        std::sort(sh.begin(), sh.end());

        const std::string auth = ComputeSigV4Authorization(
            method, UriEncode(u.path, /*keep_slash=*/true),
            CanonicalQuery(u.query), sh, payload_hash, opts_.access_key,
            opts_.secret_key, opts_.region, opts_.service, amz);
        if (auth.empty()) {
            r.transport_err = "sigv4: signing failed";
            return r;
        }

        std::vector<std::string> out = headers;
        out.push_back("x-amz-date: " + amz);
        out.push_back("x-amz-content-sha256: " + payload_hash);
        if (!opts_.session_token.empty())
            out.push_back("x-amz-security-token: " + opts_.session_token);
        out.push_back("Authorization: " + auth);
        return inner_->Request(method, url, out, body, n);
    }

   private:
    std::shared_ptr<IHttpTransport> inner_;
    SigV4Options                    opts_;
};

#endif  // KVCACHE_HAVE_OPENSSL

}  // namespace

std::string Sha256Hex(const uint8_t* data, std::size_t n) {
#if !KVCACHE_HAVE_OPENSSL
    (void)data;
    (void)n;
    return {};
#else
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_Digest(data, n, md, &md_len, EVP_sha256(), nullptr);
    return ToHex(md, md_len);
#endif
}

std::string ComputeSigV4Authorization(
    const std::string& method, const std::string& canonical_uri,
    const std::string& canonical_query,
    const std::vector<std::pair<std::string, std::string>>& signed_headers,
    const std::string& payload_hash_hex, const std::string& access_key,
    const std::string& secret_key, const std::string& region,
    const std::string& service, const std::string& amz_date) {
#if !KVCACHE_HAVE_OPENSSL
    (void)method; (void)canonical_uri; (void)canonical_query;
    (void)signed_headers; (void)payload_hash_hex; (void)access_key;
    (void)secret_key; (void)region; (void)service; (void)amz_date;
    return {};
#else
    std::string canonical_headers;
    std::string signed_list;
    for (const auto& [name, value] : signed_headers) {
        canonical_headers += name + ":" + Trim(value) + "\n";
        if (!signed_list.empty()) signed_list += ";";
        signed_list += name;
    }

    const std::string canonical_request =
        method + "\n" + canonical_uri + "\n" + canonical_query + "\n" +
        canonical_headers + "\n" + signed_list + "\n" + payload_hash_hex;

    const std::string datestamp = amz_date.substr(0, 8);
    const std::string scope =
        datestamp + "/" + region + "/" + service + "/aws4_request";
    const std::string cr_hash = Sha256Hex(
        reinterpret_cast<const uint8_t*>(canonical_request.data()),
        canonical_request.size());
    const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") +
                                        amz_date + "\n" + scope + "\n" + cr_hash;

    const std::string k0 = "AWS4" + secret_key;
    const auto k_date = HmacSha256(
        reinterpret_cast<const unsigned char*>(k0.data()), k0.size(),
        reinterpret_cast<const unsigned char*>(datestamp.data()),
        datestamp.size());
    const auto k_region = HmacSha256(
        k_date.data(), k_date.size(),
        reinterpret_cast<const unsigned char*>(region.data()), region.size());
    const auto k_service = HmacSha256(
        k_region.data(), k_region.size(),
        reinterpret_cast<const unsigned char*>(service.data()), service.size());
    static const std::string kTerm = "aws4_request";
    const auto k_signing = HmacSha256(
        k_service.data(), k_service.size(),
        reinterpret_cast<const unsigned char*>(kTerm.data()), kTerm.size());
    const auto sig = HmacSha256(
        k_signing.data(), k_signing.size(),
        reinterpret_cast<const unsigned char*>(string_to_sign.data()),
        string_to_sign.size());

    return "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
           ", SignedHeaders=" + signed_list + ", Signature=" +
           ToHex(sig.data(), sig.size());
#endif
}

std::shared_ptr<IHttpTransport> MakeSigV4Transport(
    std::shared_ptr<IHttpTransport> inner, const SigV4Options& opts,
    std::string* err) {
#if !KVCACHE_HAVE_OPENSSL
    (void)inner;
    (void)opts;
    if (err) *err = "sigv4: OpenSSL not compiled in (KVCACHE_HAVE_OPENSSL undefined)";
    return nullptr;
#else
    if (!inner) {
        if (err) *err = "sigv4: inner transport is null";
        return nullptr;
    }
    if (opts.access_key.empty() || opts.secret_key.empty() ||
        opts.region.empty()) {
        if (err) *err = "sigv4: access_key, secret_key and region are required";
        return nullptr;
    }
    return std::make_shared<SigV4Transport>(std::move(inner), opts);
#endif
}

}  // namespace kvcache::node::tier
