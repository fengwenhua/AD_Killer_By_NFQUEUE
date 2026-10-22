# KernelSU 原生更新

无需先打开 AD Killer。KernelSU 在模块列表通过已安装的 `module.prop` 中的 `updateJson`，获取最新正式版并显示更新入口；用户确认后由管理器下载、安装，安装后仍需手动重启。

## 使用

1. 在 KernelSU 设置中启用“检查模块更新”，保持 AD Killer 模块已启用。
2. 打开模块列表并刷新。远端 `versionCode` 严格大于本地时，AD Killer 卡片显示更新入口。
3. 点击更新，查看发布日志；确认后交给 KernelSU 下载与安装。不想更新可取消。
4. 安装完成后手动重启。待重启时不会反复提供同一更新。

部分 KernelSU 版本会缓存一次检查结果，列表下拉未必重新发起网络请求。若新正式版已发布但未显示，正常关闭、重新打开管理器再进入模块页；不需要清空管理器数据、改手机版本号或重装模块。当前核对的实现按模块签名缓存，网络超时也可能缓存空结果。[对应版本的更新入口与缓存](https://github.com/tiann/KernelSU/blob/46636100/manager/app/src/main/java/me/weishu/kernelsu/ui/viewmodel/ModuleViewModel.kt)

WebUI 的“包含预览版”“自动检查更新”及缓存不控制 KernelSU。管理器原生入口默认只检查正式版，使用自身的网络与设置。

## 接入与发布约定

安装包保留稳定地址，无需在手机上部署另一个守护或定时任务：

```properties
updateJson=https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases/latest/download/update.json
```

该 JSON 从真实 ZIP 生成。管理器消费 `version`、整数 `versionCode`、`zipUrl` 和 `changelog` 四字段；项目扩展字段供 WebUI 使用，不宣称 KernelSU 会校验自定义 `sha256`。不依赖私人 Token、GitHub API 配额或模块目录上架。[KernelSU 模块规范](https://kernelsu.org/guide/module.html#module-prop)、[本机版本的 JSON 解析逻辑](https://github.com/tiann/KernelSU/blob/46636100/manager/app/src/main/java/me/weishu/kernelsu/data/repository/ModuleRepositoryImpl.kt)

- `versionCode` 必须是正的 signed int32，并随版本严格递增。v1.2.3 正式版为 `12301`，对应预览版为 `12300`。
- 正式包、校验和、`update.json`、`changelog.md` 均校验完成后才设为 Latest。预览版不能被当作正式版发布，不覆盖已有正式版。
- 下载地址与日志地址均指向固定版本附件；预览包中的声明仍指向最新正式版地址，便于同号预览升级正式版。
- `build.sh` 和两渠道 CI 都执行 `tests/validate_manager_update.py`，校验标准字段、包内身份／序号／渠道、实际哈希与日志的一致性。主机测试还覆盖无效类型、溢出、字段缺失、附件串版和包篡改。
- 不通过修改手机上的 `versionCode`、关闭 TLS 校验或修改 KernelSU 数据来强行制造提示。

## 真机验收记录

2026-10-22，通过 ADB 核对的设备为 Android arm64 手机，管理器为 KernelSU `v3.3.1-21-g46636100`，已安装 AD Killer v1.2.2／`12201`／stable。

- `ksud module list` 识别到正确 `updateJson`，模块已启用、未待卸载、没有更新等待重启标记。
- 手机访问原生更新地址返回 HTTP 200，四个标准字段有效；发布前远端也是 `12201`，因此当时没有更高版本可提示。
- v1.2.3 经[完整 CI](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/39709528561)通过后[发布为正式 Latest](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases/tag/v1.2.3)。手机从相同原生更新地址读取到 `12301`／stable，HTTP 200，约 1.37 秒；这已高于已装的 `12201`。
- 已在 KernelSU 原生模块列表看到 AD Killer 的“更新”按钮及模块页更新提示；无需进入 AD Killer WebUI。
- 点击该按钮，原生“更新日志”弹窗正常显示“v1.2.3 · 拦截统计与更新缓存”及对应发布说明，提供“取消”和“更新”按钮。点击“取消”后返回模块列表，更新入口仍可用。
- 取消后仍安装 v1.2.2／`12201`，没有待安装模块目录或更新标记。本次只验收提示、日志与取消流程，未执行手机下载安装或重启。
- 验收前后模块声明、状态配置、来源及两种应用名单的 SHA-256 均未改变；关闭重开管理器后守护仍运行。本次未自动安装、重启，也未修改用户规则或拦截开关。

原生更新协议门禁提交 `3e10e4a` 的[完整 CI](https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/actions/runs/39710771181)已通过，涵盖实际正式／预览包元数据校验和主机回归。

APatch、Magisk 使用相同标准字段，本轮只有 KernelSU 真机在场；不能据此宣称另外两种管理器也已真机验收。

## 无提示时

依次核对版本序号、管理器检查开关、模块启用／待重启状态、管理器自身网络和缓存。协议与兼容条件详见[原始调研](根管理器原生更新提示适配调研.md)。旧安装的声明缺失或不正确时需在根管理器手动安装一个包含正确声明的完整包；不要手工伪造已安装版本。
