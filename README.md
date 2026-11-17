# tebview

tphp 的原生窗口扩展（[PebView](https://github.com/kingbes/pebview) C ABI + [webview](https://github.com/webview/webview) C++ 库），`type=ext` 包。

## 为什么是 ext + scripts 两段式

tphp 的扩展面是 **C**（`src/*.c` 走 ext 流水线正常编译），而 webview 库本体是 **C++**——ext 的 `sources` 只收集 `*.c/*.m`，编不了 C++。

解法（[PLAN/ext-packages.md §2.2 $PKG / §7](../../../TinyPHP.rt/PLAN/ext-packages.md)）：

1. **scripts（postInstall/postUpdate）**：`scripts/build-cxx.php` 在「落盘后、编译链入前」用 C++ 编译器把唯一的 C++ TU（`webview/webview_impl.cc`）编成中间产物 `build/webview.o`；
2. **ldflags 的 `$PKG` 前缀**：`"$PKG/build/webview.o"` 在链接时展开为包目录绝对路径，与 ext 的 C 对象一起链入 tphp。

C 扩展面（`src/tebview.c`）与 C++ 侧的边界是 `include/Pebview.h`（C ABI 契约，两者共用）。

## 环境要求

| 组件 | 说明 |
|---|---|
| **tphp artifact 三件套** | `tphp.exe` + `libtphp.a` + `include/`（同一 release，`tphp_abi` 校验） |
| **C 编译器** | clang（ext 流水线默认链；Windows 上的 tphp 即 LLVM Clang 构建） |
| **C++ 编译器** | **clang++ / g++ / c++ 任一**（Windows 推荐 LLVM-MinGW clang64 的 clang++，与 tphp 同款工具链）；找不到时用 `TPHP_TEBVIEW_CXX=<编译器>` 显式指定 |
| **WebView2 Runtime** | Windows 运行时需要（Win10/11 一般自带）；webview 内置 loader，无需 WebView2Loader.dll |
| Linux / macOS | 未纳入（PebView 的 gtk/cocoa 实现未 vendor，`os` 只声明了 windows） |

**没有 C++ 编译器时**：`tphp ext --add` 会失败（post 脚本退出非 0、不回滚），装好编译器后重跑同一条命令即可。

## 安装

```bash
tphp ext --add C:/project/tphp/tebview
# 或项目内（写回 mod.json 的 ext[]）：
cd my-project && tphp ext --add C:/project/tphp/tebview
```

安装过程：落盘 → `postInstall` 编 C++ 中间产物 → 编译 `src/` + `source/win32/` → 全量链接出新 tphp（下次启动生效）。

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
| `show` / `hide` | `(): self` | 仅 Windows |
| `run` | `(): void` | 阻塞到窗口关闭（teb 式主线程 drain） |
| `destroy` | `(): void` | 幂等；对象 GC 时 native_dtor 兜底 |

`Tphp\Tebview\VERSION` 常量 = 包版本。

## 目录

```
tebview/
├── mod.json            # ext 契约（sources/os/$PKG ldflags）+ scripts
├── include/Pebview.h   # C ABI 契约（C 扩展面 ↔ C++ 侧共用）
├── src/tebview.c       # C 扩展面：Tphp\Tebview\Window 注册 + UI 线程/drain 模型
├── webview/            # webview 库（vendored：webview.h + WebView2 头 + impl TU）
├── source/win32/       # PebView 的 C 实现（window/osdialog/icon）
├── scripts/build-cxx.php  # postInstall/postUpdate：C++ → build/webview.o
└── tests/              # hello.php（无头冒烟：页面加载即回调，var_dump 可捕获）
```

## 线程模型（实现备忘）

- **UI 线程**（uv_thread）：`webview_create → webview_run(消息泵) → webview_destroy`。所有 webview 操作经 `webview_dispatch` 投递（win32 引擎的 setter/COM 不线程安全；`terminate_impl`=PostQuitMessage 只能 UI 线程调）。
- **主 VM 线程**：`run()` 里 10ms condvar drain bind 队列 → `tphp_vm_call_bounded` 执行 PHP 回调 → `webview_return`（内部 dispatch，任意线程安全）。
- 纯 C 状态（WinState/队列）`tm_malloc`；`TVal` 只在主 VM 线程存取。

## TODO（PebView ABI 的后续接入面）

- 托盘（window_tray / tray_menu）、原生对话框（osdialog_*）、系统通知（toastShow，wintoastlib C++ TU）、窗口图标（set_icon）
- 自定义标题栏 / 点击穿透 / 状态回调（window_win.c 已 vendor，缺 PHP 面）
- Linux gtk / macOS cocoa 后端
