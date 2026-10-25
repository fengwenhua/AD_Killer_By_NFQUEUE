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
│   ├── source_sync.cpp         来源预算、条件请求、完整校验与整组回退
│   ├── source_cache.cpp        校验后单源/组合缓存、原子指针与有界回收
│   ├── source_staging.cpp      下载暂存目录、跨 exec 写入租约与崩溃回收
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
| `--reload [custom FILE_REVISION]` | 提交热重载 | 返回 `queued/deferred + revision`，不是生效 ACK；可绑定刚保存的自定义文件修订号 |
| `--apply-status [REVISION]` | 查询生效回执 | 只读返回 saved/pending/applying/applied/failed/deferred/superseded/unverified；回执绑定 boot ID、PID、启动时间 |
| `--update` | 拉取规则源 | 下载(支持 .gz)→ 逐行归类 Hosts/AdGuard → gzip 落盘 → 热重载 |
| `--update-start` | 后台更新 | 创建唯一 job，脱离 WebUI exec 会话运行并返回 job_id |
| `--update-status ID` | 更新状态 | 查询 job 的 running/success/failed/interrupted 与真实退出码 |
| `--status` | 状态 JSON | 按回执中的实际生效配置验证双栈路由；配置字段表示磁盘期望，`apply` 表示生效结果，规则数优先取有效回执 |
| `--status-fast` | 快速状态 | 只读核验双栈唯一 OUTPUT 入口，不等同完整路由校验；也带 `apply` 回执 |
| `--version-status` | 版本 JSON | module/version/versionCode |
| `--mode 1\|2` | 规则强度 | 1=仅 Hosts 2=Hosts+AdGuard;写配置 + 运行中热重载 |
| `--scope 0\|1` | 作用域 | 严格校验参数;0=全局拦截 1=仅名单内应用;运行中触发路由热重载 |
| `--dev on\|off` | DNS 查询日志 | 写 `dev_mode_enabled` 标志文件;输出 `{"dev_mode": "done"}` |
| `--quic on\|off` | 阻止 QUIC | 通知守护进程重建规则(热重载);输出 `{"block_quic": %s}` |
| `--autostart on\|off` | 开机自启 | 独立配置，默认关闭；只持久化策略，不启动、停止或重载当前引擎 |
| `--apps-apply [allow\|intercept FILE_REVISION]` | 提交应用名单 | 生成附带名单修订号的 UID 缓存，返回 `queued/deferred + revision + scope`；不是已生效 |
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
- 热重载：按 apps → editable → state 锁序捕获一致候选，构建期间使用旧策略；
  写栅栏排空旧判定后，切换期间队列仅无 connmark 旁路；双栈路由成功才发布快照，
  两条接收 socket 都观察到提交后的真正空闲才恢复判定、记录完成。
  普通回滚保留旧快照/统计/重组缓存；回滚或收包边界无法确认则故障退出。
  已有连接标记不清空，具体边界见[兼容性与协议限制](兼容性与协议限制.md)。

### 4.6.1 配置应用回执

`apply.state` 使用独立终端锁、原子替换及文件/目录同步。路由输入写入者必须在
各自配置锁内，先持久化新的 dirty 意图，再替换文件；输入全部保存后，在
apps → editable → state 锁序下提交新的应用修订号。修订号是唯一的保存/提交意图
标识，不是整个文件集合的内容哈希；可编辑文件的 SHA-256 CAS 修订号另行绑定。

引擎在同一锁序下捕获修订号、模式、路由参数、名单缓存、generation 和自定义文本；
解压/哈希/排序在锁外完成，不让大规则文件长时间阻塞暂停或保存。配置读取失败、
重复/非法键、非法 UID、缓存与当前名单不符、坏 UTF-8 或非法自定义规则均拒绝应用，
不以默认/空值代替。旧版无来源修订号的 UID 缓存仅在包名集合严格一致时兼容。
缓存来源修订号不代表能发现 Android 重新分配 UID；包安装变更、多用户等仍归 O10。
非空名单但无法取得任何已安装包 UID 时拒绝写缓存，不冒充“未安装”并发布空映射；
PackageManager 兜底采用有界 argv 调用（每个 2 秒、最多 8 MiB），空名单不查询系统。

成功切换且收包恢复后记录 `applied_revision`、实际路由参数、规则 generation/数量，
失败记录原因并保留上一次实际策略。并发保存不会被旧操作的完成覆盖；新实例不能
继承旧实例 ACK。已提交但 SIGHUP 未送达的请求由引擎每秒协调一次；dirty 尚未提交
则不会自动应用。回执无法持久化时故障退出，不伪造成功。

`--apply-status` 的 `verification=owner_and_commit` 只证明匹配当前守护身份的最近
提交，不是全套网络健康检查；完整路由/冻结/进程检查仍使用 `--status`。
只读查询不创建锁或修复文件。外部 root 工具绕过配置锁直接改文件不属于该协议，
必须显式提交 `--reload`；损坏回执应先保留诊断、按实际状态恢复，不能盲目删除锁。

规则同步只更新 `available_rule_stats.conf`（已发布文件），不能覆盖活跃快照统计。
前端在保存后观察本次提交修订号的 ACK，最多 9 秒或 24 次查询，连续三次查询错误
即停止观察；关闭页面不会继续派发隐藏操作，也不会取消后台应用。排队、保存、
失败或结果未知均不提示“已生效”。编辑器绑定文件 CAS 修订号，观察器绑定独立的
提交修订号；范围切换只提交一次后端操作，不再额外重复应用名单。

概览将待应用/失败与健康拦截区分，显示保留的实际模式、范围、QUIC 和快照计数；
缺少可信回执或完整路由核验时不显示绿色健康。快速指标观察只能降级，不能代替
完整核验升级健康状态。用户可只读核实，或显式“重新应用配置”，后者不重启守护。
状态丢失或界面关闭后仍可重新进入概览核实，不能把观察超时当作后端失败。

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

引擎在短时 rulesets 目录共享锁下选择一次指针并取得 generation 目录共享租约，
释放目录锁后再加载两个文件；租约随 `RuleBundlePaths` 副本存活，最后一次加载结束
才释放，不创建可被错误删除/重建的锁文件。generation 发布后不再修改，无须在
下载或大文件加载期间阻塞数据面；长时间 staging 写入也不持有目录协调锁。

相同的两份压缩内容复用当前代际，不新增目录或轮换回退点。不同内容先持久化
旧当前指针到 `rules.current.previous`，再发布新当前指针。文件、目录同步错误均
传播；若 rename 已可见但父目录同步失败，报告结果未确认，不删除已可见的新代际，
也不谎称旧指针完全不变。缺失 manifest 才允许旧固定文件迁移，损坏指针拒绝加载。

正常同步前后执行历史回收：默认最多 4 代、256 MiB 压缩规则、额外历史最多 7 天，
至少保护当前/上一代和全部读写租约；这些保护可以临时超过预算，会记录诊断，
不能为满足容量数字破坏活跃加载。未满 24 小时的 staging 保留，活动 staging 无论
年龄均保留。单次最多扫描 4096 项；读取/保护目标验证失败不清理，异常项跳过并
记录，故此预算不承诺约束人为放入的未知文件。清理仅涉及本模块拥有的无符号链接
目录和两份预期普通文件，不递归删除。删除前原子改名为 `.gc-<generation>`，
中断后可继续回收已部分删除的 tombstone。无同步时不额外启动后台清理进程。

staging 前检查可用空间，除新两份文件外保留 16 MiB 余量，空间读取失败也拒绝写入；
每份压缩文件仍限 64 MiB。该门槛不是空间预留，后续 ENOSPC 同样按失败处理。
GC 不自动回滚配置；需要恢复规则时应先暂停并保留诊断，再按实际状态操作。

### 4.9 规则源内容校验

每个下载成功的来源独立检查：HTML 特征直接拒绝；其余内容逐行调用与引擎相同的
Hosts/AdGuard 解析器，只聚合实际可加载的域名级规则。空白/注释、有效 Hosts、
有效 AdGuard、超长行和不支持语法分别计数。来源必须至少包含一条有效规则；候选
行达到 20 行时，AUTO 有效率低于 5% 的内容也会整体拒绝，防止 HTTP 200 错误正文夹带
少量类域名文本后被误判成功。显式浏览器列表配方允许提取非空域名子集，不套用
AUTO 的比例门槛，HTML、gzip 和资源保护保持不变。

最近一次更新的每个 URL、下载/内容状态、拒绝原因、字节数、各类行数和 ppm
有效率写入 `source_stats.conf`，另记录 downloaded/revalidated/cache_fallback、缓存
核验/获取时间与失败原因。整次任务 10 分钟、单源 90 秒，网络阶段留出本地校验余量；
正文累计 256 MiB、解压累计 256 MiB/500 万行，失败输入也计入预算。
缓存加载后及内容解析后都复核摘要，避免入口校验与实际使用间发生损坏。304 必须绑定
同一最终 URL；HaGeZi 必须整组提交或整组回退。失败不能取得 24 小时内合格缓存时
停止后续来源，未请求项明确标记，不发布成功子集。字段中的换行会先被清理。
完整策略、暂存租约及空间边界见[规则同步策略](规则同步策略.md)。

### 4.10 日志隔离与轮转

- daemon：`log/engine.log`，单文件 2 MiB，保留 `.1`、`.2`
- DNS：`log/dns_query.log`，单文件 2 MiB，保留 `.1`、`.2`
- 更新：每个 job 独立 `update_jobs/update-<id>.log`，单文件 1 MiB，超限原位截断；
  保留最新 8 个可确认结束 job，另保护最近两类任务的恢复索引；持有租约或未知任务不清理

`log_line` 每次通过有界追加器打开当前路径，因此轮转 rename 后下一行立即进入新文件，
没有“进程仍持有已删除 inode”问题。daemon、更新 job 和 DNS 不再共享日志文件；旧版
`tmp_update.log` 不再写入，也不会由更新流程删除。

#### 4.10.1 后台任务身份与恢复索引

新任务写入有界 8 KiB 的 `ADK_JOB_2` 日志，绑定任务 ID、类型、目标/渠道、
boot ID、PID、进程启动时间与可执行映像的设备号/inode。启动派发前先持久化
任务记录和 `.active-rules` / `.active-module` 恢复索引，再放行子进程；失败不派发。
同一任务族在短时 `.job-admission.lock` 内去重，相同活动请求返回已有 job ID，
不同请求或身份/租约不确定时返回 busy，不盲目创建第二个 worker。

`--update-status ID` 只读核验身份，不向 PID 发送信号；PID 被复用、旧启动或映像
不匹配不能显示 running。旧版 PID-only 记录只能视为中断；近期旧记录清理前有
24 小时迁移宽限期。`--jobs-list` 读取后端索引，即使 WebUI 丢失 localStorage
仍可发现最近/活动任务；索引损坏不返回伪造空列表，不自动删除诊断记录。

worker 持有独立 `.lease`，网络命令和安装器跨 exec 继承租约。父 worker 被杀不
等于后代已经停止；未知后代仍持有租约时状态为未核实，也不回收数据或放行新任务。
终态已经写入但租约未释放时显示 finishing，全部持有者退出后才宣布结束。
安装器派发前持久化 installing：即使安装器自行关闭继承描述符，所有者异常消失也
不能证明安装已完成；同次启动保留未核实屏障，需核查管理器，或在可确认的新启动
后恢复。只读状态核验不杀进程、重启守护或自动安装。

阶段、开始/更新时间和终态原因采用文件/目录同步写入；时间为展示用墙钟，不能
用“时间久未变化”推断任务死亡。正常保留清理只针对预期普通文件、空闲租约和
可确认终态，扫描限 4096 项，不跟随符号链接。这些任务保证不代表完整卸载已经实施。

WebUI 的规则同步、单源测试和模块下载/安装共用同一个观察器：先从后端索引恢复，
localStorage 只作辅助；未知索引不伪装为没有任务。普通观察逐渐降低到 5 秒周期，
连续查询失败指数退避（上限 8 秒），5 次仍失败即暂停并保留可恢复身份。
“继续查询”只发现/查询任务，不重新派发；未知任务未核实前禁止盲目下载/安装。
页面进入后台后暂停新的 bridge 查询，关闭界面后不再读取日志或反馈结果，也不会
取消 worker。未保存或已移除来源的测试单独显示恢复日志，不加入来源配置。

#### 4.10.2 有界任务日志页

`--update-log ID [CURSOR|tail]` 只允许固定任务目标，每页最多 16 KiB 原始字节；
日志须为本模块拥有的、非符号链接/硬链接、最大 1 MiB 的普通文件。数据采用 base64，
前端流式 UTF-8 解码，因此中文跨页、NUL 或安装器的非法编码不会破坏 JSON。
读取错误、缺失/正在轮转和合法空片段分别处理，不以失败读取伪造成功空文件。

新任务日志在创建/轮转时写入唯一 generation 头，游标绑定 generation 和字节偏移；
截断后重新长大也能识别轮转，不仅依赖 inode 或文件大小。读完后再次核对头，
不推进混合代际游标。旧日志只作有界快照兼容。查询不写入任务状态或日志。
前端仅保留末尾 64 Ki 字符供展示；任务结束且仍有大量积压时最多再读一次末尾页，
明确提示省略/轮转，不宣称已显示完整历史。日志读取失败与任务最终结果分开呈现。

#### 4.10.3 完整卸载与写入栅栏

所有会写入运行数据的 CLI 入口，先取得数据目录外
`/data/adb/.AD_Killer_By_NFQUEUE.operations.lock` 的共享租约，并在租约内复核
实际运行映像和模块目录 `.uninstalling` 标记。目录/锁/映像不可核实时拒绝派发，
不用陈旧二进制路径的新 inode 冒充正在运行的映像。普通状态/日志/来源列表查询
不创建数据目录；读取失败不会触发隐式初始化。

已接受的 CLI、daemon、后台任务和外部命令持有租约直至退出。网络/安装 exec 与
防火墙/包管理器的 posix_spawn 均继承租约；父进程被杀也不能提前解除外部写入者。
锁按 open-file-description 生命周期关闭，不用显式解锁破坏后代持有者。安装临界
阶段与任务 admission 使用同一短锁，卸载请求须在该锁内核实 installing 屏障；
不能在安装器进入临界阶段后强行取消。关闭入口后，worker 不再进入新的发布/安装
阶段，但允许记录最终结果，以便在失败留存数据时仍有诊断。

`--uninstall` 仅允许 root、固定模块及数据目录。先在任务 admission 锁内核实安装
屏障，再持久化 `.uninstalling` 关闭入口；取消网络请求并停止守护，核实双栈模块
路由和队列释放。已派发安装不可自动强杀，未知安装状态在关闭入口前返回 busy。
随后等待所有已接受写入者释放租约，取得独占操作租约，检查旧进程/写句柄及目录
挂载边界，取得目录、规则读取者和文件锁的独占租约后，才删除本模块全部数据。
遍历有时间/深度/条目/目录数上限，使用 fd-relative API，不跟随 symlink，不穿越
挂载点；硬链接仅解除数据目录内的链接。最后清理外部安装锁与操作锁并同步父目录。
标记保留至管理器移除模块，防止迟到请求重建数据；升级仍保留数据。

未知所有者、安装状态、权限、活跃租约或清理失败均返回非零并保留诊断/剩余数据。
二进制缺失/损坏时脚本只做有界只读诊断，不猜 PID、不强删锁、不手动绕过屏障。
根管理器未必因卸载钩子失败而取消模块移除，因此仍须按 ADB 手册验收；需要时重新
安装匹配版本后重试。详见[完整卸载实现](完整卸载实现.md)。

---

## 4.11 观测指标与日志

指标采用内容变化发布和空闲 15 秒心跳；DNS 查询日志有界异步批量写入，收包路径不等待日志盘，丢弃与未确认写入分别有诊断计数。日志开关跟随已确认策略，退出先释放队列和路由。见[指标与日志开销](指标与日志开销.md)。

概览显示本次守护的“规则命中”；响应发送、拒绝 verdict 提交和匹配各有独立口径。新快照绑定 boot/PID/启动时间，以单调时间判断年龄；未知归属不显示旧数字，过期与查询失败明确提示保留记录。见[数据面指标口径](数据面指标口径.md)。

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
├── source_cache/                    校验后来源缓存/组合元数据、原子指针
├── source_staging/job-*/            下载私有暂存，目录租约跨 exec 继承，安全回收崩溃残留
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
