// Logging facade implementation.
//
// Current backend: stderr passthrough. spdlog wiring lands when the dependency
// is vendored — at that point this file becomes a thin adapter and the header
// surface stays unchanged.
#include "logging.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kvcache::log {

namespace {

Level g_level = Level::Info;
std::mutex g_mu;
std::unordered_map<std::string, Logger> g_loggers;

const char* LevelName(Level l) {
    switch (l) {
        case Level::Trace:    return "trace";
        case Level::Debug:    return "debug";
        case Level::Info:     return "info";
        case Level::Warn:     return "warn";
        case Level::Error:    return "error";
        case Level::Critical: return "critical";
        case Level::Off:      return "off";
    }
    return "?";
}

}  // namespace

void Init(const InitOptions& opts) {
    std::lock_guard lk(g_mu);
    g_level = opts.level;
    // TODO(stephen): swap stderr sink for spdlog async JSON sink.
}

void Shutdown() {
    std::lock_guard lk(g_mu);
    g_loggers.clear();
}

Logger& Get(std::string_view subsystem) {
    std::lock_guard lk(g_mu);
    auto [it, _] = g_loggers.try_emplace(std::string{subsystem});
    return it->second;
}

bool Logger::ShouldLog(Level level) const noexcept {
    return static_cast<int>(level) >= static_cast<int>(g_level);
}

void Logger::Log(Level level, std::string_view msg) noexcept {
    if (!ShouldLog(level)) return;
    // Minimal placeholder. Real impl will route to spdlog with a JSON formatter.
    std::fprintf(stderr, "[%s] %.*s\n", LevelName(level),
                 static_cast<int>(msg.size()), msg.data());
}

void Logger::Trace(std::string_view m) noexcept { Log(Level::Trace,    m); }
void Logger::Debug(std::string_view m) noexcept { Log(Level::Debug,    m); }
void Logger::Info (std::string_view m) noexcept { Log(Level::Info,     m); }
void Logger::Warn (std::string_view m) noexcept { Log(Level::Warn,     m); }
void Logger::Error(std::string_view m) noexcept { Log(Level::Error,    m); }
void Logger::Crit (std::string_view m) noexcept { Log(Level::Critical, m); }

}  // namespace kvcache::log
