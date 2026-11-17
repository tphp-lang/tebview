<?php

// tebview 端到端冒烟：原生类（无 autoload / 无 FFI）
// 验证面：ctor(UI 线程建窗) → setter → bind → run(drain) → 回调 → resolve
// 页面加载即自动调用 php_time(...)（见 hello.html），var_dump 输出即 bind 链路 OK。

use Tphp\Tebview\Window;

$html = file_get_contents(__DIR__ . DIRECTORY_SEPARATOR . "hello.html");

$win = new Window(true);   // debug = 显示 devtools
$win->setTitle("PebView")  // 设置窗口标题
    ->setSize(800, 600)
    ->bind("php_time", function (array $params): string {
        sleep(5); // 假设堵塞
        var_dump($params);
        return "hello";
    })
    ->setHtml(             // 设置窗口的 HTML 内容（须在 bind 之后：绑定 glue 随 init script 注入）
        $html
    )
    // 运行窗口
    ->run();

echo "window closed\n";
