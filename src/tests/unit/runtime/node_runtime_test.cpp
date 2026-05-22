// Phase L-1 — NodeRuntime gtest coverage.
//
// We bind both listeners on ephemeral ports (port=0) so multiple
// concurrent tests don't collide on the production constants. The
// readiness probe is exercised by opening a raw TCP socket to the
// grpc port and verifying connect()+close() works; the metrics
// endpoint is exercised by sending an HTTP/1.1 GET and parsing the
// response head + body.
#include "runtime/node_runtime.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "metrics.h"

using kvcache::node::runtime::NodeRuntime;

namespace {

// Sync TCP connect on 127.0.0.1:port. Returns the connected fd or
// -1 if the dial failed.
int Dial(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Send `body` then read until EOF. Returns the raw HTTP response (head
// + body) as a single string. Empty on transport error.
std::string HttpExchange(uint16_t port, const std::string& path) {
    int fd = Dial(port);
    if (fd < 0) return {};
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) != static_cast<ssize_t>(req.size())) {
        ::close(fd);
        return {};
    }
    std::string out;
    char buf[2048];
    while (true) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        out.append(buf, static_cast<std::size_t>(r));
    }
    ::close(fd);
    return out;
}

// Split HTTP head + body on the first CRLF CRLF.
std::pair<std::string, std::string> SplitHttp(const std::string& resp) {
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return {resp, {}};
    return {resp.substr(0, pos), resp.substr(pos + 4)};
}

NodeRuntime::Options EphemeralOpts() {
    NodeRuntime::Options o;
    o.bind_host    = "127.0.0.1";
    o.grpc_port    = 0;
    o.metrics_port = 0;
    return o;
}

}  // namespace

TEST(NodeRuntimeTest, BindsOnEphemeralPortsAndReportsThemBack) {
    NodeRuntime rt(EphemeralOpts());
    ASSERT_TRUE(rt.Ok()) << rt.error();
    EXPECT_GT(rt.GrpcPort(), 0u);
    EXPECT_GT(rt.MetricsPort(), 0u);
    EXPECT_NE(rt.GrpcPort(), rt.MetricsPort());
}

TEST(NodeRuntimeTest, GrpcReadinessProbeAcceptsConnection) {
    NodeRuntime rt(EphemeralOpts());
    ASSERT_TRUE(rt.Ok());
    rt.Start();

    int fd = Dial(rt.GrpcPort());
    ASSERT_GE(fd, 0) << "expected the readiness-probe TCP listener to accept";
    ::close(fd);
}

TEST(NodeRuntimeTest, MetricsEndpointReturnsPrometheusBody) {
    // Seed a counter so the scrape body has something interesting.
    auto& reg = kvcache::metrics::Registry::Default();
    static constexpr std::string_view kKeys[] = {"phase"};
    auto& c = reg.GetOrCreateCounter("kv_l1_probe_total",
                                       "L-1 smoke test counter.", kKeys);
    const kvcache::metrics::Label lbl[] = {{"phase", "L-1"}};
    c.Inc(1.0, lbl);

    NodeRuntime rt(EphemeralOpts());
    ASSERT_TRUE(rt.Ok());
    rt.Start();

    const auto resp = HttpExchange(rt.MetricsPort(), "/metrics");
    ASSERT_FALSE(resp.empty()) << "no response from /metrics";
    auto [head, body] = SplitHttp(resp);
    EXPECT_NE(head.find("HTTP/1.1 200"), std::string::npos) << head;
    EXPECT_NE(head.find("text/plain"),   std::string::npos) << head;
    EXPECT_NE(body.find("kv_l1_probe_total"), std::string::npos) << body;
}

TEST(NodeRuntimeTest, HealthzEndpointReturnsOk) {
    NodeRuntime rt(EphemeralOpts());
    ASSERT_TRUE(rt.Ok());
    rt.Start();

    const auto resp = HttpExchange(rt.MetricsPort(), "/healthz");
    ASSERT_FALSE(resp.empty());
    auto [head, body] = SplitHttp(resp);
    EXPECT_NE(head.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_EQ(body, "ok\n");
}

TEST(NodeRuntimeTest, UnknownPathReturns404) {
    NodeRuntime rt(EphemeralOpts());
    ASSERT_TRUE(rt.Ok());
    rt.Start();

    const auto resp = HttpExchange(rt.MetricsPort(), "/no-such-thing");
    ASSERT_FALSE(resp.empty());
    auto [head, body] = SplitHttp(resp);
    EXPECT_NE(head.find("HTTP/1.1 404"), std::string::npos);
}

TEST(NodeRuntimeTest, StopUnblocksWait) {
    NodeRuntime rt(EphemeralOpts());
    ASSERT_TRUE(rt.Ok());
    rt.Start();

    std::thread w([&] {
        const int sig = rt.Wait();
        // signal=0 because we called Stop() explicitly (no signal arrived).
        EXPECT_EQ(sig, 0);
    });
    // Give Wait() a moment to actually park on the cv before stopping.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rt.Stop();
    w.join();
}

TEST(NodeRuntimeTest, PortInUseSurfacesAsError) {
    // Bind the first runtime on a real port, then attempt a second
    // runtime on the same port — should fail at construction.
    NodeRuntime rt1(EphemeralOpts());
    ASSERT_TRUE(rt1.Ok());
    const uint16_t taken = rt1.GrpcPort();

    NodeRuntime::Options o = EphemeralOpts();
    o.grpc_port = taken;
    // SO_REUSEPORT on macOS lets multiple processes share a port,
    // so this test only asserts the SAME-PROCESS case fails — both
    // sockets in one process can't carry the same SO_REUSEPORT
    // claim simultaneously. If your platform allows it, skip.
    NodeRuntime rt2(o);
    if (rt2.Ok()) {
        GTEST_SKIP() << "platform allows port sharing within one process";
    }
    EXPECT_FALSE(rt2.error().empty());
}
