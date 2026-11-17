# tebview

tphp 的原生窗口扩展（[PebView](https://github.com/kingbes/pebview) C ABI + [webview](https://github.com/webview/webview) C++ 库），`type=ext` 包。支持 **Windows（WebView2）/ Linux（WebKitGTK）/ macOS（Cocoa WKWebView）** 三平台。

## 为什么是 ext + scripts 两段式

tphp 的扩展面是 **C**（`src/*.c` 走 ext 流水线正常编译），而 webview 库本体是 **C++**；Linux 的 GTK 源需 pkg-config 动态 cflags、macOS 的 `.m` 源需 ObjC 工具链——这些都不是静态 `mod.json` 能表达的。

解法（[PLAN/ext-packages.md §2.2 $PKG / §7](../../../TinyPHP.rt/PLAN/ext-packages.md)）：

1. **scripts（postInstall/postUpdate）**：`scripts/build-cxx.php` 在「落盘后、编译链入前」按当前平台把 webview C++ TU 和平台原生源编成 `build/*.o`：
   - Windows：仅 `webview_impl.cc` → `webview.o`（平台 C 源 `source/win32/` 走 ext 流水线）
   - Linux：`webview_impl.cc`（GTK）+ `window_gtk.c` / `osdialog*.c` / `icon.c`（pkg-config 探测 cflags）
   - macOS：`webview_impl.cc`（COCOA）+ `window_mac.m` / `osdialog_mac.m` / `osdialog.c` / `icon.c`（clang ObjC）
2. **ldflags 的 `$PKG` 前缀**：`"$PKG/build/xxx.o"` 在链接时展开为包目录绝对路径，与 ext 的 C 对象一起链入 tphp。

C 扩展面（`src/tebview.c`）与平台侧的边界是 `include/Pebview.h`（C ABI 契约，三平台共用）。webview 后端由 webview.h 按 `__APPLE__` / `__unix__` / `_WIN32` 自动选择，scripts 里显式 `-DWEBVIEW_*` 只为诊断清晰。

## 环境要求

| 组件 | Windows | Linux | macOS |
|---|---|---|---|
| **tphp artifact 三件套** | `tphp.exe` + `libtphp.a` + `include/`（同一 release，`tphp_abi` 校验） | `tphp` + `libtphp.a` + `include/` | 同左 |
| **C 编译器** | clang（LLVM-MinGW） | clang / gcc | clang（Xcode CLT） |
| **C++ 编译器** | clang++ / g++ / c++ 任一 | clang++ / g++ | clang++（Xcode CLT） |
| **平台库** | WebView2 Runtime（Win10/11 一般自带；webview 内置 loader，无需 WebView2Loader.dll） | `libgtk-3-dev` + `libwebkit2gtk-4.1-dev`（或 4.0）+ `pkg-config` | Xcode CLT（WebKit / Cocoa / Carbon / Foundation / AppKit framework） |

**编译器覆盖**：`TPHP_TEBVIEW_CXX=<C++编译器>`、`TPHP_TEBVIEW_CC=<C编译器>`（Linux/macOS 平台源用）。

**依赖缺失时**：`tphp ext --add` 会失败（post 脚本退出非 0、不回滚），脚本会打印具体缺什么（如「未找到 GTK/WebKitGTK 开发包」+ apt 安装命令），装好后重跑同一条命令即可。

**验证状态**：Windows 已端到端实测（`tests/hello.php` bind 回调通过）；Linux/macOS 代码与平台配置已按 PebView 官方 `linux.sh` / `macos.sh` 移植，**未在对应平台实测编译运行**，需你在目标平台验证。

## 安装

```bash
tphp ext --add C:/project/tphp/tebview
# 或项目内（写回 mod.json 的 ext[]）：
cd my-project && tphp ext --add C:/project/tphp/tebview
```

安装过程：落盘 → `postInstall` 按平台编中间产物（`build/*.o`）→ 编译 `src/`（+ Windows 的 `source/win32/`）→ 全量链接出新 tphp（下次启动生效）。

Linux 依赖安装示例（Debian/Ubuntu）：

```bash
sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev pkg-config clang clang++
# 旧版发行版（如 Ubuntu 20.04 / Debian 11）：
sudo apt install libgtk-3-dev libwebkit2gtk-4.0-dev pkg-config clang clang++
```

macOS：`xcode-select --install`（含 clang / clang++ 与所需 framework）。

## 用法

```php
<?php
use Tphp\Tebview\Window;

$win = new Window(true);   // debug = 显示 devtools
$win->setTitle("PebView")
    ->setSize(800, 600)    // 可选第三参：Window::SIZE_HINT_*（见下）
    ->bind("php_time", function (array $params): string {
        return json_encode(["reply" => $params]);
    })
    ->setHtml($html)       // 必须在 bind 之后（绑定 glue 随 init script 注入新文档）
    ->run();               // 阻塞到窗口关闭

// JS 侧：php_time(...args).then(res => ...)  —— res 即 PHP 返回值 json 编码结果
```

| 方法 | 签名 | 说明 |
|---|---|---|
| `__construct` | `(bool $debug = true)` | UI 线程建窗；失败抛 RuntimeException（典型 = 缺 WebView2 Runtime） |
| `setTitle` | `(string): self` | 窗口标题 |
| `setSize` | `(int, int, int $hints = 0): self` | 尺寸；hints = `SIZE_HINT_NONE/MIN/MAX/FIXED`（0..3） |
| `navigate` | `(string $url): self` | 加载 URL |
| `setHtml` | `(string): self` | 加载 HTML 字符串；须在 `bind` 之后调用（绑定 glue 随 init script 注入新文档，先加载页面会导致页面脚本里绑定函数不存在） |
| `initJs` | `(string): self` | 注入每次文档创建时执行的 JS |
| `evalJs` | `(string): self` | 执行 JS |
| `bind` | `(string $name, callable $cb): self` | JS 侧 `$name(...args).then(...)`；回调收 `array $params`，返回值 json 编码回传；同名重复 bind 抛 Error；回调抛异常 → JS Promise reject |
| `setTransparent` | `(bool $enable = true): self` | 渲染层背景透明（配页面 CSS 背景透明） |
| `show` / `hide` | `(): self` | 三平台均可用（PebView window_show/hide） |
| `run` | `(): void` | 阻塞到窗口关闭（teb 式主线程 drain） |
| `destroy` | `(): void` | 幂等；对象 GC 时 native_dtor 兜底 |

`Tphp\Tebview\VERSION` 常量 = 包版本。

## 目录

```
tebview/
├── mod.json            # ext 契约（三平台 os 段 + $PKG ldflags）+ scripts
├── include/Pebview.h   # C ABI 契约（C 扩展面 ↔ 平台侧共用）
├── src/tebview.c       # C 扩展面：Tphp\Tebview\Window 注册 + UI 线程/drain 模型
├── webview/            # webview 库（vendored：webview.h + WebView2 头 + impl TU）
├── source/win32/       # PebView Windows C 实现（走 ext sources，clang 直编）
├── source/linux/       # PebView GTK 实现（scripts 编，pkg-config cflags）
├── source/darwin/      # PebView Cocoa 实现（scripts 编，ObjC）
├── scripts/build-cxx.php  # postInstall/postUpdate：按平台编 build/*.o
└── tests/              # hello.php（无头冒烟：页面加载即回调，var_dump 可捕获）
```

## 线程模型（实现备忘）

- **UI 线程**（uv_thread）：`webview_create → webview_run(消息泵) → webview_destroy`。所有 webview 操作经 `webview_dispatch` 投递（各平台 setter 均须 UI 线程：win32 COM、GTK/Cocoa 主线程；terminate 也只能 UI 线程调）。
- **主 VM 线程**：`run()` 里 10ms condvar drain bind 队列 → `tphp_vm_call_bounded` 执行 PHP 回调 → `webview_return`（内部 dispatch，任意线程安全）。
- 纯 C 状态（WinState/队列）`tm_malloc`；`TVal` 只在主 VM 线程存取。

## TODO（PebView ABI 的后续接入面）

- 托盘（window_tray / tray_menu）、原生对话框（osdialog_*）、系统通知（toastShow，wintoastlib C++ TU）、窗口图标（set_icon）
- 自定义标题栏 / 点击穿透 / 状态回调（PebView window_* 三平台已 vendor，缺 PHP 面）
- ~~Linux gtk / macOS cocoa 后端~~（已纳入三平台支持；待目标平台实测）
- Linux/macOS 上 `set_icon` 状态：Linux 可用（icon.c 的 `__linux__` 分支）；macOS PebView 原实现未覆盖（返回 `OS_UNSUPPORTED`）
