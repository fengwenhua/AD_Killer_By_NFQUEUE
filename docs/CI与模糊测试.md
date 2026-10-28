# CI 与模糊测试

## 一键入口与分组

本地 `make test` 仍按顺序执行原生、前端、契约三个组，任一失败立即返回非零。
`make test-sanitize` 仍是一键全量；CI 仅对原生组再次启用 sanitizer。

| 入口 | 内容 | 普通 CI 所属任务 |
| --- | --- | --- |
| `make test-native` | C++ 单元与使用编译夹具的 Python 行为测试 | host-tests，严格编译 |
| `make test-native-sanitize` | 同一组原生测试，ASan/UBSan，UB 不恢复继续执行 | host-tests |
| `make test-frontend` | Node/Vue 单元、离线构建及产物契约 | webui-layout |
| `make test-contracts` | Python 打包、生命周期、发布、采集器和测试编排验证 | host-tests |

纯契约组不依赖 Node 或已生成的 WebUI。原生组仍保留实际编译程序的行为回归，
并非把其中的 Python 驱动删掉。浏览器、HTTP/TLS、规则/模块更新、NFQUEUE、
nft/legacy 引擎和冻结隔离门禁均保留。Android 的两次正式构建用于可复现校验，
独立预览构建用于渠道隔离，这三次构建也保留。

前端单元从三组重复执行变为一次；纯 Python 契约从两次变为一次；取消主机组的
Node 安装和两次前端构建。本地 `make test-rules` 的额外规则产物验证仍可使用。

正式发布继续只接受 `.github/workflows/ci.yml` 的 main push/显式完整 CI，要求
host-tests、webui-layout、android-build 全部成功，且该次运行的其他任务也无失败或
跳过。本次未增加普通 CI job，所以必需 job 名不变。纯文档快速路径未启用。
扩展 fuzz 工作流不能作为发布的 CI 输入，离线发布测试已覆盖此拒绝。

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
