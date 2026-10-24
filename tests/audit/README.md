# 深度分析隔离探针

记录[待优化方案](../../待优化方案.md)中的历史隔离验证。原缺陷探针均已迁移到正常回归；当前入口仅执行空闲回收的正确行为测试，方便复核，不再断言“缺陷仍存在”。没有活动观测点，不表示项目已经没有其他问题。

## 运行

需要 C++17 编译器、zlib、Python 3、Node.js，以及已安装的前端锁定依赖：

```sh
python3 tests/audit/run_deep_audit.py
python3 tests/audit/run_deep_audit.py --sanitize
```

此历史入口不单独接入 CI，所调用的正式回归已纳入 `make test`。普通构建启用 `-Wall -Wextra -Werror`，另可选择 ASan/UBSan。当前已在 macOS 验证；包含 Linux 编译选项，但不等于已验证跨平台行为。

## 隔离方式与限制

- 测试源码保存在本目录；编译产物和全部磁盘数据使用独占临时目录，结束后由 `TemporaryDirectory` 清理。
- 强制包含 `rebase_paths.h`，先加载公共宏，再将所有参与编译单元的 `DATA_DIR` 改到临时目录。生产头文件不修改。这个测试头不能用于生产构建。
- 空闲回收测试直接包含实际 `engine.cpp`，运行 owner 的 `maintain_reassembly()` 和真实 HTTP/TLS、TCP DNS 缓存；真实 `run_daemon()` 不执行，不打开网络响应 socket。macOS 的 `prctl` 编译适配仅在本目录生效。
- 未使用网络请求、ADB、root 安装、真实 NFQUEUE、防火墙或 cgroup 迁移。冻结状态检查使用合成文件读取回调，前端只使用模拟 bridge 和内存数据。
- 依赖未调用函数的编译/链接消除，因此这不是完整 Android 二进制或真实内核集成测试。不要将 probe 二进制刷入手机。

## 当前观测点

| 标记 | 观测内容 | 证据边界 |
| --- | --- | --- |
| 无活动观测点 | 原缺陷均已迁移到正式回归 | 不等于完整安全审计或真机认证 |

O02 的 `read-failure-save` 已修复，从缺陷探针迁移到
`frontend/tests/config-edit.test.js`，正常 `make test` 验证读取失败后不可保存。
O18 的 `badfilter-substring` 已迁移至 `tests/rule_cases.tsv`，由实际 C++
解析器及 Python 合并工具共同验证黑白、精确/后缀和真正的 `$` 修饰符。
O17 的 `unknown-freezer` 已迁移至 `tests/test_cgroup_isolation.cpp` 和前端状态回归，
分别断言正常/冻结/未知，不再用 bool 折叠检查失败。
O15 的 `paused-daemon` 已迁移至 `tests/test_lifecycle.cpp`、
`tests/lifecycle_cli.cpp` / `tests/test_lifecycle_cli.py`：真实 CLI、配置、文件锁和进程调度，
用 barrier 验证 PID 发布前暂停、旧清理与新启动交错、多窗口和自启策略复核；
仅 PID 身份、引擎/防火墙执行及 cgroup 迁移隔离，不接管实际网络。
O16 的 `cleanup-failure` 已迁移至实际 C++ 清理接口、子进程执行和 CLI 正式回归：
`test_iptables_cleanup.py` / `test_lifecycle_cli.py` / `test_process_runner.cpp`。
它们断言部分残留、查询/锁/超时失败必须非零，保留外部规则且可以重试恢复；
相关依赖在普通主机测试中隔离，真实内核结果另行记录在方案进度中。
O03 原 `fault-state` 子问题随 O16 的退出故障传播一并修复，正式回归为
`test_engine_failure.cpp`，以及 `test_cleanup_failures.py` 中真实双栈切换/回滚失败后的非零退出。
O03 的 `mixed-generation` 已迁移到 `test_engine_failure.cpp`、`test_policy_barrier.cpp`
和真实内核 `test_reload_consistency.py`，验证失败保留旧快照/统计、切换期间无连接标记旁路、
双队列空闲边界与既有 PASS 连接保留。后端期望/生效修订号已由
`test_apply_state.cpp`、`test_engine_failure.cpp` 和真实内核 `test_apply_protocol.py`
验证保存/提交/失败/实际生效、身份、损坏与并发覆盖；界面由
`frontend/tests/apply*.test.js` 和 `test_webui_layout.py` 验证精确修订号观察、
旧策略呈现、核验失败不显示健康、显式重试不重启及关闭后无隐藏写入。
这些主机/浏览器/Linux 回归不替代 Android 真机长稳和压力验收。
O04 的 `unrelated-pid` 已迁移至 `test_job_identity.cpp` 与真实 CLI
`integration/test_job_identity.py`：按任务 ID、boot/PID/启动时间与可执行映像核验，
旧版 PID-only 记录不再算运行；查询不发送信号。任务记录与索引持久化、并发去重、
所有者崩溃、未知后代写入者和安装阶段不确定性另有正式回归。
O06 的 `idle-retention` 已迁移至 `tests/test_idle_maintenance.cpp`：实际 owner 定时维护调用同时回收 HTTP/TLS 和 TCP DNS 的未完成前缀，没有新数据包也能到期释放；边界时刻和时钟倒退不误回收。观察统计仍保持只读，不用修改 stats 的副作用让旧探针“通过”。原 CLI、引擎缺陷探针均已移除。

最终出现 `Audit regressions passed` 只表示这些指定回归通过；ASan/UBSan 没有报错不代表完成安全审计。
