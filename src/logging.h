// AD_Killer_By_NFQUEUE - 日志与运行时状态
#pragma once

#include <cstddef>
#include <functional>
#include <string>

// 设置当前进程的独立日志；未设置时 log_line 输出 stdout。
void configure_process_log(const char *path, size_t max_bytes, int backups);

// 追加一行到当前进程日志。
void log_line(const char *tag, const char *fmt, ...);

// 滚动写运行时状态文件:running / stopped / error
// WebUI 通过 cat runtime.state 轮询该文件等待引擎启停完成
bool write_runtime_state(const char *state);

// 读取 runtime.state(无文件返回空串)
std::string read_runtime_state();

// DNS 查询日志(带时间戳): [HH:MM:SS] domain -> verdict
void log_dns_query(const std::string &domain, const char *verdict);

// 追加文本文件(原子写临时文件 + rename)
bool write_file_atomic(const char *path, const std::string &content);

// 读取整个文本文件
bool read_file(const char *path, std::string &out);

// 读取一行配置键值(忽略 # 注释与空行)
std::string read_conf_value(const char *path, const char *key);

// 写键值对到 conf 文件(保留其它行)
bool write_conf_value(const char *path, const char *key, const char *value);

// 测试及特殊调用使用的显式锁路径版本；生产配置统一使用 STATE_LOCK_FILE。
bool write_conf_value_with_lock(const char *path, const char *key,
                                const char *value, const char *lock_path,
                                const std::function<bool()> &before_commit = {});

// 当前时间字符串 %Y-%m-%d %H:%M:%S
std::string now_string();

// 纪元秒
int64_t now_epoch();

// daemon.pid 管理
bool write_daemon_pid(int pid);
int  read_daemon_pid();
bool is_daemon_pid(int pid);
void clear_daemon_pid_if(int expected_pid);

// gzip 压缩字符串(用于规则落盘,输出 gzip 流字节)
bool gz_compress_string(const std::string &in, std::string &out);

// 二进制写文件 + 临时文件原子 rename
bool write_file_raw_atomic(const char *path, const std::string &data);

// A rename can become visible even if the following directory sync fails.
// Callers must not report that the old file was preserved in this case.
struct AtomicWriteResult {
    bool committed = false;
    bool durable = false;
};
AtomicWriteResult write_file_durable_atomic(const char *path, const std::string &data);
