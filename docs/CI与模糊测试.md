# CI 与模糊测试

## 一键入口与分组

本地 `make test` 仍按顺序执行原生、前端、契约三个组，任一失败立即返回非零。
`make test-sanitize` 仍是一键全量；CI 仅对原生组再次启用 sanitizer。

| 入口 | 内容 | 普通 CI 所属任务 |
| --- | --- | --- |
| `make test-native` | C++ 单元与使用编译夹具的 Python 行为测试 | host-unit，严格编译 |
| `make test-native-sanitize` | 同一组原生测试，ASan/UBSan，UB 不恢复继续执行 | host-sanitize |
| `make test-frontend` | Node/Vue 单元、离线构建及产物契约 | webui-layout |
| `make test-contracts` | Python 打包、生命周期、发布、采集器和测试编排验证 | host-unit |

纯契约组不依赖 Node 或已生成的 WebUI。原生组仍保留实际编译程序的行为回归，
并非把其中的 Python 驱动删掉。浏览器、HTTP/TLS、规则/模块更新、NFQUEUE、
nft/legacy 引擎和冻结隔离门禁均保留。Android 的两次正式构建用于可复现校验，
独立预览构建用于渠道隔离，这三次构建也保留。

前端单元从三组重复执行变为一次；纯 Python 契约从两次变为一次；取消主机组的
Node 安装和两次前端构建。本地 `make test-rules` 的额外规则产物验证仍可使用。

正式发布继续只接受 `.github/workflows/ci.yml` 的 main push/显式完整 CI，要求
五个主机分组、host-tests 汇总、webui-layout、android-build 全部成功，且该次运行的
其他任务也无失败或跳过。必需任务集合由 `scripts/ci_policy.py` 与发布工具共享，
汇总任务显示成功也不能绕过缺失的分组。纯文档快速路径未启用。
扩展 fuzz 工作流不能作为发布的 CI 输入，离线发布测试已覆盖此拒绝。

## 2026-11-08：主机测试并行与 PR 去重

基线为 v1.2.11 的 [CI 42183785057](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/42183785057)：
主机串行任务 1870 秒（31:10），Android 141 秒，前端 251 秒。主机中普通原生
测试 115 秒、HTTP/TLS 78 秒、规则/更新 267 秒、ASan/UBSan 329 秒、fuzz 31 秒、
NFQUEUE 6 秒、两轮引擎分别 488/485 秒、冻结隔离 53 秒。

按独立 runner 分成以下五组，避免真实防火墙、冻结和卸载测试在同一系统并发争抢状态：

| 分组 | 保留的测试 | 基线步骤时间之和，不含启动和排队 |
| --- | --- | --- |
| host-unit | Shell、Python 契约、严格原生、NFQUEUE、冻结隔离 | 约 3 分钟 |
| host-download | HTTP/TLS、完整规则和异步更新集成 | 约 5 分 45 秒 |
| host-sanitize | 完整 ASan/UBSan 和八入口 fuzz 烟测 | 约 6 分钟 |
| engine-nft | 显式 nft 后端的完整引擎集成 | 约 8 分 8 秒 |
| engine-legacy | legacy 后端的完整引擎集成 | 约 8 分 5 秒 |

这五组同时调度；host-tests 在所有组结束后执行，任何失败、取消、跳过或缺少
分组都返回失败。新增回归覆盖汇总 CLI 的退出状态，以及汇总为绿但某个组缺失/
失败时发布工具必须拒绝的情况。没有缩短原测试、降低 fuzz 次数或取消 sanitizer、
双栈/双后端、卸载和可复现构建。预期关键路径约 9—12 分钟，实际受 runner
性能和排队影响，需要以新 CI 的实测结果为准；runner 总工作量与原方案接近。

触发方式改为 main 的 push、PR 和手动运行，避免同一依赖分支 push 与 PR 同时
产生两套完整测试。没有 PR 的开发分支可手动运行 CI；main 合并后仍会完整验证。

实测结果：联合 PR 的 [CI 42468188786](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/42468188786)
八项全部通过，任务执行窗口 8:19，从创建到完成含调度为 8:38。两个 PR 合并后的
[main CI 42469894622](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/42469894622)
也八项全绿，创建时间 2026-11-08 10:05:06 UTC、完成时间 10:13:57 UTC，完整耗时
8:51。相较原先约 31 分钟的串行关键路径，本轮实测缩短约 72%；不同 runner 和
排队情况仍会影响后续耗时。这些是完整 CI 的验证结果，不代表手机性能变化。

## 八个 fuzz 入口

`tests/run_fuzz_smoke.sh` 调用 `tests/fuzz/run.py`；种子由
`tests/fuzz/seeds.py` 生成，包含已有发布 JSON 夹具，不使用手机流量或在线规则。

| 目标 | 重点覆盖 |
| --- | --- |
| packet | IPv4/IPv6、DNS、HTTP、完整/多记录 TLS、快速拒绝响应 |
| rules | Hosts/AdGuard、白名单、修饰符、字节与域名边界，以及分块排序/gzip 全字节对照 |
| nfnetlink | NFQUEUE 消息、属性长度、ACK 与截断 |
| release_json | 有界 JSON、发布字段投影、正文和 UTF-8 |
| reassembly | 分段、乱序、重叠、重传、SYN/FIN/RST、序号回绕、缓存容量与超时 |
| dns_reassembly | 两字节帧长、多消息、尾部半帧、消费/阻断回执、容量与超时 |
| rule_stream | 实际纯文本/gzip 文件、拼接/坏 gzip、解压与行数/行长限制 |
| module_zip | stored/deflate、目录/CRC/重复身份/长注释、版本与渠道拒绝 |

重组输入按最多 2048 条操作解释，每个输入创建独立解析器；时间含边界推进和倒退。
除了 sanitizer，入口还断言缓存数量/字节上限、清空及过期释放、返回域名非空，
以及 ZIP 被接受后错误版本/渠道必须拒绝。不会提取 ZIP 路径或执行安装器。

```sh
# 普通 CI：每个目标至少重放结构化种子，再进行快速覆盖引导 fuzz
FUZZ_REQUIRE_LIBFUZZER=1 FUZZ_RUNS=1000 make fuzz-smoke

# 有时间预算的扩展运行：每个目标 300 秒，共八个目标
FUZZ_REQUIRE_LIBFUZZER=1 FUZZ_SECONDS=300 make fuzz-smoke
```

普通烟测本地允许缺少 libFuzzer 时回退到确定性 ASan/UBSan 驱动；报告明确标为
`standalone`，先重放同一批种子再运行随机输入。扩展运行和 CI 必须使用真正的
libFuzzer，缺少运行库直接失败，不把回退结果当作覆盖引导 fuzz。libFuzzer 的时间、
单输入超时及崩溃保存参数遵循 [LLVM 官方文档](https://llvm.org/docs/LibFuzzer.html)。

默认单输入最多 128 KiB，单输入超时 10 秒、RSS 限制 1 GiB；规则流另使用较小的
解压预算以触达拒绝分支。这些是测试预算，不是模块生产内存承诺。

## 定时、语料与失败恢复

`.github/workflows/fuzz.yml` 计划每周一北京时间 03:23 在 main 运行，默认每个
入口 300 秒；也可手动选择 60/300/600 秒。GitHub 调度语义见
[官方 schedule 说明](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule)。
工作流只有读取仓库权限，不构建模块、不发版。

`actions/cache` 使用审核过的完整 SHA，跨成功 main 运行恢复 `dist/fuzz/corpus`；
每次运行使用独立缓存键。缓存只作为输入数据，不恢复编译二进制。
每个入口最多保留 512 个文件、8 MiB；拒绝链接和特殊对象，超长缓存输入剔除。
当前源码的种子和已提交崩溃回归优先保留；它们若超过预算则明确失败，不静默丢弃。
其余发现输入优先保留较小样本。语料是有上限的连续积累，不保证无限保存所有覆盖。

报告、运行日志、崩溃输入和扩展语料在成功/失败时均尝试上传 Actions artifact，
扩展报告保留 14 天。缓存仅在成功时保存，失败现场由 artifact 保留。
硬性取消、runner 丢失可能来不及上传，不能仅因没有 artifact 判定没有崩溃。
本地输出默认 `dist/fuzz`，可用 `FUZZ_OUTPUT` 指向另一目录；临时编译产物单独创建。

发现崩溃后先保留报告及输入，定位修复并做正常行为回归，再把原始输入提交到
`tests/fuzz/regressions/<目标>/`。后续每次烟测和扩展运行均自动重放，目录不存在
表示尚无该目标的已收录崩溃样本。控制面测试中的模拟失败仅验证非零传播和报告
留存，不能当作实际解析器崩溃或修复证据。

## 验证边界与耗时

2026-10-28 已在 Linux ARM64/Clang 14 对八个目标执行真实 libFuzzer + ASan/UBSan，
并验证已有语料恢复；Apple Clang 环境另验证确定性回退及种子重放。
主机与 sanitizer 完整用例、分组独立性、失败即停止、语料预算/链接隔离及缺少
libFuzzer 的 CI 拒绝另有回归。

v1.2.8 的完整 CI 40366870975 中 host-tests 用时 32:02；O13 提交 a3e8248 的
[完整 CI 40641961757](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/40641961757)
全部通过，host-tests 29:52、webui-layout 3:11、android-build 2:26；改前后者分别
为 3:57 和 2:17。原始时间记录在本地 dist/o13-validation/github-timings.json。
这是各一次真实运行，包含机器、编译和依赖下载波动，不能把全部差值归因于去重。
两套引擎和原生 sanitizer 仍占主要耗时；减少重复执行与扩大覆盖不承诺固定倍数
加速。上述均不替代手机或 24 小时长稳验收。


2026-10-28 后续实现 78ad4b1 的 [完整 CI 40646694145](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/40646694145)
也已全绿。新增扩展流程已在 GitHub 实际运行 [40646931025](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/40646931025)，
八入口各 60 秒全部通过；实际报告 artifact 摘要已验证，主分支语料缓存保存已由
Actions API 确认。该次是验证用 60 秒运行，周计划仍默认每入口 300 秒；不把这次
试跑当成所有未来定时任务或真机长期验收已经通过。
