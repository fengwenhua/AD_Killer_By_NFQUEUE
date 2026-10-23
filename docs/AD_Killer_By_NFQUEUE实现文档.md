# AD_Killer_By_NFQUEUE 实现文档

> 版本:v1.1
> 本文讲清楚这个模块**有什么功能、每个功能怎么实现**。
> 规则合并工具的使用见《开源规则合并指南.md》。

---

## 一、功能总览

AD_Killer_By_NFQUEUE 是一个基于 **Linux 内核 NFQUEUE** 的系统级去广告 KernelSU/Magisk 模块,
不依赖 VPN 接口,直接在内核网络栈的 mangle 表上接管出站流量:

```
应用发出 DNS 查询 / TLS 握手 / HTTP 请求
        │
        ▼
iptables mangle 表 OUTPUT 链
        │
        └─ ADK_NFQ_ENTRY(稳定入口,第一条 jump 在 A/B 间切换)
              ├─ scope=0: -j ADK_NFQ_OUT_A/B
              │     └─ 主链头部:白名单 uid -j RETURN(放行名单应用)
              └─ scope=1: -j ADK_NFQ_APPS_A/B
                    └─ 名单 uid -j 同代 ADK_NFQ_OUT_A/B,链尾 RETURN
        │
        ▼
ADK_NFQ_OUT_A/B 主拦截链
        │
        ├── UDP/TCP 53(DNS)───────────────► NFQUEUE 队列 0(拷贝上限 4096B)
        ├── TCP 80/443 未分类连接──────────► NFQUEUE 队列 1(拷贝上限 4096B)
        │       └── PASS/BLOCK connmark───► 后续包快速 RETURN / 持续 DROP
        ├── UDP 443(QUIC,可选)───────────► 队列 1 → ICMP 不可达后 DROP
        └── 其余流量 ──► RETURN 放行(链尾兜底)
        │
        ▼ (用户态引擎,2 个工作线程)
队列 0 线程:解析 DNS QNAME ─────────┐
队列 1 线程:解析 TLS SNI / HTTP Host ─┤
                                      ▼
                    FNV-1a 64 位哈希池匹配(黑/白名单)
                    ├── PASS(whitelist) → NF_ACCEPT
                    ├── BLOCK(exact)    → 快速否定响应后 NF_DROP
                    ├── BLOCK(suffix)   → 快速否定响应后 NF_DROP
                    └── PASS(default)   → NF_ACCEPT
```

**防断网设计**:队列异常立即拔除全部 iptables 规则;
规则下发先建队列后挂链(挂链前流量不被接管);`--queue-bypass` +
内核 fail-open 双保险;所有链尾兜底 RETURN。

---

## 二、模块组成

```
AD_Killer_By_NFQUEUE/
├── module/                     模块本体(KSU 规范:module.prop + 3 脚本 + webroot)
├── src/                        C++17 引擎源码
│   ├── common.h                常量与路径
│   ├── main.cpp                CLI 入口(13 个子命令)+ 守护进程生命周期
│   ├── engine.cpp              NFQUEUE 引擎(队列线程/热重载/看门狗)
│   ├── nfnetlink.cpp           手写 NFQUEUE netlink 协议(零外部依赖)
│   ├── nfnetlink_parser.cpp    有界 netlink 报文解析 + 原子 sequence
│   ├── packet.cpp              DNS QNAME / TLS SNI / HTTP Host 解析
│   ├── fast_response.cpp       双栈 RAW socket，向本地发起者发送 NXDOMAIN/RST/ICMP
│   ├── rules.cpp               规则引擎(FNV-1a 哈希池 + AdGuard 语法)
│   ├── rule_bundle.cpp         不可变 generation staging/原子指针发布
│   ├── source_validator.cpp    来源内容识别、有效率与拒绝原因统计
│   ├── update_job.cpp          后台更新 job ID 与状态协议
│   ├── bounded_log.cpp         日志大小上限与定量备份轮转
│   ├── iptables.cpp            稳定入口 + A/B 代际路由事务
│   ├── net.cpp                 有界 HTTP(S) 下载器(libcurl → execv 兜底)
│   ├── net_policy.cpp          URL 校验与无 shell argv 策略
│   └── logging.cpp             日志/状态文件/gzip/配置读写
└── scripts/merge_rules.py     开源规则合并打包工具
```

设计取舍:纯本地实现,无外部服务依赖,无文件完整性自检。

---

## 三、CLI 命令

| 命令 | 功能 | 关键实现 |
|------|------|----------|
| `--install` | 安装初始化 | 建数据目录;写默认 `state.conf`/`adblock_mode.txt`/`update_sources.txt` 等 |
| `--start` | 启动引擎 | 见 §4.1 进程模型 |
| `--stop` | 安全停止 | state=stopping → 找守护进程 → SIGTERM → 超时 8s 强杀 → 拔规则 → state=stopped |
| `--daemon` | 内部守护进程入口 | 需要当前启动修订号；发布 PID 前复核授权，裸调用不能绕过暂停 |
| `--daemon --foreground` | 显式前台调试 | 作为新的手动启动，预检并记录修订号，再前台运行；不用于自启 |
| `--start-boot` | 开机策略启动 | 后端复核自启及暂停，等待/加载期间策略变更使旧请求失效 |
| `--reload` | 热重载 | 运行中向守护进程发 SIGHUP;未运行输出 `{"status": "error"}` |
| `--update` | 拉取规则源 | 下载(支持 .gz)→ 逐行归类 Hosts/AdGuard → gzip 落盘 → 热重载 |
| `--update-start` | 后台更新 | 创建唯一 job，脱离 WebUI exec 会话运行并返回 job_id |
| `--update-status ID` | 更新状态 | 查询 job 的 running/success/failed/interrupted 与真实退出码 |
| `--status` | 状态 JSON | 完整版:按当前配置验证双栈 OUTPUT/ENTRY、queue 0/1、scope、UID 与 QUIC |
| `--status-fast` | 快速状态 | 跳过 iptables 校验(WebUI 高频轮询用) |
| `--version-status` | 版本 JSON | module/version/versionCode |
| `--mode 1\|2` | 规则强度 | 1=仅 Hosts 2=Hosts+AdGuard;写配置 + 运行中热重载 |
| `--scope 0\|1` | 作用域 | 严格校验参数;0=全局拦截 1=仅名单内应用;运行中触发路由热重载 |
| `--dev on\|off` | DNS 查询日志 | 写 `dev_mode_enabled` 标志文件;输出 `{"dev_mode": "done"}` |
| `--quic on\|off` | 阻止 QUIC | 通知守护进程重建规则(热重载);输出 `{"block_quic": %s}` |
| `--autostart on\|off` | 开机自启 | 独立配置，默认关闭；只持久化策略，不启动、停止或重载当前引擎 |
| `--apps-apply` | 应用名单生效 | 见 §4.3 三级 uid 解析;输出 `applied/deferred + scope` |
| `--config-read custom\|allow\|intercept` | 安全读取可编辑配置 | JSON 返回内容和 SHA-256 修订号；读取失败不视为空文件 |
| `--config-save TARGET REVISION` | 安全保存可编辑配置 | 从 stdin 读取 UTF-8，固定目标、语法校验、锁内修订号比较与原子替换 |

配置编辑上限为 1 MiB、20000 行、单行 1024 字节。三个目标只对应
`custom_rules.txt`、`app_whitelist.txt`、`app_intercept.txt`，不接受路径。
合法空文件返回 `ok`；不存在、读取失败或编码错误返回非零退出码，WebUI 禁止保存。
文件不存在时，仅手动 CLI 可显式使用 `missing` 修订号创建；WebUI 应先初始化完整模块。
保存冲突返回 `conflict`/退出码 2，保留草稿，重新加载并合并后再保存。
写入先同步临时文件，再 rename 并同步父目录；rename 后目录同步失败返回
`durability_unknown`（`committed=true`），不能当成“未写入”或盲目重试，须重新读取核对。
旧后端不支持这个接口时编辑器只读失败关闭，不退回 shell 直接覆盖。

首次安装默认 `autostart=false`、`enabled=false`、`manual_stop=true`。显式
`--stop` 保存暂停，`--start` 恢复为启用，但两者均不修改 `autostart`。
旧配置缺少自启项时默认关闭。只有明确开启自启且未暂停，`service.sh` 才等待开机；
`sys.boot_completed` 未就绪时不执行引擎/防火墙操作，等待 180 次仍未完成就退出。
等待完成后再检查自启和暂停配置，避免等待期间关闭开关仍启动。

---

## 四、核心机制详解

### 4.1 启动进程模型(`--start`)

```
--start(首次调用)
  ├─ mkdir 日志目录;runtime.state = "starting"
  ├─ 检测守护进程(pid 文件 + pgrep -f '[A]D_Killer_By_NFQUEUEd? --daemon' 兜底)
  │    └─ 已运行 → state="running" + 输出 {"status": "running"}  ✗ 退出
  ├─ 无 root 权限?
  │    └─ su -c 重入 --start,输出 {"status": "starting_su"}       ✗ 退出
  └─ 有 root:
       system("export ADK_NFQ_START_DISPATCHED=1; mkdir -p ...;
               umask 022; cd /; nohup <自身> --start >/dev/null 2>&1 &") ✗ 退出

--start(重入,DISPATCHED 环境变量已设)
  └─ 双 fork + setsid(孙进程被 init 收养,脱离 WebUI 会话)
       ├─ stdio 重定向到 /dev/null
       ├─ log_line 独立写入有界 log/engine.log
       └─ exec 自身为 --daemon(argv 含 "--daemon" 供 pgrep/校验)
```

要点:
- **DISPATCHED 环境变量**是防重入标记 —— nohup 重入后的 `--start` 不再派发,
  直接进 daemonize,避免无限循环；它不是授权。实际派发还携带启动修订号与 boot 标记，
  后台重入及发布 PID 前核验 `lifecycle.intent` 与最新暂停/自启策略。
- **启停协调**：意图锁只保护短事务，控制锁覆盖 PID 发布和 stop 后置清理；
  迟到的旧启动不能绕过新暂停，旧 stop 不能清理新实例。详见《软重启与守护生命周期》。
- **双 fork + setsid**:WebUI 划掉后台后守护进程不退出
- **生命周期锁**:`.daemon.lock` 由守护进程全程持有,并发启动只有一个实例能进入引擎
- 守护进程完整 `argv[0]` 为 `AD_Killer_By_NFQUEUEd`；受 Linux `TASK_COMM_LEN`
  限制，`prctl` 短名称为 `ADK_NFQd`

### 4.2 NFQUEUE netlink 协议

不依赖 libnetfilter_queue,手写 netlink 消息:

| 消息 | nlmsg_type | 说明 |
|------|-----------|------|
| 队列 BIND | `(3<<8)\|2` (0x302) | nfgenmsg.res_id=htons(队列号),attr `CMD={command=1}` |
| copy 参数 | 0x302 | attr `PARAMS={be32 copy_range; u8 copy_mode}` packed(nla_len=9) |
| fail-open | 0x302 | attr `MASK={be32 1}` + `FLAGS={be32 1}`；必须成功取得 ACK，否则不接管流量 |
| 判定回执 | `(3<<8)\|1` (0x301) | attr `VERDICT_HDR={be32 verdict; be32 packet_id}`，不是 nfmark；NF_DROP=0/NF_ACCEPT=1 |
| 收包 | `(3<<8)\|0` (0x300) | 解析 `PACKET_HDR`(包 id)+ `PAYLOAD` + `CAP_LEN` |

socket:`AF_NETLINK/SOCK_RAW/NETLINK_NETFILTER(12)`,SO_RCVBUF=4MB,SO_RCVTIMEO=2s。
Android/Linux 的解析器直接引用平台 UAPI 常量；macOS 主机测试才使用便携副本。
ACK 来源以 socket 返回的 `sockaddr_nl.nl_pid=0` 校验，消息头 `nlmsg_pid` 可能是
接收方 port ID，不能要求它恒为零。`NLMSG_ERROR` 必须含完整 `nlmsgerr`。
**队列先于 iptables 建立** —— 消除"配置 ACK 读取与包消息抢socket"的竞态,
且规则挂链瞬间监听已就绪。

两个队列线程共享的 netlink sequence 使用原子递增，避免并发 verdict 产生重复
序号。PACKET 消息即使没有 `NFQA_PAYLOAD`，只要包含 packet id 仍会进入回调并
回复 `NF_ACCEPT`；属性越界、接收截断、回调失败或 verdict `sendto` 失败统一返回
致命错误，由看门狗单次拔除规则并停止引擎，避免报文长期滞留在内核队列。

连接分类使用 `NFQA_CT` 嵌套 `CTA_MARK` / `CTA_MARK_MASK`，仅修改 bit 26..31。
新掩码为 `0xfc000000`，BLOCK/PASS 值分别为 `0xa4000000`、`0xa8000000`，保留
AOSP netd 网络和明文检测位。接管前读取双栈 `iptables-save` 检查外部位冲突；
该检查不等于覆盖 vendor/BPF 的所有隐式行为，仍需设备验收。

### 4.3 应用路由(A/B 代际事务)

**链布局:**
- `ADK_NFQ_ENTRY` 稳定入口链,`OUTPUT` 只挂载这一条 jump
- `ADK_NFQ_OUT_A/B` 两套主拦截链(DNS/TLS/QUIC 规则 + 链尾 RETURN)
- scope=0:白名单 uid 插主链头部 `-m owner --uid-owner N -j RETURN`
  (白名单流量 RETURN 回 OUTPUT → 无后续规则 → 默认 ACCEPT 放行)
- scope=1:`ADK_NFQ_APPS_A/B` 选择器链
  (链内 `uid → -j 同代 ADK_NFQ_OUT_A/B`,链尾 RETURN)

**事务更新:**
```
1. 一次取得 .route.lock；拿不到锁立即返回失败,不继续写规则
2. 读取 IPv4/IPv6 的 ENTRY 活动目标,选择另一套 A/B 代际
3. 在不触碰活动代际的前提下完整构建双栈新主链与 selector
4. 双栈构建成功后依次替换 ENTRY 第一条 jump
5. 任一切换或校验失败 → 两个协议族恢复旧 jump,删除未提交代际
6. 双栈校验成功 → 删除旧代际和旧版本遗留链
```

首次从旧版链迁移时先建好新代际和入口，再挂载 `OUTPUT → ENTRY`，随后清理
旧链；更新过程中不会先清空当前生效链。停止/看门狗清理使用同一把锁并等待
正在进行的短事务结束，避免清理和安装交错。

**状态校验:**`--status` 对 IPv4、IPv6 分别读取规则形式的 `iptables -S`
快照，要求 `OUTPUT → ENTRY` 恰好一条、ENTRY 的 scope 目标与 A/B 代际一致、
DNS UDP/TCP 均进入 queue 0、TCP 80/443 未分类连接均进入 queue 1，并逐项核对
`--queue-bypass`、UID 白/拦截名单、链尾 RETURN 以及 QUIC 快速拒绝队列的启停状态。
缺失、重复、跨代 selector 或旧版直跳链残留均会令 `iptables_active=false`。

**uid 解析三级链:**
1. `/data/system/packages.list`(本地文件,免子进程,最快)
2. `cmd package list packages -U`
3. `pm list packages -U`(兜底)

结果缓存到 `app_uids.conf` / `app_intercept_uids.conf`。

**并发保护**:`.route.lock`(完整双栈路由事务互斥)与 `.state.lock`(状态写互斥)。

### 4.4 规则引擎

- **存储**:FNV-1a 64 位哈希集合(不保留原文,内存优化)。
  黑/白各分 `exact`(精确)与 `suffix`(`||` 后缀)两个集合
- **匹配**:白名单优先;黑名单先精确后逐级后缀
  (`a.b.example.com → b.example.com → example.com`);
  四态判定文案:`PASS(whitelist)` / `BLOCK(suffix)` / `BLOCK(exact)` / `PASS(default)`
- **AdGuard 域名级语法**:
  - 支持:裸域名、`||domain^`、`*.domain` 以及对应的 `@@` 白名单
  - 跳过:`##` `#@#` `#$#` `#%#`(cosmetic/脚本)、`badfilter`、`!`/`#` 注释
  - 拒绝 URL/路径、正则和 `$third-party` 等请求级修饰符,避免扩大成整域名误杀
- **文件**:Hosts 部分与 AdGuard 部分都是 **gzip** 格式，流式解压；单源下载
  最多 64 MiB，解压和多源累计上限 256 MiB、500 万行，超长行(>1024)跳过
- **加载**:启动/SIGHUP 时加载三文件,写 `rule_stats.conf`(black/white 计数,
  即 WebUI 显示的规则数来源)

### 4.5 报文解析(纯函数,无状态)

- **DNS QNAME**:跳过 12 字节头，逐标签解析，压缩指针有界跳转并拒绝循环/越界；
  只检查查询方向(QR=0)，最多检查 4 个 question。TCP DNS 按两字节长度字段重组，
  完整查询后有下一帧半包时及时判定并保留半帧；SYN 数据计入序列号占用
- **TLS SNI**:记录层(type=0x16)→ 握手层(ClientHello)→ 扩展遍历找
  type=0x0000,取 host_name;每一步都有边界校验
- **HTTP Host**:请求方法白名单后逐行检查完整 Host 字段，字段名大小写不敏感，
  去除两端空白和可选数字端口；不把 URL 路径或 X-Host 当成 Host
- **L4 视图**:主机序 hw_proto 为 `0x0800` / `0x86dd`；IPv4 使用 IHL，IPv6
  在固定头后有界遍历常见扩展头。真实分片不重组，atomic fragment 可检查

### 4.6 看门狗与降级

- 队列线程异常(recvmsg/解析/verdict/处理异常)→ 拔除本模块路由 → 引擎退出
- 启动前根据 `/proc` 实际挂载与归属，仅将模块自身移出继承的 v2/v1 freezer
  子组并读回验证；不迁移管理器、不关闭系统冻结、不改独立 CPU/cpuset/stune 策略。
  无法确认隔离成功时拒绝接管；对应行为由 cgroup 单元测试和真实 freezer 集成测试覆盖。
- SIGTERM/SIGINT 优雅停止(拔规则 + state=stopped);SIGHUP 热重载
- 热重载:重建规则池 + 构建非活动路由代际 + 入口提交(失败恢复旧代际)

### 4.7 安全下载器

```
1. 校验 URL:仅 http/https、主机非空、拒绝凭据/控制字符/反斜杠
2. 固定系统路径 dlopen libcurl
   ├─ 仅允许 HTTP(S) 初始协议与重定向协议
   ├─ 最多 5 次重定向,TLS peer/host 校验开启,仅接受最终 2xx
   └─ 写回调与 CURLOPT_MAXFILESIZE_LARGE 双重限制单源 64 MiB
3. libcurl 不可用/失败 → 固定路径 curl 或兼容所需安全选项的 GNU wget
   ├─ fork + execv(argv),URL 始终是单独参数,不经过 shell
   ├─ curl 禁用 curlrc 和 URL glob，限制协议、重定向、状态码、总超时和文件大小
   └─ GNU wget 禁用配置和重定向；子进程 RLIMIT_FSIZE + 完成后文件校验
```

所有后端写入 `0600` 的进程唯一临时文件，只接受非空普通文件；超时、非 2xx、
非法重定向或超过 64 MiB 均删除临时文件，成功后再 rename 到调用方目标。
BusyBox wget 没有这里所需的重定向限制，其内置 TLS 不保证证书验证，故不再作为
安全兜底。可选系统下载组件不是 NDK 保证接口，无可用后端时报告失败并保留旧规则。

### 4.8 规则 generation 原子发布

`--update` 全程持有 `.update.lock`，并发调用直接返回 `busy`。下载归类后，Hosts
与 AdGuard gzip 被写入同一个不可变 staging generation；两个文件都必须通过
引擎的严格加载（要求完整 gzip 流），且合计至少有一条有效规则。校验成功后先将
staging 固化为 generation 目录，最后仅用一次 rename 切换 `rules.current` 指针。

引擎先读取一次指针，再从同一 generation 加载两个文件；generation 发布后不再
修改，因此无须在下载期间阻塞数据面。压缩、写入、校验、目录固化或指针提交任一
失败时，旧指针完全不变；旧版固定文件仍作为没有有效 manifest 时的迁移兜底。

### 4.9 规则源内容校验

每个下载成功的来源独立检查：HTML 特征直接拒绝；其余内容逐行调用与引擎相同的
Hosts/AdGuard 解析器，只聚合实际可加载的域名级规则。空白/注释、有效 Hosts、
有效 AdGuard、超长行和不支持语法分别计数。来源必须至少包含一条有效规则；候选
行达到 20 行时，有效率低于 5% 的内容也会整体拒绝，防止 HTTP 200 错误正文夹带
少量类域名文本后被误判成功。

最近一次更新的每个 URL、下载/内容状态、拒绝原因、字节数、各类行数和 ppm
有效率写入 `source_stats.conf`。字段中的换行会先被清理，失败来源不会向最终
generation 贡献任何规则。

### 4.10 日志隔离与轮转

- daemon：`log/engine.log`，单文件 2 MiB，保留 `.1`、`.2`
- DNS：`log/dns_query.log`，单文件 2 MiB，保留 `.1`、`.2`
- 更新：每个 job 独立 `update_jobs/update-<id>.log`，单文件 1 MiB，超限原位截断；
  只保留最新 8 个已结束 job，运行中的 job 永不清理

`log_line` 每次通过有界追加器打开当前路径，因此轮转 rename 后下一行立即进入新文件，
没有“进程仍持有已删除 inode”问题。daemon、更新 job 和 DNS 不再共享日志文件；旧版
`tmp_update.log` 不再写入，也不会由更新流程删除。

---

## 五、文件系统布局

```
/data/adb/modules/AD_Killer_By_NFQUEUE/          模块安装目录(只读)
├── AD_Killer_By_NFQUEUE                         引擎二进制(arm64)
├── webroot/index.html               WebUI
└── service.sh 等

/data/adb/AD_Killer_By_NFQUEUE/                  运行数据目录
├── state.conf                       状态配置(scope/dev_mode/block_quic/enabled/...)
├── adblock_mode.txt                 旧版本规则强度配置(仅作迁移兜底)
├── hosts_rules.gz / adguard_rules.gz 旧版规则库(无 manifest 时迁移兜底)
├── rules.current                    当前不可变 generation 名称(原子指针)
├── rulesets/<generation>/
│   ├── hosts_rules.gz               同代 Hosts 规则
│   └── adguard_rules.gz             同代 AdGuard 规则
├── custom_rules.txt                 用户自定义规则(WebUI 编辑;@@ 前缀白名单)
├── app_whitelist.txt / app_intercept.txt  应用名单(scope=0 放行 / scope=1 拦截)
├── app_uids.conf / app_intercept_uids.conf  包名→uid 解析缓存
├── update_sources.txt               规则源列表(--update 用)
├── update_jobs/update-<job_id>.{state,log}  独立后台更新任务(结束任务保留 8 个)
├── rule_stats.conf                  规则统计(WebUI 规则数来源)
├── source_stats.conf                最近一次逐来源有效率/拒绝原因
├── runtime.state                    运行时状态(starting/running/stopping/stopped/error)
├── daemon.pid                       守护进程 pid
├── .route.lock / .state.lock / .daemon.lock / .update.lock  并发锁
├── dev_mode_enabled                 dev_mode 标志文件
└── log/
    ├── engine.log[.1/.2]            daemon 日志(2 MiB × 3)
    └── dns_query.log[.1/.2]         DNS 查询日志(2 MiB × 3)
```

**runtime.state 状态机**（由 CLI 读取，WebUI 通过 `--status-fast` 等待启停完成）:
`starting → running | error`,`stopping → stopped`。

---

## 六、WebUI 协议

WebUI 源码位于根目录 `frontend/`（Vue 3）；模块打包时由 Vite 构建为离线单文件
`module/webroot/index.html`，不再手工维护生成的 HTML。页面、组件、组合式状态和
桥接服务分别维护，布局按可用宽高适配直板、折叠内外屏及平板；详见
[前端开发说明](../frontend/README.md)。

界面含概览、应用名单、规则管理、设置四个常驻页面。GSAP 内联用于短时
页面入场和悬浮导航选中块滑动；不接管滚动、不改变引擎或任务生命周期，减少动态
效果时直接切换。设置包含随包更新日志、关于模块与在线更新。

WebUI 将 KernelSU/APatch 的原生三参数
`exec(command, JSON options, callbackName)` 桥接封装成 Promise，再执行 CLI，
不直接读引擎内存:

| 操作 | 命令 | 判定 |
|------|------|------|
| 状态轮询 | `AD_Killer_By_NFQUEUE --status` | 解析最后一行 JSON 的 `running`/`iptables_active` |
| 版本信息 | `AD_Killer_By_NFQUEUE --version-status` | 严格解析 module/version/versionCode；失败标明随包界面版本，不一致时提示 |
| 启动/停止 | 直接 `--start/--stop` + 轮询 `--status-fast` | 保留即时错误，等到真实 `running`/`stopped`；运行还需完整状态确认路由 |
| 开机自启 | `--autostart on\|off` | 只保存开机策略，不隐式启动或停止 |
| 规则更新 | `--update-start` + `--update-status <job_id>` | 持续等待后端真实终态；刷新页面后从 localStorage 恢复 job |
| 模式切换 | `--mode 1\|2` | 保存配置并提交异步重载；`--status` 的 mode 不是实际应用 ACK |
| 来源管理 | `--sources-list` / `--sources-save` / `--source-test <URL或内置ID>` | 草稿显式保存；测试独立 job，不替换规则，保存成功才重排 |
| 应用名单 | `base64` 写 scope 对应 app_*.txt → `--apps-apply` | 输出含 `deferred` 则提示启动后生效 |
| 自定义规则 | `--config-read custom` → `--config-save custom REVISION` → `--reload` | 保存与热重载分开；冲突或读取失败不覆盖，退出码 0 仍不冒充应用 ACK |
| DNS 日志 | `tail -n 200 log/dns_query.log`(1s 轮询增量) | — |

应用页随 `scope` 切换数据文件和文案：scope=0 编辑 `app_whitelist.txt`（名单
应用放行），scope=1 编辑 `app_intercept.txt`（仅名单应用拦截）。切换范围后 WebUI
立即执行 `--apps-apply`，确保新范围对应的 UID 缓存与路由同步，而不是继续覆盖另一
种 scope 的名单。

---

## 七、构建与测试

```bash
# 依赖:Android NDK r26+(arm64-v8a)、Node.js 24 LTS / 22.12+、npm
NDK=/usr/local/share/android-commandlinetools/ndk/26.3.11579264 ./build.sh

# 产物:dist/AD_Killer_By_NFQUEUE-v<版本>.zip(刷入包)+ dist/AD_Killer_By_NFQUEUE(裸二进制)
```

源码仅依赖 libc/liblog/libz(libcurl 运行时 dlopen,不链接)。
