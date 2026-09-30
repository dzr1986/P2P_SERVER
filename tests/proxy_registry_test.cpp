// proxy_registry_test.cpp：注册双索引、容量、地址迁移与单调时钟租约回归
#include "server/proxyserver/src/ProxyRegistry.h"

#include <cstdio>
#include <cstdlib>

using namespace p2p;
using namespace std::chrono_literals;

static void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static sockaddr_in address(uint16_t port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    result.sin_port = htons(port);
    return result;
}

int main() {
    ProxyRegistry registry;
    const auto now = ProxyRegistry::TimePoint{};
    auto a = address(10001), b = address(10002), c = address(10003);
    sockaddr_in target{};

    check(!registry.register_peer("", a, 2, now), "empty UUID rejected");
    check(!registry.register_peer(std::string(33, 'x'), a, 2, now), "long UUID rejected");
    check(!registry.register_peer("A", a, 0, now), "zero capacity rejects registration");
    check(registry.register_peer("A", a, 2, now), "register A");
    check(registry.register_peer("B", b, 2, now), "register B");
    check(!registry.register_peer("C", c, 2, now), "capacity bounds new registrations");
    check(!registry.resolve("C", c, "B", target, now), "unregistered source rejected");
    check(!registry.resolve("A", c, "B", target, now), "wrong source address rejected");
    check(registry.resolve("A", a, "B", target, now) && sockaddr_eq(target, b),
          "registered source resolves target");

    check(registry.register_peer("A", a, 2, now + 1s), "refresh allowed at capacity");
    check(registry.register_peer("A", c, 2, now + 2s), "UUID moves to new endpoint");
    check(!registry.register_peer("C", a, 2, now + 2s),
          "old endpoint cannot evict migrated UUID through stale reverse index");
    check(!registry.unregister_peer("A", a), "old endpoint cannot unregister migrated UUID");
    check(!registry.resolve("A", a, "B", target, now + 2s), "old endpoint cannot relay");
    check(registry.resolve("A", c, "B", target, now + 2s), "new endpoint can relay");
    check(registry.register_peer("C", c, 2, now + 3s), "same address replaces UUID at capacity");
    check(!registry.registered("A", c, now + 3s), "replaced UUID removed");
    check(registry.size() == 2, "replacement does not consume capacity");

    check(registry.register_peer("C", b, 2, now + 4s), "move onto occupied endpoint");
    check(registry.size() == 1 && !registry.registered("B", b, now + 4s),
          "occupied endpoint's previous UUID removed");
    check(registry.register_peer("A", c, 2, now + 4s), "freed endpoint can be reused");
    check(registry.registered("C", b, now + 4s), "reuse does not remove new owner");
    check(!registry.unregister_peer("C", c), "wrong endpoint cannot unregister");
    check(registry.unregister_peer("C", b), "registered owner unregisters");
    check(registry.register_peer("B", b, 2, now + 4s), "unregistered endpoint reused");
    check(registry.register_peer("A", c, 2, now + 60s), "refresh extends only A's lease");

    check(registry.resolve("A", c, "B", target, now + 123s), "target live before lease deadline");
    check(!registry.resolve("A", c, "B", target, now + 124s),
          "expired target rejected before periodic cleanup");
    check(!registry.resolve("B", b, "A", target, now + 124s),
          "expired source rejected before periodic cleanup");
    check(registry.register_peer("C", a, 2, now + 124s),
          "full registry reclaims expired capacity before timer runs");
    registry.expire(now + 179s);
    check(registry.size() == 2 && registry.registered("A", c, now + 179s),
          "cleanup preserves refreshed lease");
    registry.expire(now + 180s);
    check(registry.size() == 1, "lease expires exactly at its deadline");
    check(registry.register_peer("D", c, 2, now + 180s), "expired endpoint reused");
    registry.expire(now + 244s);
    check(registry.registered("D", c, now + 244s), "cleanup leaves new owner intact");
    check(registry.unregister_peer("D", c) && registry.size() == 0, "all entries reclaimed");

    for (uint16_t i = 1; i <= 10000; ++i) {
        check(registry.register_peer("A", address(i), 1, now), "repeated address migration");
        check(registry.size() == 1, "migration remains bounded");
    }
    check(!registry.register_peer("B", address(1), 1, now), "no stale endpoint after churn");
    std::printf("proxy registry tests PASS (capacity, migration, source binding, expiry)\n");
    return 0;
}
