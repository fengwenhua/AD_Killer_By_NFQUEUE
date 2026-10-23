# ADB 状态检查与卸载残留排查

适用于 AD Killer（模块 ID：`AD_Killer_By_NFQUEUE`）v1.1 及后续版本。用于先检查、保留证据，再决定是否处理，不是自动清理脚本。

## 1. 连接手机与安全边界

电脑安装 Android Platform Tools，手机开启 USB 调试并授权电脑。在电脑终端执行：

```sh
adb devices -l
```

状态应为 `device`。`unauthorized` 需要在手机确认授权，`offline` 需要先解决连接问题。多台设备连接时必须选定序列号；以下 `SERIAL` 替换为上一步显示的目标设备序列号：

```sh
adb -s SERIAL shell
su
id
```

在手机上批准 root 请求，确认 `id` 包含 `uid=0(root)`。**下文除明确标注“电脑终端”的命令外，都在这个手机 root shell 内执行。** 退出 root shell 和 ADB shell 分别执行一次 `exit`。

先设置本次 shell 会话的路径变量，不会修改手机配置：

```sh
ADK_MODDIR=/data/adb/modules/AD_Killer_By_NFQUEUE
ADK_DATADIR=/data/adb/AD_Killer_By_NFQUEUE
ADK_BIN="$ADK_MODDIR/AD_Killer_By_NFQUEUE"
date
getprop ro.product.model
getprop ro.build.version.release
uname -r
cat /proc/uptime
```

排查期间遵守以下边界：

- 不执行 `--start`、`--stop`、`--reload`、更新、安装或卸载命令，不切换配置。
- v1.2.3 及更早版本**不要用 `--status` 做严格只读取证**：这些版本可能在守护进程不存在时自动清理残留防火墙。v1.2.4 起已移除此副作用。跨版本检查仍建议先用 `--status-fast`，再独立核对进程和防火墙。
- 不清空任何 iptables 表，不按队列号、端口号或过期 PID 杀进程；不要删除共享的 `/data/adb` 或其他模块文件。
- 不要在日常手机运行 `make test-engine-integration`、`make test-update-integration` 等一次性 Linux 环境测试。
- 查询失败不等于没有残留。`Permission denied`、命令不支持、锁等待超时均应标记为“未能检查”。不要为了得到“干净”结果而隐藏错误。

## 2. 模块版本和配置

```sh
cat "$ADK_MODDIR/module.prop"
"$ADK_BIN" --version-status
"$ADK_BIN" --status-fast
cat "$ADK_DATADIR/state.conf"
cat "$ADK_DATADIR/runtime.state"
cat "$ADK_DATADIR/engine_error.conf"
```

关注以下字段：

| 字段 | 含义与判断 |
| --- | --- |
| `version` / `versionCode` | `module.prop` 与二进制版本应匹配；不要把电脑上的构建版本当成已安装版本 |
| `running` / `runtime` | 正常启动后应为 `true` / `running`；手动暂停时停止是正常状态 |
| `frozen` / `freezer_state` | 新构建区分正常/冻结/未知；`frozen=null` 或 `freezer_state=unknown` 不能当成未冻结，仍需只读重试 |
| `last_error` / `last_error_code` | 结合日志时间判断；文件为空不代表其他检查可以省略 |
| `iptables_active` | v1.2.3 及更早快速状态仅按进程近似；v1.2.4 起快速状态检查双栈 OUTPUT 唯一入口，但仍不证明代际链完整 |
| `route_check` | v1.2.4 起 `entry` 为入口检查，`full` 为完整双栈布局校验 |
| `daemon_image_current` | v1.2.4 起，运行中为 `false` 表示仍是不同于当前安装文件的旧映像；不能只看 `--version-status` |
| `mode` / `scope` | `mode=2` 为 Hosts + AdGuard；`scope=0` 为全局模式，`scope=1` 为应用名单模式 |
| `dev_mode` | 域名判定日志开关；开启时注意访问记录隐私 |
| `block_quic` | 是否接管并拒绝 UDP 443；不代表能读取 QUIC 内部域名 |
| `autostart` | 是否开机自启；`false` 不影响当前已手动启动的引擎 |

`runtime.state`、`metrics.conf` 都是磁盘快照，退出异常时可能保留旧值，不能单独用来证明进程仍在运行。统计时间持续刷新但流量计数不变时，仍需核对真实 OUTPUT 跳转；参见[软重启生命周期](软重启与守护生命周期.md)。

### 已显式暂停后的结果解读（优化构建）

下面是读取已有结果的说明，不要求在只读取证期间执行启停。
优化后的构建会在停止响应中返回 `cleanup.routes`、`cleanup.process_gone`、
`cleanup.queues`、`cleanup.remaining` 和 `cleanup.errors`；旧安装包可能没有这些字段。

- `routes=clean` 才表示已成功读取双栈规则并确认本模块归属链/引用消失。
  `partial` 是已确认仍有残留，`unknown` 是查询、锁或命令收尾无法核实；空输出和读取失败不是干净。
- `process_gone=true` 同时依赖生命周期锁已释放和进程身份核对，不只看 PID 文件。
- `queues=released` 表示队列已释放；`foreign` 表示经 socket inode 验证是其他监听器，模块不会停止它们。
  `occupied/unknown` 不能当成本模块队列已清理，不能把 netlink port ID 直接当 PID 杀掉。
- 只有整体核实且状态保存成功才返回 `status=stopped`、退出码 0。
  `cleanup_unknown/cleanup_incomplete/cleanup_failed` 会保留诊断，WebUI 显示“停止结果待核实”，允许显式重试暂停。
- `.route.lock` 仍被占用时，可能是旧防火墙命令正在完成。新的命令保留锁租约，父守护被强杀也不会提前放行新实例；
  不要删除或替换锁文件、清空全局防火墙来绕过保护。先保存日志和进程/FD 证据，再决定如何处理。

## 3. 进程、线程、内存和冻结组

先列出匹配的完整命令行，同时检查后台更新工作进程，不只看 PID 文件：

```sh
ps -A -o PID,PPID,NAME,ARGS | grep -E '[A]D_Killer_By_NFQUEUE|[A]DK_NFQ'
cat "$ADK_DATADIR/daemon.pid"
```

正常守护进程的参数包含 `AD_Killer_By_NFQUEUEd --daemon`；`/proc/PID/comm` 的短名是 `ADK_NFQd`。不同 `ps` 实现显示名称可能不同。如果 `ps` 不支持这些列，可用 `ps -A`，再通过 `/proc` 核实。

一次性或并行 ADB 查询命令自身也可能在参数中包含模块路径。匹配到 `sh`、`su`、`grep` 等检查命令时，需要核对完整参数和实际执行文件，不能仅凭字符串匹配就认定是模块残留。

读取 PID 文件后先做格式检查，再核对实际进程身份：

```sh
ADK_PID=$(cat "$ADK_DATADIR/daemon.pid" 2>/dev/null)
case "$ADK_PID" in
    ''|*[!0-9]*|0) printf '%s\n' 'PID 缺失或无效；请从 ps 输出核对，不要继续使用此 PID' ;;
    *)
        cat "/proc/$ADK_PID/comm"
        tr '\000' ' ' < "/proc/$ADK_PID/cmdline"
        printf '\n'
        readlink "/proc/$ADK_PID/exe"
        ;;
esac
```

只有确认这是本模块守护进程后，才继续下面的检查。若文件不存在、PID 被其他程序复用，或身份不一致，先记录异常；**不能据此杀进程**。只有 `daemon.pid` 文件而没有对应进程，也不代表引擎仍在运行。

```sh
cat "/proc/$ADK_PID/status"
ps -T -p "$ADK_PID" -o PID,TID,STAT,NAME
cat "/proc/$ADK_PID/cgroup"
grep cgroup /proc/mounts
```

正常稳态通常为主线程 + 两个队列线程，共 3 个线程。`S`（睡眠等待）本身正常，不等于卡死；结合计数器是否继续更新判断。`VmRSS` 为当前驻留内存，`VmHWM` 为峰值，不能把较大的虚拟地址空间 `VmSize` 当作实际占用内存。

冻结组应结合实际 cgroup 挂载点和 `--status-fast` 判断。v1 freezer 成员位于 `/`、v2 成员位于 `/` 是已脱离对应应用子组的证据；不要假设所有手机都在同一路径提供 `cgroup.freeze` 或 `freezer.state`，根组也可能没有这些文件。无需为了排查写入 `cgroup.procs` 或改变系统冻结策略。

可选的短时 CPU 观察：

```sh
top -b -n 2 -d 2 -p "$ADK_PID"
```

若手机 `top` 不支持这些参数，改用其支持的只读方式；不同工具的 CPU 百分比口径可能不同。单次采样不能证明全天稳定性。

## 4. 引擎日志、规则同步和任务

```sh
ls -lh "$ADK_DATADIR/log"
tail -n 100 "$ADK_DATADIR/log/engine.log"
cat "$ADK_DATADIR/rule_stats.conf"
cat "$ADK_DATADIR/source_stats.conf"
ls -lt "$ADK_DATADIR/update_jobs"
grep -H . "$ADK_DATADIR"/update_jobs/*.state
```

重点看最近一次启动、热重载、规则发布的时间和结果。首次没有规则时的“全放行”警告，不代表后续同步成功后仍为空；以最新加载记录和当前计数为准。

- `publish_status=published` 表示一次规则快照发布成功；`accepted_sources` / `source_count` 显示接受来源数。
- 浏览器过滤列表中不支持的元素隐藏、脚本等语法会被过滤，`filtered_lines` 非零不能直接判为下载失败。
- 任务 `.state` 保存 `success:0`、`failed:1` 等最终结果。旧失败记录可能是单源测试，应检查同 ID 的 `.log`，不要据此认定当前引擎故障。
- 手动查看某个任务时，把下面的 `JOB_ID` 替换为上一步文件名里的实际 ID；不要照抄历史任务 ID。

```sh
tail -n 80 "$ADK_DATADIR/update_jobs/update-JOB_ID.log"
```

域名判定日志只有开发日志开启且产生对应流量时才会更新。**里面可能包含个人访问域名，不要未经脱敏上传到公开 Issue。** 如需本地查看：

```sh
tail -n 50 "$ADK_DATADIR/log/dns_query.log"
```

这个文件名虽然叫 `dns_query.log`，当前实现也会记录成功提取出的 HTTP/TLS 域名判定。`BLOCK(exact)` / `BLOCK(suffix)` 是规则命中，`PASS(default)` 是未命中默认放行；没有日志不能单独证明引擎没有处理数据。

## 5. NFQUEUE 队列与数据面指标

```sh
cat /proc/net/netfilter/nfnetlink_queue
cat "$ADK_DATADIR/metrics.conf"
```

本模块运行时使用 queue 0 处理 DNS、queue 1 处理 HTTP/TLS 和启用时的 QUIC 拒绝。以下数字只是格式示例：

```text
    0  12345     0 2  4096     0     0      500  1
    1 4000000000 0 2  4096     0     0     2000  1
```

Linux 6.6 的列定义见内核 [`nfnetlink_queue.c` 的 `seq_show()`](https://github.com/torvalds/linux/blob/v6.6/net/netfilter/nfnetlink_queue.c#L1440-L1448)；厂商内核如有改动，应核对对应源码：

| 列 | 含义 |
| --- | --- |
| 1 | 队列编号 |
| 2 | 监听 socket 的 Netlink port ID，**不一定是进程 PID**；大整数并不异常 |
| 3 | 当前排队、等待判决的包数；瞬时非零不一定异常，持续增长才需警惕 |
| 4 / 5 | 复制模式 / 复制长度，本模块通常为 `2` / `4096` |
| 6 / 7 | 队列丢包 / 向用户态投递失败的丢包计数，关注是否持续增长 |
| 8 | 包 ID 序列，不能当成业务请求数；监听器重建时会重置，长期运行还可能回绕 |
| 9 | 上述内核实现固定输出 `1`，不是进程数或健康标志 |

只看到 queue 0/1 不能证明归本模块所有。需要关联 `/proc/net/netlink` 中协议 `12`、对应 port ID 的 socket inode，再与已确认身份的守护进程 FD 比较：

```sh
cat /proc/net/netlink
ls -l "/proc/$ADK_PID/fd"
```

FD 的 `socket:[inode]` 与 netlink 记录匹配，才能确认归属。不要把第二列直接当作 `/proc/PID` 或杀进程的目标。

在手机自然产生网络流量时，间隔 10–30 秒手动再执行一次本节查询，比较以下指标。无需启停引擎、修改规则或制造压力：

| 指标 | 观察要点 |
| --- | --- |
| `updated_at` | 引擎稳态下应持续刷新；还要排除手机时间变化 |
| `dns_packets` / `tls_packets` | 有相关流量时应增长；不是所有应用流量都能识别到域名 |
| `block_exact_hits` / `block_suffix_hits` | 命中计数，不是去重后的广告数量 |
| `dns_negative_replies` / `tcp_resets` / `quic_rejects` | 快速拒绝的执行计数，QUIC 取决于开关与实际流量 |
| `queue_errors` / `verdict_failures` / `response_failures` | 正常预期为 0；新增或持续增长应进一步排查 |
| `domain_parse_failures` | 提取域名失败后存在安全放行路径；非零不等同于崩溃，但也不能宣称全部流量都已识别 |
| `reassembly_flows` / `reassembly_bytes` | 握手重组的瞬时状态；关注是否有界、空闲后是否回落 |
| `reassembly_expired` | 未完成重组状态的超时清理计数，不等于队列丢包 |

零丢包也不证明所有流量都被检查：queue-bypass、fail-open、作用范围和协议边界可能导致放行。

## 6. IPv4 / IPv6 防火墙

先确认两条查询都成功，不要一上来隐藏错误或只看筛选后的空输出：

```sh
iptables --version
ip6tables --version
iptables -w 5 -t mangle -S
ip6tables -w 5 -t mangle -S
```

引擎运行时，通常可见 `OUTPUT` 跳转至 `ADK_NFQ_ENTRY`，再连接当前代际链，例如 `ADK_NFQ_OUT_A` / `ADK_NFQ_OUT_B`；应用名单模式还会有 `ADK_NFQ_APPS_*` 链。只应有当前有效路径，不能只看链名存在。

确认完整查询成功后，可筛选本模块规则：

```sh
iptables -w 5 -t mangle -S | grep ADK_NFQ_
ip6tables -w 5 -t mangle -S | grep ADK_NFQ_
```

查看命中计数用下面的只读命令，**不要加 `-Z` 清零**：

```sh
iptables -w 5 -t mangle -L -n -v -x
ip6tables -w 5 -t mangle -L -n -v -x
```

核对 DNS queue 0、HTTP/TLS queue 1、`--queue-bypass`、连接标记的 DROP/RETURN 顺序，以及 QUIC 开关对应的 UDP 443 规则。某条规则零命中可能只是没有匹配流量；IPv6 链存在也不能代替真实 IPv6 功能验证。`-L` 与 `-S` 输出格式不同，不要求逐字一样。

## 7. 卸载前后怎么查残留

### 卸载前：先保存基线

记录日期、版本、已确认的守护 PID、进程路径、队列记录、双栈规则及计数。保存本机记录时注意域名和其他应用信息脱敏，不将真实设备序列号或访问历史提交到仓库。

在根管理器中由用户操作卸载。**检查不代替卸载，也不代替重启。** 区分两个阶段：

1. 标记卸载后、重启前：先检查一次，但模块目录和进程可能尚未完成移除，不能直接宣称卸载成功。
2. 用户自行按管理器要求重启后：重新连接 ADB、进入 root shell、重新设置第 1 节路径变量，再做最终验收；重启会让旧 PID 失效。

### 卸载后：即使二进制已消失，也能独立检查

**不要再执行旧二进制的 `--status`，否则可能在取证前顺手清理现场。** 直接查询系统：

```sh
ps -A -o PID,PPID,NAME,ARGS | grep -E '[A]D_Killer_By_NFQUEUE|[A]DK_NFQ'
cat /proc/net/netfilter/nfnetlink_queue
iptables -w 5 -t mangle -S
ip6tables -w 5 -t mangle -S
ls -ld /data/adb/modules/AD_Killer_By_NFQUEUE
ls -ld /data/adb/modules_update/AD_Killer_By_NFQUEUE
ls -ld "$ADK_DATADIR"
ls -l "$ADK_DATADIR/daemon.pid"
ls -l /data/adb/.AD_Killer_By_NFQUEUE.install.lock
```

目录和 PID 文件不存在时，`ls` 报 `No such file or directory` 是相应检查的预期结果，不是整套检查失败。但权限错误不能当成不存在。若进程还在，即使 `readlink /proc/PID/exe` 显示 `(deleted)`，仍属于活跃进程残留，不能只根据模块目录已删除判断完成。

如需排查手动安装过的额外自启脚本，可检查常见共享脚本目录，**只读搜索，不删除任何脚本**：

```sh
for ADK_HOOKDIR in /data/adb/service.d /data/adb/post-fs-data.d; do
    if [ -d "$ADK_HOOKDIR" ]; then
        grep -R -n -E 'AD_Killer_By_NFQUEUE|ADK_NFQ_' "$ADK_HOOKDIR"
    fi
done
```

本项目标准安装通过模块自身 `service.sh` 自启，不额外在上述共享目录安装脚本。这是针对常见手动自启的补充检查，不覆盖用户自定义的所有启动机制；匹配结果也需要人工确认归属。

### 验收标准：运行残留与数据保留分开判定

| 检查项 | 完成卸载并重启后的预期 |
| --- | --- |
| 守护进程和后台工作进程 | 无本模块活跃进程；不要只检查旧 PID |
| NFQUEUE | 无本模块监听 socket；其他程序的队列不能算作本模块残留，也不能擅自关闭 |
| IPv4 / IPv6 mangle 表 | 两次完整查询成功，均无 `ADK_NFQ_` 链及引用；系统和其他模块的规则保留 |
| 已安装目录 / 待更新目录 | 两个模块专属目录均不存在；标记卸载但未重启阶段单独记录 |
| `daemon.pid` | 正常卸载钩子会删除；残留文件可能是旧记录，需与真实进程区分 |
| `/data/adb/.AD_Killer_By_NFQUEUE.install.lock` | 安装初始化创建的模块专属空锁文件，当前卸载钩子不删除；文件存在不代表锁仍被持有，要求彻底清理时需单独核实 |
| `/data/adb/AD_Killer_By_NFQUEUE/` | **当前卸载脚本刻意保留，方便重装复用规则，不属于运行残留** |
| 日志、规则快照、更新任务记录和下载包 | 可能随数据目录保留；记录体积与隐私影响，不当成活跃进程 |
| `runtime.state` / `metrics.conf` 等 | 保留数据里的历史快照可以仍显示旧状态；不作为卸载后存活证据 |
| 自启脚本引用 | 不应有仍能启动本模块的额外有效引用；发现后先核实，不自动删除 |
| 网络 | 用户检查常用网页和应用的新连接；涉及 IPv6、VPN、代理时分别说明验证范围 |

`/proc/net/netfilter/nfnetlink_queue` 文件本身仍存在、内核 NFQUEUE 支持仍加载，都不等于本模块残留。正常观察不应尝试卸载内核模块，也不要清空 conntrack；已有连接标记可能暂留至连接结束，缺少本模块规则时不能单独据此判定仍在拦截。

检查安装锁时，可用 `stat -c '%F %s %i' /data/adb/.AD_Killer_By_NFQUEUE.install.lock` 获取类型、大小和 inode，再结合 `/proc/locks` 中的设备号与 inode 核实是否仍有活跃锁。不要把空文件当成活跃安装，也不要在安装或升级进行中删除锁文件。仅在用户明确要求彻底清理且确认未使用后，单独删除这个精确路径，不动共享父目录。

**发现进程或路由残留时先报告证据，再另行确认清理方案。** 本文不提供一键杀进程、清空防火墙或删除数据的命令。若用户要求“彻底删除所有数据”，需额外确认备份与删除范围；管理器卸载不等于数据擦除。

## 8. 建议的检查结论模板

```text
时间 / 模块版本 / 系统与内核：
检查阶段：运行中 / 已标记卸载但未重启 / 卸载并重启后
进程：实际路径、参数、是否被冻结、线程和内存
NFQUEUE：归属、积压、两类丢包计数及观察期间变化
防火墙：IPv4 与 IPv6 是否查询成功、是否有本模块链与引用
规则与日志：最近一次同步、最新异常、错误是否仍在增长
目录：已安装目录、待更新目录、PID 文件、刻意保留的数据
网络：用户已验证的场景、尚未覆盖的协议或网络
结论：正常 / 存在运行残留 / 仅保留数据 / 证据不足
本次是否修改设备：否；如需清理，单独申请确认
```

只读状态检查不能替代长时间稳定性测试或真实拦截验收。需要更全面的协议与故障测试时，参阅[真机上线审计与验收方案](真机上线审计与验收方案.md)。
