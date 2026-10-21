# HaGeZi Pro − Fake 内置来源

## 使用

“规则 → 开源规则来源”中固定保留一项 **HaGeZi Pro − Fake**。
默认停用，新装/升级均不会自动加强拦截；普通来源、自定义规则和开机自启策略不变。
支持测试以及启用/停用，不可编辑或删除。启停需保存来源并同步后才改变已发布规则，
不是停止模块守护，也不是取消一个正在进行的下载任务。

- 唯一标识：`builtin:hagezi-pro-minus-fake`，不是下载 URL。
- Pro：<https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro.txt>
- Fake：<https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/fake.txt>
- 不恢复 HaGeZi Light；普通 13 源配方和 Python 默认模式保持原样。
- `--sources-list` 返回锁定标识、名称和两份固定依赖；`--sources-save` 仅能修改
  该项的启停状态。遗漏该项时后端恢复原状态，不会因旧页面保存而把它删掉。
  未知 `builtin:` 标识拒绝保存；生产版本不提供运行期地址覆盖接口。

## 运算边界

1. 按原 Python 配方，归一化域名/Hosts/`*.domain`，转成精确域名集合。
2. Pro 和 Fake 分别去重，只保留 `Pro集合 − Fake集合`，输出排序后的 Hosts 规则。
3. 再与其他来源合并、全局按精确/后缀、黑/白类型去重，并原子发布规则 generation。

例如 Pro 包含 `a.example`、`b.example`，Fake 包含 `b.example`，本组只贡献
`a.example`；若另一个来源仍包含 `b.example`，模块仍会按那个来源拦截它。
Fake 不会转成白名单，也不会对子域名做额外推断。这不是全局去重的别名。
输出为精确匹配，与旧脚本一致；不等同于原始 wildcard 文件的完整后缀覆盖效果。

两份上游均须下载、校验成功。缺失、空文件、HTML、坏 gzip、资源超限会使整组
失败，绝不直接使用未经排除的 Pro。其余普通来源成功时沿用现有部分发布策略，
明确标记本组失败；全部来源不可用时保留旧规则。空差集可通过测试，但若没有
其他可用规则，正式同步不会用空规则包覆盖旧 generation。

测试复用相同下载、校验、差集和资源限制，返回后台 job；无论启用与否均可测试。
不改正式同步时间/结果、当前规则、来源开关、引擎或自启配置。
后台来源任务也会隔离自身的继承冻结组，避免离开管理器后下载被一起冻结。

正式同步记录按一个组合来源显示成功/失败和时间；成功后列出 Pro 唯一数、
Fake 唯一数、差集剔除数、最终保留数。相关数量已先完成组内去重/差集，
不能把 Fake 全量行数或剔除数量当成普通全局重复行数。

## 验证

单元测试覆盖域名归一化、集合差、重复、缺失依赖、资源边界和临时文件清理；
集成测试覆盖默认停用、启停保存、固定标识保护、整组失败、原子发布和失败保留旧规则。
独立 Python 比对完整集合而非只比较数量，浏览器回归覆盖测试、保存、错误反馈与长 URL。

模块不捆绑下载后的上游规则。来源及许可参见[第三方规则与许可](第三方规则与许可.md)。
电脑的 13 源完整快照参考脚本不自动加入本组；单独比对命令为：

```sh
python3 tests/test_hagezi_parity.py /path/to/test_hagezi_source /path/to/pro.txt /path/to/fake.txt
```
