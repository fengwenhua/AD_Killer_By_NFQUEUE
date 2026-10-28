# 主机端测试

`make test` 只使用普通用户权限，不执行真实 `iptables`、NFQUEUE 或 Android 系统写入。
主机测试还需 Python 3 和 Node.js 24 LTS / 22.12+；先执行
`npm --prefix frontend ci --include=dev`。Node 导入真实 Vue 组合式逻辑，模拟原生
WebUI 回调接口；测试同时构建并检查离线页面，不依赖提交生成的 HTML。
每次运行都使用独立临时构建目录，因此多个工作区或 CI job 可以并行执行。

```bash
make test

# CI 可独立调用；本地 make test 仍包含全部三组
make test-native
make test-native-sanitize
make test-frontend
make test-contracts

# 同时验证 data/out_rules 的 gzip 与 rule_stats.conf 一致性
make test-rules

# 指定编译器；失败后保留临时二进制便于调试
CXX=clang++ KEEP_TEST_BUILD=1 make test

# libFuzzer + ASan/UBSan 短程烟测（默认每个入口 500 次）
make fuzz-smoke
FUZZ_RUNS=10000 make fuzz-smoke
FUZZ_REQUIRE_LIBFUZZER=1 FUZZ_SECONDS=300 make fuzz-smoke

# 全套测试启用 -Werror、ASan 与 UBSan
make test-sanitize

# Linux/root 环境：真实内核 NFQUEUE + iptables 双栈回环
make test-nfqueue-integration

# 仅一次性 Linux/root：运行完整引擎、双栈 DNS/HTTP/TLS、热切换、停止和冲突拒绝
make test-engine-integration
IPTABLES_BACKEND=legacy make test-engine-integration

# 真实 libcurl / curl CLI：本地 HTTP/TLS、重定向、证书拒绝及大小上限
make test-download-integration

# 不改系统配置的合成规则/域名提取微基准；不是手机应用启动测速
make benchmark
RULE_COUNT=1000000 make benchmark
```

真实规则同步/启动/重载的 RSS 测量另用 `make profile-rule-memory`，仅限一次性
Linux/root 环境；用法、固定输入与前后对照见
[`规则处理内存基线`](../docs/规则处理内存基线.md)。该入口不会升级手机，
也不加入每次 CI 的大内存性能门禁。

覆盖范围：

- `test_source_preview` / `test_preview.py`：默认 500 / 可选 100–5000 Unicode 码点、gzip/非法 UTF-8，
  真实双下载后端的片段上限（包括忽略 Range）、TLS/重定向拒绝、错误与空内容；
  HaGeZi CLI 预览对正式配置/规则/同步记录进行字节级不变验证。
  包括“前缀已返回、尾部延迟 3 秒”的服务，验证外部 curl 不会因输出缓冲等尾部。
- `test_policy.py`：真实不同 UID 的双栈 DNS/HTTP，范围独立名单、第三个未选 UID、
  空拦截名单、两种规则分类、自定义例外、合法空快照发布和损坏候选保留。
- `test_app_catalog` / `test_application_lifecycle.py`：包管理器完整查询与主用户边界、
  共享 UID、重装/卸载/编号复用、旧缓存不授权、30 秒自动核实、未知保留与恢复、
  保存竞争及查询取消。Linux 仅模拟 Android 包目录，仍执行真实 CLI、双栈路由、
  NFQUEUE 和流量；不代替厂商分身真机验收，见 [应用名单生命周期](../docs/应用名单生命周期.md)。
- `test_release_catalog` / `test_document.py` / `test_module_update.py`：严格有界 JSON、
  发布字段精简、第二页有效附件、长日志和截取提示、总时间/字节预算、HTTP 206 与
  未读完正文的拒绝、库初始化阻塞回收，以及静态元数据后备。
  参数与边界见 [更新检查策略](../docs/更新检查策略.md)。

- `test_packet`、`test_nfnetlink_parser`：IP、DNS、TLS/HTTP 和 netlink 边界解析；
- `test_fast_response`：NXDOMAIN 的 ID/问题区/EDNS/压缩指针、TCP RST 序列号、
  双栈 IP/UDP/TCP/ICMP 校验和与大小边界；fuzz 同时覆盖快速响应构造。
- `test_rules`、`test_rule_stream`、`test_source_validator`：规则语义、流式 gzip、资源上限与快照并发；
- `test_recipe_parity.py`：12 个普通源共享配方、C++/Python 完整规则内容与计数一致，覆盖
  显式 Hosts→AdGuard、低比例浏览器列表、gzip、部分失败、非法域名和隔离输出；
- `test_config_store`、`test_update_job`、`test_rule_bundle`：配置锁、任务状态和原子 generation；
- `test_data_paths.py`：独立数据路径与共享根目录权限边界；
- `test_iptables`：纯命令规划和路由快照判定，不调用宿主机防火墙；
- `test_tcp_reassembly`、`test_metrics`：有界握手重组与并发指标；
- Python 契约测试：规则合并器、Vue WebUI 离线构建产物，以及可复现 ZIP、许可证、校验和、
  支持矩阵与第三方许可清单。
- `test_boot_service.py`：在临时目录以假 getprop/sleep 执行实际开机脚本，验证
  默认不自启、缺失/非法配置、手动暂停、180 次等待超时和等待后配置复核；无 root 写入。
- `test_queue_ownership`：解析占用队列与非 PID 的 netlink port ID，验证 socket inode
  后才显示进程身份，避免误认；真实 `test_startup_conflict.py` 验证外部队列占用时
  普通启动和直接 daemon 入口均拒绝，保留其他进程/队列、暂停配置和防火墙。

构建发布包后可独立验证结构和校验和：

```bash
python3 tests/validate_module_zip.py dist/AD_Killer_By_NFQUEUE-v<版本>.zip
python3 tests/verify_checksum.py dist/AD_Killer_By_NFQUEUE-v<版本>.zip
python3 tests/validate_manager_update.py dist/AD_Killer_By_NFQUEUE-v<版本>.zip
```

`test_manager_update.py` 离线验证原生管理器标准四字段、严格 int32 版本序号、
预览转正式、固定发布附件、哈希及日志一致性；构建与 CI 对实际两渠道 ZIP 再执行
`validate_manager_update.py`。这不是 KernelSU/APatch/Magisk 三端的真机安装证明。

`tests/fuzz/` 中的 harness 分别直接调用 DNS/TLS/HTTP/IP、Hosts/AdGuard 和
NFQUEUE netlink 和发布 JSON 解析入口。发布目标包含有效与非法 JSON 种子，复制到临时
corpus 后运行，不回写源码。`FUZZ_CXX` 可指定支持 `-fsanitize=fuzzer` 的 Clang；
缺少 libFuzzer runtime 时烟测自动改用确定性随机驱动，仍保留 ASan/UBSan；长时间
fuzz 时应使用完整 LLVM，并给各二进制传入持久 corpus 和 artifact 目录。

`test_listing_and_release.py` 使用合成 GitHub 响应验证收录清单、main 全绿门禁、
渠道与来源一致性、版本单调增长、禁止复用标签、草稿上传顺序及中断不公开。
不创建真实发布；完整官方清单／ZIP 校验在 CI 的固定 SHA dry-run Action 中执行。
还覆盖原始 CI 字节绑定、草稿发现、部分上传与明确恢复、响应丢失只读确认、
设置变化、状态延迟、回执故障和重复点击。`test_release_transport.py` 检查只读
客户端、artifact/asset 媒体类型、下载去凭据、HTTPS 与大小/期限限制；
操作边界见 [发布中断恢复](../docs/发布中断恢复.md)。

`test-nfqueue-integration` 会临时插入仅匹配本机测试端口的 IPv4/IPv6 mangle 规则，
实际绑定 queue、启用 copy/maxlen/fail-open、接收 packet ID 和 payload、发送 ACCEPT
verdict，并验证 verdict 写 PASS/BLOCK connmark 后，同一连接的下一包会绕过队列或在
队列前被 DROP，并校验 verdict 保留模拟系统低 20 位和 bit 24/25。随后把队列上限
设为 1，实际验证队列满时 fail-open，以及监听器退出后的 queue-bypass；退出 trap
会删除测试规则。该测试只应在一次性 Linux CI runner 或明确授权的测试机
上以 root/sudo 运行，不应在日常工作站或生产设备直接执行。

真实 Android 内核、双栈 iptables、NFQUEUE verdict 故障和 WebView 交互仍按
[`docs/真机上线审计与验收方案.md`](../docs/真机上线审计与验收方案.md) 中的设备矩阵执行。

`test-engine-integration` 会在一次性环境创建 `/data/adb/AD_Killer_By_NFQUEUE` 并接管
测试环境的出站 DNS/TCP 80/443；若已有数据目录或 queue 0/1 则拒绝运行。
它不是只读检查，禁止在用户手机或日常工作站执行。测试会清理自己创建的数据，
并验证外部匿名规则在引擎退出后仍然保留。
同时覆盖 CLI 停止、SIGKILL 后新连接 bypass 以及残留路由下重新启动。
真实 socket 测试断言被拦截 DNS/TCP/QUIC 在 0.5 秒内收到 NXDOMAIN、reset、
port-unreachable，而非仅断言原请求未到达；正常请求仍能到达，原生和无 GLUE_CT
路径均覆盖双栈。该入口要求 NET_RAW，用于向本地测试发起者返回快速失败。
此入口还需 `conntrack` 和提供真实 `pgrep` 的 `procps`（Debian/Ubuntu：
`apt-get install conntrack procps`）。前台启动回归必须保留真实进程发现路径，
验证启动者不误认自身为旧守护，重复启动仍复用已有守护。独立使用
conntrack-tools 读取真实 IPv4/IPv6 连接，核验 CTNETLINK 掩码 PASS/BLOCK 更新保留
所有非模块位、非法输入不写入、连接缺失不创建；再以仅测试构建宏模拟无 GLUE_CT
内核，重跑同一套完整引擎测试。生产包没有可由环境变量开启的测试绕过开关。
独立 conntrack 位保留测试会在连接建立前安装端口受限、无写入动作的 connmark
匹配规则，显式激活按需跟踪 hooks；退出时清理。CTNETLINK 可用不代表新连接已被
跟踪，不能因返回 ENOENT 就跳过测试。真实 NFQUEUE probe 还故意向已裁决 ID
重复发送 verdict，验证内核的异步 ENOENT 被精确识别而不误触发停机。
引擎启动前还执行 CLI 自启配置测试，验证默认暂停、配置持久化、旧配置缺省关闭和
非法参数不写入；自启开关不能隐式启动或停止引擎。

指标/日志集成通过 `tests/integration/run_observability.py` 进入独立网络命名空间，
还需 `util-linux` 的 `unshare` 与 `iproute2` 的 `ip`。外层持续产生真实本机 DNS
干扰流量，内层保留完整空闲不重写与精确日志计数断言，避免将 CI runner 的后台
流量算入测试。不能创建命名空间时明确失败，不退回共享网络或跳过断言。

`test-download-integration` 需要编译器、Python 3、curl、libcurl 和 OpenSSL，
只启动临时回环 HTTP/TLS 服务并使用临时文件，不连接外部规则源。

`make test-update-integration` 在一次性 Linux/root 环境编译真实 CLI，使用本地
HTTP 服务覆盖仅 AdGuard、仅 Hosts、混合来源、暂停时计数更新、损坏/空源保留
旧 generation 及异步 job。需要 libcurl.so.4 或 /usr/bin/curl；禁止在用户手机运行。
同一入口同时验证真实 `--install` 不改变 `/data/adb` 权限。

手机页面另有独立浏览器回归（Android 接口全部模拟，无手机操作）：

```bash
python3 -m pip install playwright==1.62.0
python3 -m playwright install chromium
python3 tests/test_webui_layout.py --screenshots dist/webui-preview
python3 tests/test_webui_persistent_state.py
python3 tests/test_webui_performance.py --verify
```

可通过 `--browser-path` 指定已有 Chromium。覆盖 320–1024px 共 10 个宽度、
深浅两主题、导航同级结构、横向溢出、44px 触控、关键颜色对比度及同步结果反馈。
预览规则数为模拟数据，不是真机同步成功的证明。

`test_webui_persistent_state.py` 验证手机尺寸下的实时拦截计数，以及真正刷新页面后
更新日志和“有更新”提示的本地缓存恢复；缓存下载前必须重新检查，且不会自动安装。

`test_webui_route_recovery.py` 验证路由丢失和旧守护映像会退出绿色状态，提示手动恢复；
只通过完整校验恢复正常显示，观察本身不自动启停。真实内核集成还覆盖生命周期停止
保留策略、状态查询不修改防火墙、仅映像升级及双栈入口缺失后的手动恢复。

浏览器测试覆盖四页导航、设置/更新日志、版本显示与离线回退，以及实际
插值、快速切换后几何归位、运行中 reduced-motion 与草稿保留。静态尺寸断言在
减少动态效果下执行，动画行为另测；同时覆盖原生合成推页、等高视口、虚拟列表换列/
键盘遍历、立即弹窗/焦点闭环、字符数持久化、模块更新及拒绝存储测试。
性能探针使用 800 应用/64 来源/CPU 降速，记录 RAF 间隔但不冒充手机实际 FPS；
CI 只对有界 DOM、底栏逐帧不位移及视口隔离等确定性条件设门槛。

`tests/integration/device_latency_probe.cpp` 是需另行交叉编译的授权真机回环探针。
它不启停引擎、不写规则/配置；要求引擎已运行、QUIC 阻断已开启、指定域名确实
被拦截，且回环 UDP 53/443、TCP 80 端口空闲。每次 socket 等待最多 1 秒，验证
IPv4/IPv6 DNS 否定回答、TCP reset、QUIC 不可达与正常 HTTP 放行并输出毫秒数。
它不是自动部署脚本，不应在未经确认的设备上执行。

## 深度分析隔离探针（手动运行）

`python3 tests/audit/run_deep_audit.py` 及其 `--sanitize` 模式用于复核当前故障路径，
所有防火墙/守护操作均用桩隔离，不访问手机。源码位于 `tests/audit/`，只有编译产物和
测试数据使用临时目录。它刻意断言当前问题，**不是修复后的通过证明，也不接入默认 CI**；
修复后需迁移成正确行为回归。覆盖范围与限制见[探针说明](audit/README.md)。

CI 分组、八个 fuzz 入口、定时语料与崩溃回归流程见
[CI 与模糊测试](../docs/CI与模糊测试.md)。

离线设备报告汇总使用 `scripts/summarize_device_baseline.py`，无需 ADB。
13 项回归覆盖会话/时钟/缺失、计数与瞬时量、输入变化、配额/链接/异常 JSON、
私密字段与独占输出；用法见 [设备基线协议](../docs/设备基线采集协议.md)。
