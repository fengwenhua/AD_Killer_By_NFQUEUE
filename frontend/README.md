# Vue 3 WebUI

维护入口在本目录。`module/webroot/` 是构建输出，不提交、不手工编辑。
模块端不需要 Node.js、npm 或 Python。Vue/GSAP 运行时、JS、CSS 和各自许可声明
一起内联到一个 `index.html`，兼容管理器离线 `file://` 加载，不使用运行时 CDN。

## 开发与打包

使用 Node.js 24 LTS（也支持 22.12+）及 npm，依赖以 `package-lock.json` 为准。
在仓库根执行：

```sh
npm --prefix frontend ci --include=dev
npm --prefix frontend run dev
npm --prefix frontend test
npm --prefix frontend run build
python3 tests/test_webui_contract.py module/webroot/index.html
```

`NDK=/path/to/ndk ./build.sh` 会依次执行锁定依赖安装、Vue 构建、离线产物检查、
NDK 编译和 ZIP 打包。任何前端步骤失败都会终止，不回退到旧页面。
Vite 开发服务器只监听本机；普通浏览器没有原生桥接时显示“未连接”，不会模拟
成功或执行手机操作。自动测试中的管理器是显式 mock，不属于发布代码。

## 代码分工

| 路径 | 职责 |
|---|---|
| `src/App.vue` | 应用组装、四个常驻页面、导航；切页不销毁草稿和任务观察者 |
| `src/views/` | 概览、应用名单、来源及自定义规则页面模板 |
| `src/components/` | 按钮、开关、图标、来源卡片、日志面板 |
| `src/composables/` | 引擎控制、后台同步、来源草稿/测试、应用、规则、主题与生命周期 |
| `src/services/` | KernelSU/APatch 三参数命名回调、UTF-8/安全 shell 参数及固定 CLI/路径 |
| `src/domain/` | 来源校验、统计报告、历史记录、集合快照和日志重叠等纯函数 |
| `src/assets/styles.css` | 统一语义配色、触控/焦点、响应式布局和减少动画支持 |
| `tests/` | 导入真实模块的 Node 单元测试，不 eval 旧 HTML/导出测试专用页面全局 |

原生桥接按注入 Java 接口 `(command, JSON options, global callback name)` 调用，
不能直接把它当作 npm Promise API；并发回调唯一，返回/抛错/超时均清理。
开机自启只写策略，不隐式 start/stop；来源测试只观察自己的后台 job，不覆盖正式
同步记录。同步报告必须匹配 job 和已发布 generation，不能把旧结果算成本次。

来源对象采用稳定 `id` 与 Vue `:key`。展示顺序只在初始化和保存成功后提交；切换、
编辑、测试、保存中或保存失败均不按草稿开关重排。来源和自定义规则默认独立收起。
关闭页面只停止前端轮询，不停止手机后台任务；重新打开按保存的 job ID 恢复。
查询短暂失败时继续观察，不据此删除任务记录或重复创建任务。

`useSourcePreview` / `SourcePreview` 先打开原生 `dialog`，留出绘制帧后才调用桥接。
`--source-preview URL [字符数]` 默认 500、允许 100–5000 个 Unicode 码点。设置保存到
本管理器 WebUI 的 `localStorage`；非法值回退 500，存储被拒绝时明确提示仅本次有效。
成功内容按 URL+字符数缓存 5 分钟，LRU 最多 16 项；错误不缓存，刷新失败不伪装旧成功。
同一在途请求合并，最多 2 个原生请求并发；关闭弹窗取消未启动的队列，不杀共享下载。
内置配方并行显示两份依赖及各自错误，不把原文当作差集或完整来源测试通过。
正文仅做文本插值，禁止 `v-html`。固定高度弹窗内部滚动，支持关闭按钮、遮罩、Esc、
焦点闭环与返回触发按钮，不撑动来源列表。首次读取仍取决于网络，缓存不是完整规则。
后台热重载是异步的，名单/配置保存提示不能冒充实际应用确认。

## 切页性能与设置

GSAP 3.15.0 通过 npm 精确锁定并内联，手机不拉 CDN。只引入 core/CSSPlugin，
不引入 ScrollTrigger、滚动接管或无休止的背景动画。界面依然是现有 Vue 3，不迁移框架。

- `useNavigationMotion` 使用 Web Animations 原生合成 240ms 双页面左右推移，不再每帧
  用 JS 写样式，也不在每次点击扫描长页面的全部计算样式。按导航顺序确定方向，不淡入、不纵移。
  最多两页参与过渡，快速点击合并至最后目标，不重复回退图层；结束/卸载时取消动画对象。
- 四个等高滚动视口常驻，外层文档不滚动，头部/底栏不因页面长短重新定位；非当前页 `inert`。
  子组件不因切页属性反复渲染；动效不重新请求名单、来源或启动后台任务。
- 每页自身 `scrollTop` 原生保留，应用列表内部滚动也保留。不接管滚轮、触摸或调用
  `window.scrollTo`；长文档不再成为整个动画图层的尺寸。
- 导航指示块由 CSS 等分布局和仅 transform 的过渡驱动，不动画宽高、不用 ResizeObserver
  中断补位。移除底栏背景模糊，避免移动内容经过底栏时持续重绘模糊背景。
- `useVirtualApps` 仅渲染可见行及缓冲行，Tab/Shift+Tab 能遍历未挂载行；保留勾选、搜索，
  旋转换列夹紧范围。长名称视觉省略但仍完整可搜索/朗读。来源条目保留稳定 key 和高度，
  仅挂载邻近卡片内容；键盘操作时展开完整内容，指针操作恢复按需显示。
- 系统 `prefers-reduced-motion: reduce` 即时生效，隐藏窗口时结束当前动效；折叠/旋转
  取消过渡并按 CSS 重排，不改变表单和路由。GSAP 仅保留 250ms 主题图标反馈，无循环动画。
- 手机底部为安全区之上的悬浮圆角导航；大屏/短横屏改为悬浮侧栏。深浅主题分别维护
  背景、卡片、前景、状态色与阴影，关键对比度和 44px 触控由浏览器回归检查。
- 版本由 `module/module.prop` 构建注入；`useModuleInfo` 只读 `--version-status` 获取
  本机版本，失败明确显示“随包界面版本”，不一致时在关于模块提示，不冒充最新版本。
- 设置位于规则管理右侧，含关于模块、本地更新日志及禁用的“获取更新”按钮。
  更新占位不联网、不下载、不安装。更新日志维护于 `src/domain/module.js`，不是在线公告。
- GSAP 使用 Standard License，不是 MIT；构建保留 npm 包版权头与许可链接。

### 原生系统栏边界

页面主题按钮只改变 WebUI 配色。Android 的状态栏/导航栏背景、图标明暗由管理器窗口
控制，不应将 CSS `color-scheme` 或 HTML `theme-color` 当作原生窗口 API。
2026-10-20 只读 ADB 检查的设备为 Android 16 / KernelSU v3.3.1；实装 APK 桥接方法中
存在 `enableEdgeToEdge`，但没有设置系统栏颜色、亮暗图标或管理器主题的方法。
直接启用边到边不能保证暗色页面上的图标可读，故本版不盲目启用、不隐藏系统栏，
也不改写管理器偏好或整台手机的夜间模式。系统栏白底问题尚未在此管理器中修复；
需管理器提供相应接口，或用户自行匹配管理器主题后在设备上另行验收。
接口参考：[KernelSU WebViewInterface](https://github.com/tiann/KernelSU/blob/main/manager/app/src/main/java/me/weishu/kernelsu/ui/webui/WebViewInterface.kt)、
[WebUI 窗口](https://github.com/tiann/KernelSU/blob/main/manager/app/src/main/java/me/weishu/kernelsu/ui/webui/WebUIActivity.kt)。

## 设备适配与验收

依据可用 CSS 宽度和高度，不识别品牌/UA，不按物理像素推断设备类型。

- 直板/折叠外屏：单栏、底部导航，覆盖 320/351/393/430 CSS px。
- 大折叠展开/竖屏平板：概览按卡片至少 340px 自动分栏，720px 起应用双列。
- 阔折叠/短横屏：宽至少 600px、高不超过 600px 时使用侧栏，保留竖向空间。
- 大屏/横屏平板：960px 起侧栏；1100px 起规则双栏和概览分区，内容最大宽 1360px。
- 支持安全区、动态视口高度、长 URL 换行、44px 以上触控、明暗主题、键盘焦点与
  `prefers-reduced-motion`。折叠/旋转仅重排 CSS，不重建页面或重新加载数据。

```sh
python -m pip install playwright==1.62.0
python -m playwright install chromium
python tests/test_webui_layout.py --screenshots dist/webui-preview
```

浏览器回归直接加载生产 `file://` 产物，覆盖 52 组尺寸/主题、四个页面、来源
CRUD/测试/保存、同步成功/部分失败/失败、启停错误反馈，以及连续
直板 → 大折叠 → 阔折叠 → 平板横竖屏 → 直板的窗口变化，检查焦点、表单、名单和草稿。
这些是桌面 Chromium 视口模拟，不代表各款实体设备已验收；安装后仍需在实际管理器
WebView 检查安全区、软键盘、折叠切换和原生接口。

静态尺寸检查开启 reduced-motion，避免移动中的浮点坐标影响像素断言；另有真实
原生双页水平插值/进退方向、快速反复切页、导航几何对齐、各页/内部列表滚动恢复、
运行中切换 reduced-motion、版本回退/不一致、
离线设置和更新占位无副作用测试。不是仅关闭所有动效后宣称动画可用。

```sh
python tests/test_webui_performance.py --verify
```

性能探针模拟 800 应用、64 个展开来源及 4 倍 CPU 降速，记录 RAF 间隔、样式计算、
布局次数及底栏逐帧边界。CI 只验证有界 DOM、等高视口、底栏不位移及无背景模糊，
不设依赖机器负载的 FPS 阈值；RAF 数据也不等于 Android 实体 WebView 的呈现帧率。

浏览器测试的来源夹具与原生桥接在同一个 init script 中注入，不用 `file://`
页面的 `sessionStorage` 在刷新之间传递测试数据；回归还覆盖存储被清空/拒绝的情形。
CI 固定 Playwright 1.62.0 并使用其配套 Chromium。日志会打印两者版本；本地使用
`--browser-path` 指定旧浏览器通过，不等于已在 CI 浏览器上通过。

构建参考：[Vue 官方快速开始](https://vuejs.org/guide/quick-start.html)、
[Vite 官方指南](https://vite.dev/guide/)、
[singlefile 插件说明](https://github.com/richardtallent/vite-plugin-singlefile)。
动效参考：[GSAP 安装](https://gsap.com/docs/v3/Installation/)、
[context 生命周期清理](https://gsap.com/docs/v3/GSAP/gsap.context()/)、
[减少动态效果建议](https://gsap.com/docs/v3/GSAP/gsap.matchMedia()/)。
原生交互参考：[dialog 与焦点](https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/dialog)、
[Vue 大列表性能](https://vuejs.org/guide/best-practices/performance.html)。
