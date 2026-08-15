#include "TlsIo.h"
#include "Log.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <cstring>

namespace p2p {

void tls_library_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    });
}

static bool load_or_make_cert(SSL_CTX* ctx, const char* cert_pem, const char* key_pem) {
    if (cert_pem && cert_pem[0] && key_pem && key_pem[0]) {
        if (SSL_CTX_use_certificate_file(ctx, cert_pem, SSL_FILETYPE_PEM) != 1) {
            LOGW("Tls", "load cert failed: %s", cert_pem);
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, key_pem, SSL_FILETYPE_PEM) != 1) {
            LOGW("Tls", "load key failed: %s", key_pem);
            return false;
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            LOGW("Tls", "cert/key mismatch");
            return false;
        }
        return true;
    }

    // 临时自签：测试与内网「先通再切」用，生产应提供正式证书
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) return false;
    bool ok = true;

    X509* x = X509_new();
    if (!x) { EVP_PKEY_free(pkey); return false; }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 365 * 24 * 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("p2p-derp"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    if (X509_sign(x, pkey, EVP_sha256()) == 0) {
        X509_free(x);
        EVP_PKEY_free(pkey);
        return false;
    }
    ok = SSL_CTX_use_certificate(ctx, x) == 1 && SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
    X509_free(x);
    EVP_PKEY_free(pkey);
    if (ok) LOGI("Tls", "using ephemeral self-signed cert (CN=p2p-derp)");
    return ok;
}

bool tls_make_server_ctx(TlsCtx& out, const char* cert_pem, const char* key_pem) {
    tls_library_init();
    out.reset();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return false;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    if (!load_or_make_cert(ctx, cert_pem, key_pem)) {
        SSL_CTX_free(ctx);
        return false;
    }
    out.ctx = ctx;
    return true;
}

bool tls_make_client_ctx(TlsCtx& out, bool insecure) {
    tls_library_init();
    out.reset();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return false;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    }
    out.ctx = ctx;
    return true;
}

bool TlsConn::accept(SSL_CTX* ctx, int fd) {
    close();
    if (!ctx || fd < 0) return false;
    ssl_ = SSL_new(ctx);
    if (!ssl_) return false;
    SSL_set_fd(ssl_, fd);
    if (SSL_accept(ssl_) != 1) {
        LOGW("Tls", "SSL_accept failed: %s", ERR_reason_error_string(ERR_get_error()));
        close();
        return false;
    }
    return true;
}

bool TlsConn::connect(SSL_CTX* ctx, int fd) {
    close();
    if (!ctx || fd < 0) return false;
    ssl_ = SSL_new(ctx);
    if (!ssl_) return false;
    SSL_set_fd(ssl_, fd);
    if (SSL_connect(ssl_) != 1) {
        LOGW("Tls", "SSL_connect failed: %s", ERR_reason_error_string(ERR_get_error()));
        close();
        return false;
    }
    return true;
}

ssize_t TlsConn::read(void* buf, size_t n) {
    if (!ssl_) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    int r = SSL_read(ssl_, buf, (int)n);
    if (r > 0) return r;
    int err = SSL_get_error(ssl_, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
    return -1;
}

ssize_t TlsConn::write(const void* data, size_t n) {
    if (!ssl_ || !data) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t off = 0;
    while (off < n) {
        int w = SSL_write(ssl_, p + off, (int)(n - off));
        if (w <= 0) {
            int err = SSL_get_error(ssl_, w);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

void TlsConn::close() {
    if (!ssl_) return;
    std::lock_guard<std::mutex> lk(mu_);
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
    ssl_ = nullptr;
}

} // namespace p2p
