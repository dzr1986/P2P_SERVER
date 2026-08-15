#ifndef P2P_COMMON_TLS_IO_H
#define P2P_COMMON_TLS_IO_H

// OpenSSL 薄封装：DERP 式 TCP/TLS 中继面（Proxy 服务端 + 客户端）
//   - 有证书文件则加载；否则服务端可生成临时自签（测试/内网）
//   - 客户端 insecure=true 时不校验对端证书（自签联调）

#include <cstddef>
#include <mutex>
#include <string>

#include <openssl/ssl.h>

namespace p2p {

struct TlsCtx {
    SSL_CTX* ctx = nullptr;
    ~TlsCtx() { reset(); }
    TlsCtx() = default;
    TlsCtx(const TlsCtx&) = delete;
    TlsCtx& operator=(const TlsCtx&) = delete;
    TlsCtx(TlsCtx&& o) noexcept : ctx(o.ctx) { o.ctx = nullptr; }
    TlsCtx& operator=(TlsCtx&& o) noexcept {
        if (this != &o) { reset(); ctx = o.ctx; o.ctx = nullptr; }
        return *this;
    }
    void reset() {
        if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; }
    }
    bool valid() const { return ctx != nullptr; }
};

void tls_library_init();

// 服务端：cert/key 为 PEM；任一为空则生成 P-256 自签（CN=p2p-derp）
bool tls_make_server_ctx(TlsCtx& out, const char* cert_pem, const char* key_pem);

// 客户端：insecure=true 不校验证书（自签/内网）
bool tls_make_client_ctx(TlsCtx& out, bool insecure);

class TlsConn {
public:
    TlsConn() = default;
    ~TlsConn() { close(); }
    TlsConn(const TlsConn&) = delete;
    TlsConn& operator=(const TlsConn&) = delete;

    bool accept(SSL_CTX* ctx, int fd);
    bool connect(SSL_CTX* ctx, int fd);
    ssize_t read(void* buf, size_t n);
    ssize_t write(const void* data, size_t n);
    void close();
    bool valid() const { return ssl_ != nullptr; }

private:
    SSL* ssl_ = nullptr;
    std::mutex mu_;
};

} // namespace p2p

#endif // P2P_COMMON_TLS_IO_H
