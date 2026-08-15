#ifndef P2P_COMMON_LOG_H
#define P2P_COMMON_LOG_H

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace p2p {

// ---------------------------------------------------------------------------
// 轻量分级日志：线程安全，可开关，可写 syslog 风格时间戳
// ---------------------------------------------------------------------------
enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static Logger& instance() {
        static Logger s;
        return s;
    }

    void set_level(LogLevel lv) { lv_ = lv; }
    LogLevel level() const { return lv_; }

    // format 属性开启编译期格式串检查（成员函数 this 占第 1 参，fmt 为第 4 参）
    __attribute__((format(printf, 4, 5)))
    void log(LogLevel lv, const char* tag, const char* fmt, ...) {
        if (lv < lv_) return;
        char line[1024];
        {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(line, sizeof(line), fmt, ap);
            va_end(ap);
        }
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char ts[32];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

        std::lock_guard<std::mutex> lk(mu_);
        fprintf(stderr, "[%s][%s] %s\n", ts, tag, line);
        fflush(stderr);
    }

private:
    Logger() : lv_(LogLevel::Info) {}
    std::mutex mu_;
    LogLevel lv_;
};

#define P2P_LOG(lv, tag, ...) \
    ::p2p::Logger::instance().log(::p2p::LogLevel::lv, tag, __VA_ARGS__)

#define LOGD(tag, ...) P2P_LOG(Debug, tag, __VA_ARGS__)
#define LOGI(tag, ...) P2P_LOG(Info, tag, __VA_ARGS__)
#define LOGW(tag, ...) P2P_LOG(Warn, tag, __VA_ARGS__)
#define LOGE(tag, ...) P2P_LOG(Error, tag, __VA_ARGS__)

} // namespace p2p

#endif // P2P_COMMON_LOG_H
