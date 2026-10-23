# 深度分析隔离探针

用于复核[待优化方案](../../待优化方案.md)中的当前行为，**不是修复实现，也不是“正确行为”的通过证明**。探针刻意断言当前缺陷或边界；生产代码修复后，相关断言应失效，随后应迁移成验证正确行为的正式回归测试。

## 运行

需要 C++17 编译器、zlib、Python 3、Node.js，以及已安装的前端锁定依赖：

```sh
python3 tests/audit/run_deep_audit.py
python3 tests/audit/run_deep_audit.py --sanitize
```

默认没有接入 `make test` 或发布 CI，避免把“仍能复现缺陷”当成永久发布条件。普通构建启用 `-Wall -Wextra -Werror`，另可选择 ASan/UBSan。当前已在 macOS 验证；包含 Linux 编译选项，但不等于已验证跨平台行为。

## 隔离方式与限制

- 测试源码保存在本目录；编译产物和全部磁盘数据使用独占临时目录，结束后由 `TemporaryDirectory` 清理。
- 强制包含 `rebase_paths.h`，先加载公共宏，再将所有参与编译单元的 `DATA_DIR` 改到临时目录。生产头文件不修改。这个测试头不能用于生产构建。
- CLI 探针直接包含实际 `main.cpp`，只调用被测静态函数；进程迁移、daemon、日志、配置读写和防火墙依赖使用桩。测试只进行自身 PID 的信号 0 查询，不发送终止信号。
- 引擎探针直接包含实际 `engine.cpp`，运行真实 `hot_reload()`、规则加载/匹配与重组代码；防火墙安装/清理使用故障注入桩，真实 `run_daemon()` 不执行。macOS 的 `prctl` 编译适配仅在本目录生效。
- 未使用网络请求、ADB、root 安装、真实 NFQUEUE、防火墙或 cgroup 迁移。冻结状态检查使用合成文件读取回调，前端只使用模拟 bridge 和内存数据。
- 依赖未调用函数的编译/链接消除，因此这不是完整 Android 二进制或真实内核集成测试。不要将 probe 二进制刷入手机。

## 当前观测点

| 标记 | 观测内容 | 证据边界 |
| --- | --- | --- |
| `unrelated-pid` | 用探针自身 PID 模拟被复用的旧任务 PID，查询仍为 running | 未强制内核复用 PID |
| `idle-retention` | 读取重组统计不执行回收，显式 prune 后释放 | 无实际等待；结合引擎调用路径判断空闲回收缺口 |

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
双队列空闲边界与既有 PASS 连接保留。期望/生效修订号与界面 ACK 仍待后续实施，
不能把切换子问题修复当作 O03 全部完成。
当前隔离审计保留其余 2 个观测点，不再要求这些已修复的缺陷存在。

最终出现 `Audit characterization completed` 仅表示预期的当前行为已被观测；ASan/UBSan 没有报错也不代表完成安全审计。
