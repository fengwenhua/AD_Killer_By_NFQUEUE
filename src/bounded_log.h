#pragma once

#include <cstddef>
#include <string>

// 追加前按 max_bytes 轮转 path(.1 ... .N)；backups=0 时原位截断。
// generation_header 给任务日志写入唯一轮转头；默认关闭，不改变 DNS/引擎日志格式。
bool append_bounded_log(const std::string &path, const std::string &data,
                        size_t max_bytes, int backups, bool generation_header = false);
