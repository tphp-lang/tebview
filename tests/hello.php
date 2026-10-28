<?php

require __DIR__ . "/../vendor/autoload.php";

use Tphp\Tebview\Window;

$html = file_get_contents(__DIR__ . DIRECTORY_SEPARATOR . "hello.html");


$win = new Window();
$win->setTitle("PebView") // 设置窗口标题
    ->setSzie(800, 600)
    ->setHtml( // 设置窗口的 HTML 内容
        $html
    )
    ->bind("php_time", function (array $params): string {
        sleep(5); // 假设堵塞
        var_dump($params);
        return "hello";
    })
    // 运行窗口
    ->run();
