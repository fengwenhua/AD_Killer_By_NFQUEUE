# KernelSU 模块仓库收录与不可变发布

本项目已准备收录元数据和 CI 检查，**不代表已上架或已通过审核**。已安装模块通过 `updateJson` 检查更新，与被模块仓库收录是两件事。

官方入口：[开发者门户](https://developers.kernelsu.org/)；依据：[快速开始](https://developers.kernelsu.org/docs/zh-CN/getting-started)、[module.yaml](https://developers.kernelsu.org/docs/zh-CN/module-yaml)、[发布规则](https://developers.kernelsu.org/docs/zh-CN/releasing)。

## 仓库中已准备的内容

- 根目录 `module.yaml`：ID 保持 `AD_Killer_By_NFQUEUE`，详情使用现有 README，作者、MIT 许可、问题反馈地址及 Android API 31 / arm64-v8a 提示均已配置。此文件采用 YAML 1.2 允许的 JSON 子集，便于本地主机测试仅用 Python 标准库严格解析；不是把版本或下载地址写进清单。
- 普通 CI 仅保留正式／预览两套 artifact，不再自动发布 Release。
- CI 对两套 ZIP 执行原有包与管理器协议校验，并运行固定 SHA 的 KernelSU 官方 `module-release` Action（`dry-run: true`，不公开任何版本）。该校验不等于 Portal 审核；第三方工具固定在 `b6d340ddcd562ad482568b6e66b8d7a9c34d645e`，升级需复核。
- 手动 `Publish immutable release` 工作流：读取指定全绿 main CI 的原始产物，先创建 draft，全部附件上传且校验成功后再发布。保留现有 ZIP 命名和手机更新协议，无需修改手机配置。

## 仓库所有者需确认的操作

1. GitHub 仓库 → **Settings → General → Releases → Enable release immutability**。该设置会锁定以后发布的安装包和标签，不追溯锁定已有 v1.2.5；不要删除或重建旧版本。
2. 确认开启后，在 **Settings → Secrets and variables → Actions → Variables** 添加仓库变量 `KERNELSU_IMMUTABLE_RELEASES`，值为 `enabled`。这只是发布前确认门槛，不能代替 GitHub 设置；发布后仍核验实际 `immutable` 标志。无需新增私人 Token 或把 Token 写进模块。
3. 登录开发者门户，安装／配置 **KernelSU Modules GitHub App**，仅选择 `fengwenhua/AD_Killer_By_NFQUEUE`。它对代码只读，只能写入检查结果；安装 App 不等于提交收录。
4. 发布一个新的不可变正式版后，在 Portal 执行 **我的仓库 → 预检 → 提交收录**。ID 的可用性由 Portal 检查；当前官方还要求 GitHub 账号注册满一年。等待维护者审核，不保证审核通过时间。

App 授权、门户登录和提交审核需要仓库所有者确认。配置提交本身不会自动执行这些操作。不要提前将 `updateJson` 改成尚未收录的模块仓库 API，否则现有手机可能无法获取更新。

## App 是什么，如何授权

这里的 App 是 GitHub 集成 **KernelSU Modules**，不是要在手机安装的 APK，也不需要自己创建 App。官方说明见 [App 权限](https://developers.kernelsu.org/docs/zh-CN/faq#kernelsu-modules-app-能对我的仓库做什么)：`Metadata: read`、`Contents: read`，以及用于发布验证结果的 `Checks: write`。它不能修改代码、Release、Issue 或仓库设置；授权时仍应核对 GitHub 实际展示的权限。

1. 用拥有本仓库管理员权限的 `fengwenhua` 账号打开 [开发者门户](https://developers.kernelsu.org/)，选择使用 GitHub 登录，并在 GitHub 页面确认登录授权。
2. 回到门户，点击 **安装 / 配置 App**；也可直接打开官方 [GitHub App 安装入口](https://github.com/apps/kernelsu-modules/installations/new)。登录门户与把 App 安装到仓库是两个步骤，可能分别要求确认。
3. 安装账号选择 `fengwenhua`，仓库范围选择 **Only select repositories**，只勾选 `AD_Killer_By_NFQUEUE`，核对权限后点击 **Install**。如果已安装，进入 **Configure** 将本仓库加入即可，无需授权所有仓库。
4. 返回门户刷新 **我的仓库**，确认出现 `fengwenhua/AD_Killer_By_NFQUEUE`。若没有，检查安装范围、登录账号以及仓库的 `admin` 权限；仅有 write / maintain 不够。GitHub 账号设置的 **Applications → Installed GitHub Apps** 可再次配置或撤销安装。

这些步骤需要你在自己的浏览器完成登录、二次验证和首次授权。不要把密码、Token、Cookie 或验证码发给协作者。仅安装 App 不会自动提交审核。

## 提交审核操作手册

截至 2026-10-22，已通过 GitHub API 开启本仓库不可变发布，并读回确认 `enabled: true`；Actions 仓库变量 `KERNELSU_IMMUTABLE_RELEASES=enabled` 也已配置。旧 v1.2.5 不会被追溯锁定，标签及附件保持原样，因此仍需先发布一个新的不可变正式版，不能直接用旧版本完成收录。

1. **准备正式版**：按下节流程发布未使用的新版本（v1.2.6 起采用此流程），等待发布工作流成功并确认 `immutable: true`。源码升版或 CI 构建完成不等于已发布，实际状态以 GitHub Release 为准。
2. **执行预检**：在门户 **我的仓库** 找到本项目，点击 **预检**。检查清单、文档、模块 ID、最新 Release 等项目；修复全部错误后重新检查。没有错误才可提交，警告不会阻止提交。门户预检不等同于下载 ZIP 后的完整验证。
3. **检查展示内容**：点击 **预览**，检查名称、简介、README 和图片是否正确。
4. **提交收录**：点击 **提交收录**，确认 ID 是 `AD_Killer_By_NFQUEUE`，再点击 **提交审核**。ID 必须可用，GitHub 账号需注册满一年；不要为了绕过冲突擅自更改已安装模块的 ID。
5. **确认已提交**：到 **我的模块** 查看状态，出现 **待审核 / pending** 才表示提交成功。安装 App、预检通过或 GitHub CI 全绿，都不等于已提交或已收录。
6. **等待审核和同步**：按门户显示的结果处理。通过后状态为 **已收录 / listed**；还需同步到一个有效的稳定版本，才会显示在网站和 Manager 中。若验证失败，查看门户或 GitHub 的 **KernelSU Modules** 检查结果，修复后点击 **立即同步**（每个模块最多每 10 分钟一次）。

官方逐步指南：[快速开始](https://developers.kernelsu.org/docs/zh-CN/getting-started)；按钮与状态说明：[开发者门户手册](https://developers.kernelsu.org/docs/zh-CN/portal)。

协作分工：可以代为准备清单、修复验证问题，并在获准后构建和发布新版本；门户登录、App 授权需要所有者操作。当前没有已授权的门户会话，不能仅凭 Git 推送权限代替你提交审核。完成授权后可提供不含敏感信息的截图或错误文本，以便继续协助；正式提交仍需明确确认。

## 发布一个新版本

1. 更新 `module/module.prop` 中的数字版本、正式 versionCode，以及 `CHANGELOG.md`。预览包的 versionCode 为正式值减一。新公开版本的 code 必须高于所有已发布的正式／预览版本及官方索引曾接受的最高值；不要通过删除旧发布来降低它。
2. 提交推送，等待对应 **CI** 全部通过。保留该次 CI 的 run ID；不得使用 PR、分支构建或只通过 Android job 的 run。
3. Actions → **Publish immutable release** → **Run workflow**，分支选择 `main`，填写 run ID、未用过的标签（例如 `v1.2.6`），选择 `stable` 或 `preview`。
4. 工作流按指定渠道下载原始 artifact，核验 CI SHA、源码版本、ZIP、校验和、更新 JSON 和发布说明。不会重新编译或把另一渠道的包冒充正式版。
5. 所有检查通过后才创建 draft，上传唯一 ZIP、`.sha256`、`update.json`、`changelog.md`，核对远端摘要，再公开。正式版设为 Latest，预览版不设为 Latest；最后必须确认 GitHub 的 `immutable: true`。

工作流若在上传中断，会留下草稿供核查，不会自动删除、覆盖或强行重跑。存在同名 Release（包括草稿）或标签均拒绝发布；检查失败应先核对现场，不能用 `--clobber`、移动 tag 或替换旧附件绕过。

现有 v1.2.5 的下载地址、附件与标签保持原样；发布新版本不会自动修改手机上已安装的模块。

## 预览与正式不能再覆盖同一标签

为保持现有客户端兼容性，公开标签仍是 `v<数字版本>`。一旦选某个渠道公开，该标签就已消耗：

- 要试用 `v1.2.6` 再正式发布同数字版本：先下载 Actions 的预览 artifact 手动测试，之后首次公开该 CI 的正式包。
- 要让 WebUI 在线检测到一个公开预览版：通过手动工作流发布 `v1.2.6` 预览包。下一次公开正式版使用新版本（例如 `v1.2.7`），重新构建，不替换预览包。
- 不能给一个预览 ZIP 只取消 pre-release 标记就称为本项目正式渠道；ZIP、二进制、界面以及更新元数据都带渠道信息。也不能把稳定渠道 ZIP 伪装成预览版来绕过保护。

正式更新仍由 GitHub Latest 的 `update.json` 提供。预览仅通过 WebUI“包含预览版”或手动安装获取，KernelSU 模块仓库当前不把预览作为稳定版展示。

## 本地验证

```sh
python3 scripts/validate_listing.py
python3 tests/test_listing_and_release.py
python3 tests/test_release_contract.py
```

主机测试全部使用合成 GitHub 响应，不创建真实 Release，不修改仓库设置或手机。官方 Action 的完整清单／ZIP 预检在 CI 中执行；真实收录检查还会在提交 Portal 后独立运行。
