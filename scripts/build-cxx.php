<?php
// scripts/build-cxx.php —— postInstall / postUpdate（落盘后、编译链入前）
//
// C++ 中间产物构建：webview/webview_impl.cc（唯一 C++ TU，含 webview.h 实现）
//   → build/webview.o，随后由 mod.json ldflags 的 "$PKG/build/webview.o"
//   参与本次 ext 链接（PLAN/ext-packages.md §2.2 $PKG / §7 post=编译前）。
//
// cwd = 包目录（tphp ext --add 的 scripts 约定）；env：
//   TPHP_PKG_DIR      包目录
//   TPHP_TEBVIEW_CXX  显式指定 C++ 编译器（可选，如 "clang++" / 完整路径）
//
// C++ 环境要求见 README.md「环境要求」。

$dir = getenv("TPHP_PKG_DIR");
if (!is_string($dir) || $dir === "") {
    $dir = getcwd();   // 兜底：post 时机 cwd = 包目录
}
$dir = str_replace("\\", "/", $dir);
$src = $dir . "/webview/webview_impl.cc";
$obj = $dir . "/build/webview.o";

if (!is_file($src)) {
    echo "[tebview] build-cxx: missing $src\n";
    exit(1);
}

// ---- C++ 编译器探测：TPHP_TEBVIEW_CXX > clang++ > g++ > c++ ----
$cxx = getenv("TPHP_TEBVIEW_CXX");
if (is_string($cxx) && $cxx !== "") {
    $cxx = trim($cxx);
} else {
    $cxx = "";
    foreach (["clang++", "g++", "c++"] as $cand) {
        // --version 有输出即认为存在（clang++/g++ 均打印 "version"）
        $out = shell_exec($cand . " --version");
        if (is_string($out) && $out !== "" && strpos($out, "version") !== false) {
            $cxx = $cand;
            echo "[tebview] build-cxx: using $cxx\n";
            break;
        }
    }
}
if ($cxx === "") {
    echo "[tebview] build-cxx: 未找到 C++ 编译器（clang++/g++/c++ 均不可用）\n";
    echo "[tebview] build-cxx: 请安装 C++ 环境，详见 README.md「环境要求」；\n";
    echo "[tebview] build-cxx: 或用 TPHP_TEBVIEW_CXX=<编译器> 显式指定后重试 tphp ext --add。\n";
    exit(1);
}

// ---- 编译（stderr 由 shell 透传控制台，错误可见）----
// WEBVIEW_STATIC：webview.h 的 WEBVIEW_API 在 C++ 模式下默认 inline，
// 不定义则单 TU 无人 ODR-use → .text 为空（链接期 webview_* 全部 undefined）
@mkdir($dir . "/build", 0777, true);
if (file_exists($obj)) unlink($obj);

$cmd = $cxx . " -c -std=c++17 -O1 -DNDEBUG -DWEBVIEW_STATIC"
     . " -I\"" . $dir . "/webview\""
     . " -o \"" . $obj . "\""
     . " \"" . $src . "\"";
shell_exec($cmd);

if (!file_exists($obj)) {
    echo "[tebview] build-cxx: webview.o 编译失败（上方为编译器输出）\n";
    echo "[tebview] build-cxx: 修复后重跑 tphp ext --add <本包路径> 即可（不回滚）。\n";
    exit(1);
}
echo "[tebview] build-cxx: ok -> build/webview.o\n";
