#pragma once
#include "common.h"
#include "logging.h"
#include <string>

inline void record_engine_error(const std::string &code, std::string message) {
    for (char &c : message) if (c == '\n' || c == '\r') c = ' ';
    if (message.size() > 2048) message.resize(2048);
    write_file_atomic(FILE_ENGINE_ERROR, "code=" + code + "\nmessage=" + message + "\n");
    log_line("错误", "%s", message.c_str());
}

inline void fail_engine_start(const std::string &code, const std::string &message) {
    record_engine_error(code, message);
    write_runtime_state("error");
}
