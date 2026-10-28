<?php

namespace Tphp\Tebview;

use Tphp\Thread;

class Window extends Base
{
    /**
     * webview
     *
     * @var mixed
     */
    private mixed $pv = null;

    public function __construct(bool $debug = true)
    {
        $this->pv = self::ffi()->webview_create($debug, null);
    }

    public function setTitle(string $title): self
    {
        self::ffi()->webview_set_title($this->pv, $title);
        return $this;
    }

    public function setSzie(int $width, int $height, int $hints = 0): self
    {
        self::ffi()->webview_set_size($this->pv, $width, $height, $hints);
        return $this;
    }

    /**
     * setSzie
     */
    public function setSize(int $width, int $height, int $hints = 0): self
    {
        self::ffi()->webview_set_size($this->pv, $width, $height, $hints);
        return $this;
    }

    public function setHtml(string $html): self
    {
        self::ffi()->webview_set_html($this->pv, $html);
        return $this;
    }

    public function run(): self
    {
        self::ffi()->webview_run($this->pv);
        return $this;
    }

    /**
     * bind — JS 侧 `name(...args).then(res => ...)` 可调用。
     * 默认线程异步
     */
    public function bind(string $name, callable $cb): self
    {
        $pvAddr = $this->pv;
        $c_fn = function (string $id, string $req, mixed $arg) use ($cb): void {
            $w = gettype($arg) === "object" ? $arg->getAddress() : (int)$arg;
            $t = new Thread();
            $t->spawn(static function () use ($id, $req, $cb, $w): void {
                $wv = self::ffi()->cast("void *", $w);
                $params = json_decode($req, true) ?? [];
                $result = json_encode($cb($params));
                self::ffi()->webview_return($wv, $id, 0, $result);
            });
            $t->detach();
        };
        self::ffi()->webview_bind($this->pv, $name, $c_fn, $pvAddr);
        return $this;
    }

    public function destroy(): void
    {
        if ($this->pv !== null) {
            self::ffi()->webview_destroy($this->pv);
            $this->pv = null;
        }
    }

    public function __destruct()
    {
        $this->destroy();
    }
}
