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
| `paused-daemon` | 暂停配置下，daemon 入口仍到达引擎桩 | 证明入口缺少复核；未复现真实启动/暂停交错 |
| `cleanup-failure` | 清理桩保留路由时，实际 stop 仍返回 0/stopped | 故障注入，不会留下真实路由 |
| `unrelated-pid` | 用探针自身 PID 模拟被复用的旧任务 PID，查询仍为 running | 未强制内核复用 PID |
| `mixed-generation` | 路由安装返回失败后，真实规则匹配已切至新快照 | 路由依赖为桩，规则逻辑真实执行 |
| `fault-state` | 回滚失败后 `g_stop=true`，`g_fault=false` | 不声称执行了完整 daemon 退出路径 |
| `idle-retention` | 读取重组统计不执行回收，显式 prune 后释放 | 无实际等待；结合引擎调用路径判断空闲回收缺口 |
| `unknown-freezer` | membership/events 读取失败折叠成 false | 使用合成 cgroup 数据，未冻结真实进程 |
| `badfilter-substring` | 合法黑白域名含 badfilter 子串时被 AdGuard 解析器拒绝 | 同时以普通域名及 Hosts 解析作对照 |
| `read-failure-save` | 真实前端组合逻辑在读取失败后允许保存，覆盖模拟旧规则为空 | 不修改手机规则 |

最终出现 `Audit characterization completed` 仅表示预期的当前行为已被观测；ASan/UBSan 没有报错也不代表完成安全审计。
