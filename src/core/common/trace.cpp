// Distributed-tracing facade — implementation.
//
// The implementation is deliberately self-contained: no external SDK
// dependency. Production users who want OTLP-HTTP / Jaeger / Zipkin
// install a custom Exporter; the rest of the code base is oblivious.
#include "trace.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace kvcache::trace {

namespace {

// Process-local 128-bit / 64-bit ID generation. We use a single shared
// std::mt19937_64 instance under a spin-flag for thread safety —
// startSpan is on the cold path of any real workload (lookup vs metric
// inc), so a contended atomic is acceptable. If profiling ever flags
// this we can switch to a per-thread RNG seeded from std::random_device.
std::mutex&         RngMu()  { static std::mutex m; return m; }
std::mt19937_64&    Rng()    { static std::mt19937_64 r(std::random_device{}()); return r; }

uint64_t RandU64() {
    std::lock_guard<std::mutex> lk(RngMu());
    return Rng()();
}

void FillRandomBytes(uint8_t* out, std::size_t n) {
    while (n > 0) {
        uint64_t v = RandU64();
        std::size_t take = n < sizeof(v) ? n : sizeof(v);
        std::memcpy(out, &v, take);
        out += take;
        n   -= take;
    }
}

SpanContext NewSpanContext(const SpanContext& parent) {
    SpanContext c;
    if (parent.IsValid()) {
        c.trace_id = parent.trace_id;
    } else {
        FillRandomBytes(c.trace_id.data(), c.trace_id.size());
    }
    FillRandomBytes(c.span_id.data(), c.span_id.size());
    return c;
}

// Thread-local active-span stack: each StartSpan pushes the new context
// at construction; End() pops it. The Tracer reads `.back()` to find
// the parent at StartSpan time.
thread_local std::vector<SpanContext> g_active_stack;

void PushActive(const SpanContext& ctx)  { g_active_stack.push_back(ctx); }
void PopActive (const SpanContext& ctx) {
    for (auto it = g_active_stack.rbegin(); it != g_active_stack.rend(); ++it) {
        if (it->span_id == ctx.span_id) {
            g_active_stack.erase(std::next(it).base());
            return;
        }
    }
    // If the caller End()s out of order we silently ignore — debugging
    // a stuck stack is worse than a missing pop.
}

class NoopExporter final : public Exporter {
   public:
    void Export(const SpanData&) override {}
};

std::shared_ptr<Exporter> EnsureExporter(const std::shared_ptr<Exporter>& e) {
    if (e) return e;
    static std::shared_ptr<Exporter> noop = std::make_shared<NoopExporter>();
    return noop;
}

void HexEncode(const uint8_t* in, std::size_t n, std::string* out) {
    static constexpr char kHex[] = "0123456789abcdef";
    out->resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        (*out)[2 * i]     = kHex[(in[i] >> 4) & 0xF];
        (*out)[2 * i + 1] = kHex[(in[i]) & 0xF];
    }
}

void JsonEscape(std::string_view in, std::ostringstream& out) {
    for (char c : in) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out << c;
                }
        }
    }
}

}  // namespace

// ---- Span ----------------------------------------------------------------

struct Span::Pending {
    SpanData                 data;
    std::shared_ptr<Exporter> exporter;
    bool                     ended = false;
};

Span::Span(Span&& other) noexcept : data_(std::move(other.data_)) {}

Span& Span::operator=(Span&& other) noexcept {
    if (this != &other) {
        End();
        data_ = std::move(other.data_);
    }
    return *this;
}

Span::~Span() { End(); }

SpanContext Span::Context() const noexcept {
    return data_ ? data_->data.context : SpanContext{};
}

void Span::SetAttribute(std::string_view key, std::string_view value) {
    if (!data_ || data_->ended) return;
    data_->data.attributes.push_back({std::string(key), std::string(value)});
}

void Span::SetAttribute(std::string_view key, int64_t value) {
    if (!data_ || data_->ended) return;
    data_->data.attributes.push_back({std::string(key), value});
}

void Span::SetAttribute(std::string_view key, double value) {
    if (!data_ || data_->ended) return;
    data_->data.attributes.push_back({std::string(key), value});
}

void Span::SetAttribute(std::string_view key, bool value) {
    if (!data_ || data_->ended) return;
    data_->data.attributes.push_back({std::string(key), value});
}

void Span::SetError(std::string_view message) {
    if (!data_ || data_->ended) return;
    data_->data.status         = StatusCode::Error;
    data_->data.status_message = std::string(message);
}

void Span::End() noexcept {
    if (!data_ || data_->ended) return;
    data_->ended    = true;
    data_->data.end = Clock::now();
    if (data_->data.status == StatusCode::Unset) {
        data_->data.status = StatusCode::Ok;
    }
    PopActive(data_->data.context);
    if (data_->exporter) {
        try { data_->exporter->Export(data_->data); }
        catch (...) { /* exporters must never throw past this point */ }
    }
}

// ---- Tracer --------------------------------------------------------------

Tracer& Tracer::Get() {
    static Tracer t;
    return t;
}

Span Tracer::StartSpan(std::string_view name) {
    Span s;
    s.data_ = std::make_unique<Span::Pending>();
    s.data_->data.parent  = ActiveContext();
    s.data_->data.context = NewSpanContext(s.data_->data.parent);
    s.data_->data.name    = std::string(name);
    s.data_->data.start   = Clock::now();
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.data_->exporter = EnsureExporter(exporter_);
    }
    PushActive(s.data_->data.context);
    return s;
}

void Tracer::SetExporter(std::shared_ptr<Exporter> exporter) {
    std::lock_guard<std::mutex> lk(mu_);
    exporter_ = std::move(exporter);
}

std::shared_ptr<Exporter> Tracer::exporter() const {
    std::lock_guard<std::mutex> lk(mu_);
    return EnsureExporter(exporter_);
}

SpanContext Tracer::ActiveContext() {
    if (g_active_stack.empty()) return SpanContext{};
    return g_active_stack.back();
}

// ---- InMemoryExporter ----------------------------------------------------

void InMemoryExporter::Export(const SpanData& span) {
    std::lock_guard<std::mutex> lk(mu_);
    spans_.push_back(span);
}

std::vector<SpanData> InMemoryExporter::spans() const {
    std::lock_guard<std::mutex> lk(mu_);
    return spans_;
}

std::size_t InMemoryExporter::Size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return spans_.size();
}

void InMemoryExporter::Clear() {
    std::lock_guard<std::mutex> lk(mu_);
    spans_.clear();
}

// ---- JsonStderrExporter --------------------------------------------------

void JsonStderrExporter::Export(const SpanData& span) {
    std::ostringstream out;
    std::string trace_hex, span_hex, parent_hex;
    HexEncode(span.context.trace_id.data(), span.context.trace_id.size(), &trace_hex);
    HexEncode(span.context.span_id.data(),  span.context.span_id.size(),  &span_hex);
    if (span.parent.IsValid()) {
        HexEncode(span.parent.span_id.data(), span.parent.span_id.size(), &parent_hex);
    }
    const auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        span.end - span.start).count();

    out << "{\"trace_id\":\"" << trace_hex
        << "\",\"span_id\":\""  << span_hex << "\"";
    if (!parent_hex.empty()) out << ",\"parent\":\"" << parent_hex << "\"";
    out << ",\"name\":\"";
    JsonEscape(span.name, out);
    out << "\",\"duration_ns\":" << dur_ns;
    out << ",\"status\":\""
        << (span.status == StatusCode::Error ? "ERROR" :
            span.status == StatusCode::Ok    ? "OK"    : "UNSET")
        << "\"";
    if (!span.status_message.empty()) {
        out << ",\"status_message\":\"";
        JsonEscape(span.status_message, out);
        out << "\"";
    }
    if (!span.attributes.empty()) {
        out << ",\"attrs\":{";
        bool first = true;
        for (const auto& a : span.attributes) {
            if (!first) out << ",";
            first = false;
            out << "\"";
            JsonEscape(a.key, out);
            out << "\":";
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    out << "\"";
                    JsonEscape(v, out);
                    out << "\"";
                } else if constexpr (std::is_same_v<T, bool>) {
                    out << (v ? "true" : "false");
                } else {
                    out << v;
                }
            }, a.value);
        }
        out << "}";
    }
    out << "}\n";
    const auto s = out.str();
    std::fwrite(s.data(), 1, s.size(), stderr);
}

// ---- env install ---------------------------------------------------------

void InstallFromEnv() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = std::getenv("KVCACHE_TRACE_JSON");
        if (!v) return;
        if (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0) {
            Tracer::Get().SetExporter(std::make_shared<JsonStderrExporter>());
        }
    });
}

}  // namespace kvcache::trace
