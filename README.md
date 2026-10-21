# AD_Killer_By_NFQUEUE

> [!IMPORTANT]
> 本项目主要参考月虹分享页中的 [YH_Hosts](https://yhyun.asia/)，是基于其产品与功能思路完成的独立开源实现。项目代码主要由大模型辅助，以 Vibe Coding 方式完成。本项目与 YH_Hosts 原作者无隶属或官方合作关系，仅供个人学习、研究与技术交流使用。不保证适用于任何生产环境，也不对误拦截、断网、数据损失、设备异常或其他直接/间接损失承担责任。使用前请自行审查代码、规则源及许可证，并做好备份。

基于 Linux 内核 **NFQUEUE** 的 Android 系统级去广告模块。核心引擎使用 C++ 实现，支持 KernelSU、Magisk 和 APatch，并提供离线 WebUI。

## 核心特性

- 非 VPN 方案：通过 NFQUEUE 处理 DNS、HTTP、TLS 和 QUIC 流量。
- 纯本地引擎：运行时无外部服务依赖，支持自定义 Hosts / AdGuard 规则。
- 快速失败：命中后返回 NXDOMAIN、TCP reset 或 ICMP 不可达，减少应用等待超时。
- 原子更新：规则下载、校验、去重和发布失败时保留当前可用规则。
- 灵活范围：支持全局拦截、按应用拦截、应用白名单和 QUIC 开关。
- 防断网设计：队列冲突拒绝启动，并使用 fail-open、queue-bypass 和链尾兜底。
- 可视化管理：WebUI 支持状态、规则源、应用范围、日志和单源测试。

## 环境要求

- Android 12（API 31）及以上。
- arm64-v8a 设备。
- KernelSU、Magisk 或 APatch。
- 内核需支持 NFQUEUE、conntrack、mangle、owner 和 queue-bypass。

> [!WARNING]
> NFQUEUE 0/1 被其他程序占用时，模块会拒绝启动。如已安装 YH_Hosts 或其他同类模块，请先停用、卸载并重启设备。

## 安装与使用

1. 从 [Releases](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases) 下载最新 ZIP。
2. 在根权管理器中刷入模块并重启。
3. 打开 WebUI，在“规则”中保存来源，回到概览页执行同步。
4. 确认规则正常后手动启动引擎；首次安装默认暂停且不开机自启。

运行数据保存在 `/data/adb/AD_Killer_By_NFQUEUE/`。规则源最多支持 64 个普通 HTTP(S) 地址，另提供默认停用的 HaGeZi Pro − Fake 内置来源。

在“设置 → 检查更新”中下载校验并确认安装，可选预览版与自动检查；自动检查默认关闭，不自动安装。更新日志读取 GitHub 最新 Release。安装后需手动重启，v1.0 首次升级需在根管理器中手动刷入完整 ZIP。

## 本地构建

需要 Android NDK r26+、Node.js 22.12+ / 24 LTS、npm 和 Python 3：

```bash
NDK=/path/to/android-ndk ./build.sh
python3 tests/verify_checksum.py dist/AD_Killer_By_NFQUEUE-v1.2.1.zip
```

构建产物位于 `dist/`。版本号仅在 [`module/module.prop`](module/module.prop) 中维护。
`main` 分支的完整 CI 全部通过后会自动发布对应版本的 prerelease，经人工验证后再在 GitHub 转为正式版。

## 测试

```bash
npm --prefix frontend ci --include=dev
make test
```

主机测试不会修改本机防火墙。需要 root 的真实 NFQUEUE 测试仅应在一次性 Linux 环境执行。

## 文档

- [实现文档](docs/AD_Killer_By_NFQUEUE实现文档.md)
- [开源规则合并指南](docs/开源规则合并指南.md)
- [HaGeZi 内置特殊来源](docs/HaGeZi内置特殊来源.md)
- [兼容性与协议限制](docs/兼容性与协议限制.md)
- [模块更新](docs/模块更新.md)
- [ADB 状态检查与卸载残留排查](docs/ADB状态检查与卸载残留排查.md)
- [真机上线审计与验收方案](docs/真机上线审计与验收方案.md)
- [第三方规则与许可](docs/第三方规则与许可.md)

## 许可证

项目自有代码使用 [MIT License](LICENSE)。第三方库与规则源仍需遵守各自的许可条款。
