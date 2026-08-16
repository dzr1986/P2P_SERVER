// cfg_file_test：学 EasyTier 配置覆盖（文件 < P2P_* < 命令行）
#include "server/config/CfgFile.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace p2p;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) printf("  ok: %s\n", msg);                \
        else { printf("  FAIL: %s\n", msg); g_fail++; }     \
    } while (0)

static uint32_t ip4_be(const char* s) {
    in_addr a{};
    inet_pton(AF_INET, s, &a);
    return a.s_addr;
}

static void unset_p2p_env() {
    unsetenv("P2P_DISABLE_ENV_PARSING");
    unsetenv("P2P_AUTH_SECRET");
    unsetenv("P2P_ENABLE_AUTH");
    unsetenv("P2P_INSTANCE_NAME");
    unsetenv("P2P_STATUS_PORT");
    unsetenv("P2P_STATUS_ALLOW");
    unsetenv("CFG_TEST_SECRET");
}

int main() {
    unset_p2p_env();

    CHECK(expand_cfg_env("plain", false) == "plain", "expand no token");
    setenv("CFG_TEST_SECRET", "s3cr3t", 1);
    CHECK(expand_cfg_env("pre-${CFG_TEST_SECRET}-post", false) == "pre-s3cr3t-post",
          "expand ${ENV}");
    CHECK(expand_cfg_env("pre-${CFG_TEST_SECRET}-post", true) == "pre-${CFG_TEST_SECRET}-post",
          "disable-env-parsing keeps token");
    CHECK(expand_cfg_env("x-${CFG_TEST_MISSING}-y", false) == "x-${CFG_TEST_MISSING}-y",
          "undefined ${ENV} kept");

    CHECK(ipv4_in_allow_list(ip4_be("8.8.8.8"), {}), "empty allow = pass");
    CHECK(ipv4_in_allow_list(ip4_be("127.0.0.1"), {"127.0.0.1"}), "/32 implicit");
    CHECK(ipv4_in_allow_list(ip4_be("127.0.0.1"), {"127.0.0.1/32"}), "/32 explicit");
    CHECK(ipv4_in_allow_list(ip4_be("10.1.2.3"), {"10.0.0.0/8"}), "/8 match");
    CHECK(!ipv4_in_allow_list(ip4_be("8.8.8.8"), {"10.0.0.0/8"}), "/8 miss");
    CHECK(ipv4_in_allow_list(ip4_be("192.168.1.9"), {"127.0.0.1", "192.168.1.0/24"}),
          "list second rule");

    CfgData cfg;
    cfg.instance_name = "from-file";
    cfg.status_port = 1;
    cfg.enable_auth = false;
    setenv("P2P_INSTANCE_NAME", "from-env", 1);
    setenv("P2P_STATUS_PORT", "16010", 1);
    setenv("P2P_ENABLE_AUTH", "1", 1);
    setenv("P2P_AUTH_SECRET", "env-secret", 1);
    apply_cfg_env(cfg);
    CHECK(cfg.instance_name == "from-env", "env overrides instance");
    CHECK(cfg.status_port == 16010, "env overrides status_port");
    CHECK(cfg.enable_auth, "env enables auth");
    CHECK(cfg.auth_secret == "env-secret", "env overrides secret");
    unset_p2p_env();

    CfgData keep;
    keep.instance_name = "keep-me";
    apply_cfg_env(keep);
    CHECK(keep.instance_name == "keep-me", "unset env does not clobber");

    const char* path = "/tmp/p2p_cfg_file_test.cfg";
    FILE* fp = fopen(path, "w");
    CHECK(fp != nullptr, "write temp cfg");
    if (fp) {
        fputs("NatServer1=127.0.0.1\n", fp);
        fputs("Proxy1_1=127.0.0.1\n", fp);
        fputs("InstanceName=file-inst\n", fp);
        fputs("StatusPort=16011\n", fp);
        fputs("StatusAllow=127.0.0.1,10.0.0.0/8\n", fp);
        fputs("AuthSecret=${CFG_TEST_SECRET}\n", fp);
        fclose(fp);
    }
    setenv("CFG_TEST_SECRET", "expanded", 1);
    CfgData loaded;
    CHECK(load_server_set(loaded, path), "load_server_set");
    CHECK(loaded.instance_name == "file-inst", "file InstanceName");
    CHECK(loaded.status_port == 16011, "file StatusPort");
    CHECK(loaded.status_allow.size() == 2, "file StatusAllow count");
    CHECK(loaded.auth_secret == "expanded", "file ${ENV} expand");
    unset_p2p_env();
    unlink(path);

    if (g_fail) {
        printf("cfg_file_test FAIL %d\n", g_fail);
        return 1;
    }
    printf("cfg_file_test OK\n");
    return 0;
}
