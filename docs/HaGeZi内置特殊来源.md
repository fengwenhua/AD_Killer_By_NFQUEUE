# HaGeZi 内置配方

## 选择与升级

“规则 → 开源规则来源”固定保留一个 HaGeZi 内置项，可启停、测试以及选择配方。
配方地址不可编辑，内置项不可删除；选择只修改草稿，需保存来源并同步后才改变已发布规则。
不会因选择、保存、测试或预览自动启动守护，也不改变开机自启策略。

| 配方 | 标识 | 请求与结果 |
| --- | --- | --- |
| 完整 Pro（新安装默认） | `builtin:hagezi-pro` | 只请求 Pro，保留该文件全部有效域名，不执行 Fake 差集 |
| 兼容旧版：排除 Fake | `builtin:hagezi-pro-minus-fake` | 请求 Pro 与 Fake，只贡献 `Pro集合 − Fake集合` |

Fake 是假冒商店、诈骗等网站的名单，不是“无效规则”。上游说明 Pro 包含 Fake 类别，
但改为完整 Pro 是策略变化，不能当作单纯去重。因此旧安装保留原配方和启停，
不因升级自动切换。新配置标记为 `source_defaults_version=3`；从 v2 升级不改写现有
来源文件。更早默认模板更新普通来源时也保留内置配方和启停，缺失时补为停用的旧配方。

`--sources-list` 返回当前配方、依赖和两种可选配方；保存只允许一个内置项。
旧页面保存时遗漏内置项，后端保留原配方和启停；未知标识或同时提交两种配方均拒绝。

## 地址与缓存

- Pro：<https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro.txt>
- Fake：<https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/fake.txt>
- 正式下载、单源测试和预览均支持已知 CDN 候选；实际地址可在结果中查看，详见[规则同步策略](规则同步策略.md)。

两种配方使用不同缓存身份和依赖数量。完整 Pro 的成功缓存不能当作旧差集缓存，反之亦然。
切换回原配方可复用它自己仍有效的缓存；局部复用不延长核验年龄，失败回退仍受 24 小时限制。
完整 Pro 每次只请求一份内容，接收预算为单源 64 MiB；旧配方两份内容合计共享 90 秒和
128 MiB 接收预算。两者仍共享整次同步的时间、字节、解压和行数上限。

旧配方必须在一次来源处理中完整下载或经有效条件请求确认 Pro 与 Fake。任一依赖失败
时只允许回退完整旧组，不能混用新 Pro 与旧 Fake，也不能以剩余成功来源发布半套规则。
HTML、空文件、坏 gzip、资源超限或缓存损坏均按既有规则拒绝。

## 匹配语义与 Light

两种内置配方都沿用原有转换：归一化域名/Hosts/`*.domain`，组内排序去重，输出
**精确匹配的 Hosts 域名**。Fake 不会变成白名单，也不会从其他来源中全局剔除。
完整 Pro 只是取消本组差集，不顺带扩大为后缀匹配，也不保证等同于上游 DNS 工具的
完整 wildcard 效果。例：`example.org` 的精确规则不自动拦截 `child.example.org`。

HaGeZi Light 使用 AdGuard 后缀规则。因此本轮保留 Light 的默认订阅，不依据上游
分级包含关系直接删掉它；同一根域名的精确与后缀规则在本模块中有不同覆盖范围。

2026-11-24 用同一上游提交 `a9656847eb8401b14583144db60d6a8d4ee4646c` 固定快照核验：
Pro 归一化为 239389 个域名，Fake 为 10357 个，两者字面交集 10350；旧差集保留 229039。
Fake 中 7 个域名未以同样字符串出现在 Pro，6 个在 Pro 中有父域名。Light 解析为
76665 条后缀规则，其中 12625 个根域名不在 Pro 的精确字符串集合中。这些数字是本次
快照事实，不是固定更新目标，也不能把上游“包含”误读成转换后的字符串集合完全包含。
原始公共规则与完整 SHA-256 仅留在忽略目录，不进入模块或 Git。

## 验证与范围

单元测试验证完整 Pro 和旧差集、精确/后缀边界、重复、资源限制及源配置单一性。
独立 Python 比较两种 C++ 配方的完整集合，不只比较数量。真实 HTTP/CLI 回归验证
新装默认、旧安装启停保持、显式切换、缓存隔离与过期、失败保留旧 generation，
以及停用时测试和预览不改规则、正式缓存或同步记录。浏览器回归覆盖窄屏明暗主题、
未保存草稿、保存失败与成功、重新创建 WebView 和完整 Pro 单份预览。

```sh
# 使用主机测试构建的辅助程序；无参数文件时使用离线夹具。
python3 tests/test_hagezi_parity.py /path/to/test_hagezi_source
python3 tests/test_hagezi_parity.py /path/to/test_hagezi_source /path/to/pro.txt /path/to/fake.txt
```

普通来源参考脚本不自动加入本组。来源及许可见[第三方规则与许可](第三方规则与许可.md)，
上游分级说明见[包含矩阵](https://github.com/hagezi/dns-blocklists/blob/main/CHEATSHEET.md#inclusion-matrix)。
本地和 CI 通过不替代用户升级后的设备验收；手机操作与软重启由用户手动执行。
