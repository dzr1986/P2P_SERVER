#ifndef P2P_COMMON_FILE_H
#define P2P_COMMON_FILE_H

// 文件句柄 RAII（C++ 风格重构基础设施）：
//   消灭「每条错误路径手写 fclose」的重复与泄漏风险

#include <cstdio>
#include <memory>
#include <string>

namespace p2p {

using FileHandle = std::unique_ptr<FILE, int (*)(FILE*)>;

inline FileHandle open_file(const std::string& path, const char* mode) {
    return FileHandle(fopen(path.c_str(), mode), &fclose);
}

// 整文件读入 string；文件不存在/为空返回 false
inline bool read_file_all(const std::string& path, std::string& out) {
    FileHandle fp = open_file(path, "rb");
    if (!fp) return false;
    out.clear();
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp.get())) > 0) out.append(chunk, n);
    return !out.empty();
}

} // namespace p2p

#endif // P2P_COMMON_FILE_H
