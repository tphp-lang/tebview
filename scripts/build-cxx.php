<?php
// scripts/build-cxx.php —— postInstall / postUpdate（落盘后、编译链入前）
//
// 两段式构建：非 C 或需平台工具链的 TU 先编成 .o → build/，再由 mod.json
// ldflags 的 "$PKG/build/xxx.o" 展开绝对路径参与本次 ext 链接
// （PLAN/ext-packages.md §2.2 $PKG / §7 post=编译前）。
//
// 三平台分工：
//   Windows → 仅编 webview_impl.cc（EDGE/WebView2）；平台 C 源 source/win32/
//             走 ext 流水线（clang 直编，无需 pkg-config）
//   Linux   → 编 webview_impl.cc（GTK）+ window_gtk.c / osdialog.c /
//             osdialog_gtk.c / icon.c（pkg-config cflags）
//   macOS   → 编 webview_impl.cc（COCOA）+ window_mac.m / osdialog.c /
//             osdialog_mac.m / icon.c（clang ObjC，framework 链接在 mod.json）
//
// cwd = 包目录（tphp ext --add 的 scripts 约定）；env：
//   TPHP_PKG_DIR      包目录
//   TPHP_TEBVIEW_CXX  显式指定 C++ 编译器（可选）
//   TPHP_TEBVIEW_CC   显式指定 C 编译器（Linux/macOS 平台源用，可选）
//
// 环境要求见 README.md「环境要求」。

$dir = getenv("TPHP_PKG_DIR");
if (!is_string($dir) || $dir === "") {
    $dir = getcwd();
}
$dir = rtrim(str_replace("\\", "/", $dir), "/");
$build = $dir . "/build";

// tphp 提供 PHP_OS_FAMILY 常量（"Windows"/"Linux"/"Darwin"，非 php_uname()）
$family = PHP_OS_FAMILY;
$is_win = ($family === "Windows");
$is_mac = ($family === "Darwin");

// ---- 编译器探测 ----
function teb_pick_cc(string $env_name, array $cands): string {
    $v = getenv($env_name);
    if (is_string($v) && trim($v) !== "") return trim($v);
    foreach ($cands as $c) {
        $out = shell_exec($c . " --version");
        if (is_string($out) && $out !== "" && stripos($out, "version") !== false) return $c;
    }
    return "";
}

$cxx = teb_pick_cc("TPHP_TEBVIEW_CXX", ["clang++", "g++", "c++"]);
$cc  = teb_pick_cc("TPHP_TEBVIEW_CC", ["clang", "gcc", "cc"]);
if ($cxx === "" || (!$is_win && $cc === "")) {
    echo "[tebview] build-cxx: 未找到 C++/C 编译器（可用 TPHP_TEBVIEW_CXX / TPHP_TEBVIEW_CC 指定）\n";
    echo "[tebview] build-cxx: 详见 README.md「环境要求」。\n";
    exit(1);
}
echo "[tebview] build-cxx: cxx=$cxx" . ($is_win ? "" : " cc=$cc") . "\n";

@mkdir($build, 0777, true);

// ---- 编译单个 TU：成功返回 true，失败输出诊断 ----
function teb_compile(string $cmd, string $obj, string $label): bool {
    if (file_exists($obj)) unlink($obj);
    shell_exec($cmd);                       // stderr 透传控制台
    if (!file_exists($obj)) {
        echo "[tebview] build-cxx: FAILED $label（上方为编译器输出）\n";
        return false;
    }
    echo "[tebview] build-cxx: ok -> " . basename($obj) . "  ($label)\n";
    return true;
}

$inc_webview = "-I\"" . $dir . "/webview\"";
// WEBVIEW_STATIC：webview.h 的 WEBVIEW_API 在 C++ 模式默认 inline，
// 不定义则单 TU 无人 ODR-use → .text 空（链接期 webview_* 全 undefined）
$defines = "-DWEBVIEW_STATIC";
$extra_cflags = "";
$platform_srcs = [];                        // [ [cmd, obj, label], ... ]
$platform = "EDGE";

if ($is_win) {
    $defines .= " -DWEBVIEW_EDGE";
} elseif ($is_mac) {
    $defines .= " -DWEBVIEW_COCOA";
    $platform = "COCOA";
    $inc_native = "-I\"" . $dir . "/source/darwin\"";
    // framework 头文件在系统 SDK 默认搜索路径；-fobjc-arc 关闭（PebView 源码
    // 用手动 retain/release + associated objects，ARC 会与之冲突）
    $objc = "$cc -c -O1 -DNDEBUG -fno-objc-arc $inc_native";
    $platform_srcs = [
        ["$objc -o \"$build/window_mac.o\" \"$dir/source/darwin/window_mac.m\"",
         "$build/window_mac.o", "window_mac.m (ObjC)"],
        ["$objc -o \"$build/osdialog_mac.o\" \"$dir/source/darwin/osdialog_mac.m\"",
         "$build/osdialog_mac.o", "osdialog_mac.m (ObjC)"],
        ["$cc -c -O1 -DNDEBUG $inc_native -o \"$build/osdialog.o\" \"$dir/source/darwin/osdialog.c\"",
         "$build/osdialog.o", "osdialog.c"],
        ["$cc -c -O1 -DNDEBUG $inc_native -o \"$build/icon.o\" \"$dir/source/darwin/icon.c\"",
         "$build/icon.o", "icon.c"],
    ];
} else {
    $defines .= " -DWEBVIEW_GTK";
    $platform = "GTK";
    // pkg-config：优先 webkit2gtk-4.1（PebView linux.sh 同款），回退 4.0
    $gtk_cflags = null;
    foreach (["gtk+-3.0 webkit2gtk-4.1", "gtk+-3.0 webkit2gtk-4.0"] as $mods) {
        $out = shell_exec("pkg-config --cflags " . $mods . " 2>/dev/null");
        if (is_string($out) && trim($out) !== "") {
            $gtk_cflags = trim($out);
            echo "[tebview] build-cxx: pkg-config ($mods) ok\n";
            break;
        }
    }
    if ($gtk_cflags === null) {
        echo "[tebview] build-cxx: 未找到 GTK/WebKitGTK 开发包（gtk+-3.0 + webkit2gtk-4.1 或 4.0）\n";
        echo "[tebview] build-cxx: Debian/Ubuntu: sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev pkg-config\n";
        echo "[tebview] build-cxx: 详见 README.md「环境要求」。\n";
        exit(1);
    }
    $extra_cflags = " " . $gtk_cflags;
    $inc_native = "-I\"" . $dir . "/source/linux\"";
    $cflags_native = "$cc -c -O1 -DNDEBUG $inc_native $gtk_cflags";
    $platform_srcs = [
        ["$cflags_native -o \"$build/window_gtk.o\" \"$dir/source/linux/window_gtk.c\"",
         "$build/window_gtk.o", "window_gtk.c"],
        ["$cflags_native -o \"$build/osdialog_gtk.o\" \"$dir/source/linux/osdialog_gtk.c\"",
         "$build/osdialog_gtk.o", "osdialog_gtk.c"],
        ["$cflags_native -o \"$build/osdialog.o\" \"$dir/source/linux/osdialog.c\"",
         "$build/osdialog.o", "osdialog.c"],
        ["$cflags_native -o \"$build/icon.o\" \"$dir/source/linux/icon.c\"",
         "$build/icon.o", "icon.c"],
    ];
}
echo "[tebview] build-cxx: backend = $platform\n";

// ---- 编 webview C++ TU ----
$webview_obj = "$build/webview.o";
$cmd_cxx = "$cxx -c -std=c++17 -O1 -DNDEBUG $defines $inc_webview $extra_cflags"
         . " -o \"$webview_obj\" \"$dir/webview/webview_impl.cc\"";
if (!teb_compile($cmd_cxx, $webview_obj, "webview_impl.cc ($platform)")) exit(1);

// ---- 编平台 C/ObjC 源（Win 无：走 ext sources） ----
foreach ($platform_srcs as $job) {
    if (!teb_compile($job[0], $job[1], $job[2])) exit(1);
}
echo "[tebview] build-cxx: done\n";
