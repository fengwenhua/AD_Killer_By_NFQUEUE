#pragma once

#include <cstddef>
#include <string>

// 追加前按 max_bytes 轮转 path(.1 ... .N)；backups=0 时原位截断。
bool append_bounded_log(const std::string &path, const std::string &data,
                        size_t max_bytes, int backups);
