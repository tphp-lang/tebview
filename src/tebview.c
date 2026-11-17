/*
 * tphp/tebview - PebView C ABI 原生封装（type=ext，替代 FFI 方案）
 *
 * 对外暴露单个原生类 Tphp\Tebview\Window：
 *
 *   $win = new Window(true);            // debug = 显示 devtools
 *   $win->setTitle("PebView")
 *       ->setSize(800, 600)             // 可选第三参 SizeHint（0=NONE 1=MIN 2=MAX 3=FIXED）
 *       ->setHtml($html)
 *       ->bind("php_time", function (array $params): string {
 *           return "hello";             // 返回值 json 编码后 resolve 给 JS Promise
 *       })
 *       ->run();                        // 阻塞到窗口关闭（teb 同款主线程 drain）
 *   $win->destroy();                    // 幂等；对象释放时 native_dtor 兜底
 *
 * 线程模型（teb drain 模式；win32 引擎 setter/COM 不线程安全）：
 *   - UI 线程（uv_thread）：webview_create → webview_run(消息泵) → webview_destroy。
 *     所有 webview 操作（setter/navigate/bind/terminate）经 webview_dispatch
 *     投递到该线程执行（dispatch=PostMessage，泵未启动时排队、run 后处理）。
 *   - 主 VM 线程：run() 里 drain bind 队列（10ms condvar），PHP 回调经
 *     tphp_vm_call_bounded 执行；webview_return 内部 dispatch，任意线程可调。
 *   - bind 回调抛异常：tphp_warn + status=1（JS Promise reject），不静默。
 *
 * 窗口句柄存 props["w"]（TV_INT 地址）；destroy 后再用一律抛 Error（不静默）。
 * 纯 C 数据（WinState/队列/注册表）用 tm_malloc——mimalloc 默认堆线程安全
 * （teb 在 mg 线程同款用法）；TVal 生命周期数据只在主 VM 线程存取。
 */
#include "pkg.h"       /* tphp_ext_reg_const_* / tphp_json_decode_c / tphp_json_encode_c */
#include "class.h"     /* TClass / tphp_class_find_prop / tphp_class_register_compile */
#include "value.h"     /* TVal / TV_* / TStr / TObj / TArr / tm_* */
#include "opcode.h"    /* VM / tphp_vm_raise / tphp_warn */

#include "Pebview.h"   /* PebView C ABI 契约（webview_* / window_*） */

#include <uv.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

/* ---- vm.c ----（未随 include/ 分发，teb 同款本地 extern）
 * 异常安全的回调调用：回调抛异常落本函数 local setjmp，返回 false。 */
extern bool tphp_vm_call_bounded(struct VM *vm, TVal callable, TVal *args,
                                 int n, TVal *ret);

#define WHO_CTOR   "Tphp\\Tebview\\Window::__construct"
#define WHO_SETTLE "Tphp\\Tebview\\Window::setTitle()"
#define WHO_SIZE   "Tphp\\Tebview\\Window::setSize()"
#define WHO_NAV    "Tphp\\Tebview\\Window::navigate()"
#define WHO_HTML   "Tphp\\Tebview\\Window::setHtml()"
#define WHO_INITJS "Tphp\\Tebview\\Window::initJs()"
#define WHO_EVALJS "Tphp\\Tebview\\Window::evalJs()"
#define WHO_BIND   "Tphp\\Tebview\\Window::bind()"
#define WHO_TRANS  "Tphp\\Tebview\\Window::setTransparent()"
#define WHO_SHOW   "Tphp\\Tebview\\Window::show()"
#define WHO_HIDE   "Tphp\\Tebview\\Window::hide()"
#define WHO_RUN    "Tphp\\Tebview\\Window::run()"

/* ---- WinState（纯 C 数据；TVal 生命周期数据只在主 VM 线程存取） ---- */

typedef struct BindReg BindReg;
typedef struct BindReq BindReq;

typedef struct WinState {
    uv_thread_t tid;
    bool tid_valid;
    uv_mutex_t mu;
    uv_cond_t cv;
    bool sync_valid;
    bool debug;
    webview_t w;                /* UI 线程写（ready 前），此后主线程只读 */
    bool ready;
    bool w_ok;
    bool closing;               /* destroy 已发起：setter 拒绝、泵即将退 */
    bool quit;                  /* UI 线程已结束（w 已销毁） */
    BindReg *regs;              /* 主线程链表 */
    BindReq *q_head, *q_tail;   /* UI → 主 drain 队列 */
} WinState;

struct BindReg {
    WinState *st;
    char *name;                 /* 拷贝 */
    TVal cb;                    /* addref 持有（主线程存取） */
    struct BindReg *next;
};

struct BindReq {
    char *name;                 /* 拷贝（trampoline 线程） */
    char *id;                   /* 拷贝 */
    char *req;                  /* 拷贝（JSON 文本） */
    struct BindReq *next;
};

static void bind_trampoline(const char *id, const char *req, void *arg);

/* ---- 句柄存取 ---- */

static WinState *w_of(TObj *o) {
    int s = tphp_class_find_prop(o->cls, "w");
    if (s < 0 || o->props[s].t != T_INT || o->props[s].u.i == 0) return NULL;
    return (WinState *)(intptr_t)o->props[s].u.i;
}

static WinState *need_w(VM *vm, TObj *o, const char *who) {
    WinState *st = w_of(o);
    if (!st) RAISE(vm, "Error", "%s: window is already destroyed", who);
    return st;
}

static TVal fluent(TObj *o) {
    TVal r = TV_OBJ(o);
    tval_addref(r);
    return r;
}

static char *tv_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = tm_malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* ---- UI 线程任务（webview_dispatch 载荷）----
 * ctx 所有权由 free_ctx 决定：字符串型任务在 fn 内 free 字符串、
 * ui_job_cb free 结构体；句柄型任务（terminate/bind）不 free ctx。 */

typedef struct UiJob {
    void (*fn)(void *ctx);
    void *ctx;
    bool free_ctx;
} UiJob;

typedef struct JobStr { WinState *st; char *s; } JobStr;
typedef struct JobSize { WinState *st; int w, h, hints; } JobSize;
typedef struct JobInt  { WinState *st; int v; } JobInt;

static void ui_job_cb(webview_t w, void *arg) {
    (void)w;
    UiJob *j = (UiJob *)arg;
    j->fn(j->ctx);
    if (j->free_ctx) tm_free(j->ctx);
    tm_free(j);
}

static void job_set_title(void *p) {
    JobStr *j = (JobStr *)p;
    webview_set_title(j->st->w, j->s);
    tm_free(j->s);
}
static void job_navigate(void *p) {
    JobStr *j = (JobStr *)p;
    webview_navigate(j->st->w, j->s);
    tm_free(j->s);
}
static void job_set_html(void *p) {
    JobStr *j = (JobStr *)p;
    webview_set_html(j->st->w, j->s);
    tm_free(j->s);
}
static void job_init_js(void *p) {
    JobStr *j = (JobStr *)p;
    webview_init(j->st->w, j->s);
    tm_free(j->s);
}
static void job_eval_js(void *p) {
    JobStr *j = (JobStr *)p;
    webview_eval(j->st->w, j->s);
    tm_free(j->s);
}
static void job_set_size(void *p) {
    JobSize *j = (JobSize *)p;
    webview_set_size(j->st->w, j->w, j->h, j->hints);
}
static void job_set_transparent(void *p) {
    JobInt *j = (JobInt *)p;
    webview_set_transparent(j->st->w, j->v);
}
static void job_do_bind(void *p) {
    BindReg *reg = (BindReg *)p;
    webview_bind(reg->st->w, reg->name, bind_trampoline, reg);
}
static void job_do_terminate(void *p) {
    webview_terminate((webview_t)p);   /* PostQuitMessage 必须在 UI 线程 */
}

/* 锁内检查 + dispatch（quit/closing 后一律拒绝；返回 false 由调用方抛） */
static bool ui_dispatch(WinState *st, void (*fn)(void *), void *ctx,
                        bool free_ctx) {
    bool ok = false;
    uv_mutex_lock(&st->mu);
    if (st->w_ok && !st->closing && !st->quit) {
        UiJob *j = (UiJob *)tm_malloc(sizeof *j);
        if (j) {
            j->fn = fn;
            j->ctx = ctx;
            j->free_ctx = free_ctx;
            webview_dispatch(st->w, ui_job_cb, j);
            ok = true;
        }
    }
    uv_mutex_unlock(&st->mu);
    return ok;
}

/* ---- UI 线程主函数 ---- */

static void ui_main(void *arg) {
    WinState *st = (WinState *)arg;
    webview_t w = webview_create(st->debug ? 1 : 0, NULL);
    uv_mutex_lock(&st->mu);
    st->w = w;
    st->w_ok = (w != NULL);
    st->ready = true;
    uv_cond_signal(&st->cv);
    uv_mutex_unlock(&st->mu);
    if (w) webview_run(w);      /* 消息泵；关窗/terminate 后返回 */
    /* 泵已退出，本线程即 UI 线程：直接 delete（destroy 内部无 dispatch） */
    if (w) webview_destroy(w);
    uv_mutex_lock(&st->mu);
    st->quit = true;
    uv_cond_signal(&st->cv);
    uv_mutex_unlock(&st->mu);
}

/* ---- bind 队列（UI trampoline → 主 drain） ---- */

static void bind_trampoline(const char *id, const char *req, void *arg) {
    BindReg *reg = (BindReg *)arg;
    WinState *st = reg->st;
    BindReq *q = (BindReq *)tm_malloc(sizeof *q);
    if (!q) return;
    memset(q, 0, sizeof *q);
    q->name = tv_strdup(reg->name);
    q->id = tv_strdup(id ? id : "");
    q->req = tv_strdup(req ? req : "");
    if (!q->name || !q->id || !q->req) {
        tm_free(q->name); tm_free(q->id); tm_free(q->req); tm_free(q);
        return;
    }
    uv_mutex_lock(&st->mu);
    if (st->q_tail) st->q_tail->next = q;
    else st->q_head = q;
    st->q_tail = q;
    uv_cond_signal(&st->cv);
    uv_mutex_unlock(&st->mu);
}

static void free_req(BindReq *q) {
    if (!q) return;
    tm_free(q->name);
    tm_free(q->id);
    tm_free(q->req);
    tm_free(q);
}

/* ---- bind 回调分发（主 VM 线程；teb_dispatch 同款有界调用） ---- */

static void run_dispatch_one(VM *vm, WinState *st, BindReq *q) {
    BindReg *reg = NULL;
    for (BindReg *r = st->regs; r; r = r->next) {
        if (strcmp(r->name, q->name) == 0) { reg = r; break; }
    }
    if (!reg) { free_req(q); return; }   /* 无注册（理论不可达）：丢弃 */

    /* JS → PHP 入参：req 是 JSON 文本（webview bind 协议）；
     * 解码失败按 FFI 时代 json_decode($req, true) ?? [] 兜底空数组 */
    TVal pv = TV_NULL;
    if (!tphp_json_decode_c(q->req, strlen(q->req), &pv)) {
        pv = TV_ARR(tarr_new());
    }
    TVal cb_args[1] = { pv };
    TVal cbv = reg->cb;
    tval_addref(cbv);        /* 防回调内 destroy → teardown 释放注册表 UAF */
    TVal out = TV_NULL;
    bool ok = tphp_vm_call_bounded(vm, cbv, cb_args, 1, &out);

    TVal out_json = TV_NULL;      /* TStr 持有份 */
    const char *result = "null";
    int status = 0;
    if (ok) {
        TStr *ts = NULL;
        if (tphp_json_encode_c(out, &ts) && ts) {
            out_json = TV_STR(ts);
            result = tstr_val(ts);
        }
    } else {
        tphp_warn("PHP Warning: TebView bind callback raised "
                  "(JS promise rejected)\n");
        result = "{\"error\":\"callback raised\"}";
        status = 1;
    }
    /* webview_return 内部 dispatch（线程安全）；closing 期泵未死仍可 resolve */
    uv_mutex_lock(&st->mu);
    if (st->w_ok && !st->quit)
        webview_return(st->w, q->id, status, result);
    uv_mutex_unlock(&st->mu);

    tval_release(&out_json);
    tval_release(&out);
    tval_release(&cbv);
    tval_release(&cb_args[0]);    /* pv（arr/str 所有权归本次调用） */
    free_req(q);
}

/* ---- 生命周期 ---- */

/* 请求关闭（不 join）：置 closing → dispatch terminate 到 UI 线程 */
static void st_close_request(WinState *st) {
    uv_mutex_lock(&st->mu);
    if (st->w_ok && !st->closing && !st->quit) {
        st->closing = true;      /* 此后 setter/run_dispatch 拒绝新投递 */
        UiJob *j = (UiJob *)tm_malloc(sizeof *j);
        if (j) {
            j->fn = job_do_terminate;
            j->ctx = st;
            j->free_ctx = false;
            webview_dispatch(st->w, ui_job_cb, j);
        }
    }
    uv_mutex_unlock(&st->mu);
}

/* join UI 线程 + 释放注册表/队列/同步原语 + st 本体 */
static void st_teardown(WinState *st) {
    if (st->tid_valid) {
        uv_thread_join(&st->tid);
        st->tid_valid = false;
    }
    uv_mutex_lock(&st->mu);
    BindReg *r = st->regs;
    while (r) {
        BindReg *n = r->next;
        tm_free(r->name);
        tval_release(&r->cb);
        tm_free(r);
        r = n;
    }
    st->regs = NULL;
    BindReq *q = st->q_head;
    while (q) {
        BindReq *n = q->next;
        free_req(q);
        q = n;
    }
    st->q_head = st->q_tail = NULL;
    uv_mutex_unlock(&st->mu);
    if (st->sync_valid) {
        uv_mutex_destroy(&st->mu);
        uv_cond_destroy(&st->cv);
        st->sync_valid = false;
    }
    tm_free(st);
}

static void w_clear(TObj *o) {
    int s = tphp_class_find_prop(o->cls, "w");
    if (s >= 0) o->props[s] = TV_INT(0);
}

/* ---- native 方法 ---- */

static TVal nm_window_construct(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    bool debug = true;                     /* FFI 时代默认 true（开发窗） */
    if (n >= 1) {
        if (args[0].t == T_TRUE) debug = true;
        else if (args[0].t == T_FALSE) debug = false;
        else if (args[0].t == T_INT) debug = (args[0].u.i != 0);
        else
            RAISE(vm, "TypeError", "%s: argument debug must be bool", WHO_CTOR);
    }

    WinState *st = (WinState *)tm_malloc(sizeof *st);
    if (!st) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_CTOR);
    memset(st, 0, sizeof *st);
    st->debug = debug;
    if (uv_mutex_init(&st->mu) != 0) {
        tm_free(st);
        RAISE(vm, "RuntimeException", "%s: mutex init failed", WHO_CTOR);
    }
    st->sync_valid = true;
    if (uv_cond_init(&st->cv) != 0) {
        uv_mutex_destroy(&st->mu);
        st->sync_valid = false;
        tm_free(st);
        RAISE(vm, "RuntimeException", "%s: cond init failed", WHO_CTOR);
    }
    if (uv_thread_create(&st->tid, ui_main, st) != 0) {
        uv_mutex_destroy(&st->mu);
        uv_cond_destroy(&st->cv);
        st->sync_valid = false;
        tm_free(st);
        RAISE(vm, "RuntimeException", "%s: cannot spawn UI thread", WHO_CTOR);
    }
    st->tid_valid = true;

    /* 等 UI 线程建窗（w_ok=false 时 webview_create 失败，典型 = 缺 WebView2
     * Runtime；ui_main 在 w=NULL 时跳过 run/destroy 直接 quit） */
    uv_mutex_lock(&st->mu);
    while (!st->ready) uv_cond_wait(&st->cv, &st->mu);
    bool ok = st->w_ok;
    uv_mutex_unlock(&st->mu);
    if (!ok) {
        st_teardown(st);
        RAISE(vm, "RuntimeException",
              "%s: webview_create failed (Windows: is WebView2 Runtime "
              "installed? see README.md)", WHO_CTOR);
    }
    int s = tphp_class_find_prop(o->cls, "w");
    o->props[s] = TV_INT((int64_t)(intptr_t)st);
    return TV_NULL;
}

static TVal nm_set_title(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_SETTLE);
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "%s expects string $title", WHO_SETTLE);
    JobStr *j = (JobStr *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_SETTLE);
    j->st = st;
    j->s = tv_strdup(tstr_val((TStr *)args[0].u.p));
    if (!j->s || !ui_dispatch(st, job_set_title, j, true)) {
        tm_free(j->s);
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_SETTLE);
    }
    return fluent(o);
}

static TVal nm_set_size(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_SIZE);
    if (n < 2 || args[0].t != T_INT || args[1].t != T_INT)
        RAISE(vm, "TypeError", "%s expects (int $width, int $height, int $hints = 0)",
              WHO_SIZE);
    int64_t hints = (n >= 3 && args[2].t != T_NULL) ? args[2].u.i : 0;
    if (hints < 0 || hints > 3)
        RAISE(vm, "ValueError", "%s: hints must be 0..3 (SIZE_HINT_*)", WHO_SIZE);
    JobSize *j = (JobSize *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_SIZE);
    j->st = st;
    j->w = (int)args[0].u.i;
    j->h = (int)args[1].u.i;
    j->hints = (int)hints;
    if (!ui_dispatch(st, job_set_size, j, true)) {
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_SIZE);
    }
    return fluent(o);
}

static TVal nm_navigate(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_NAV);
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "%s expects string $url", WHO_NAV);
    JobStr *j = (JobStr *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_NAV);
    j->st = st;
    j->s = tv_strdup(tstr_val((TStr *)args[0].u.p));
    if (!j->s || !ui_dispatch(st, job_navigate, j, true)) {
        tm_free(j->s);
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_NAV);
    }
    return fluent(o);
}

static TVal nm_set_html(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_HTML);
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "%s expects string $html", WHO_HTML);
    JobStr *j = (JobStr *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_HTML);
    j->st = st;
    j->s = tv_strdup(tstr_val((TStr *)args[0].u.p));
    if (!j->s || !ui_dispatch(st, job_set_html, j, true)) {
        tm_free(j->s);
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_HTML);
    }
    return fluent(o);
}

static TVal nm_init_js(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_INITJS);
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "%s expects string $js", WHO_INITJS);
    JobStr *j = (JobStr *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_INITJS);
    j->st = st;
    j->s = tv_strdup(tstr_val((TStr *)args[0].u.p));
    if (!j->s || !ui_dispatch(st, job_init_js, j, true)) {
        tm_free(j->s);
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_INITJS);
    }
    return fluent(o);
}

static TVal nm_eval_js(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_EVALJS);
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "%s expects string $js", WHO_EVALJS);
    JobStr *j = (JobStr *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_EVALJS);
    j->st = st;
    j->s = tv_strdup(tstr_val((TStr *)args[0].u.p));
    if (!j->s || !ui_dispatch(st, job_eval_js, j, true)) {
        tm_free(j->s);
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_EVALJS);
    }
    return fluent(o);
}

static TVal nm_set_transparent(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_TRANS);
    bool enable = true;
    if (n >= 1) {
        if (args[0].t == T_TRUE) enable = true;
        else if (args[0].t == T_FALSE) enable = false;
        else if (args[0].t == T_INT) enable = (args[0].u.i != 0);
        else
            RAISE(vm, "TypeError", "%s: argument enable must be bool", WHO_TRANS);
    }
    JobInt *j = (JobInt *)tm_malloc(sizeof *j);
    if (!j) RAISE(vm, "RuntimeException", "%s: out of memory", WHO_TRANS);
    j->st = st;
    j->v = enable ? 1 : 0;
    if (!ui_dispatch(st, job_set_transparent, j, true)) {
        tm_free(j);
        RAISE(vm, "Error", "%s: window is closed", WHO_TRANS);
    }
    return fluent(o);
}

/* bind(string $name, callable $cb): self —— JS 侧 name(...args).then(...)。
 * 注册表只在主线程动（无 unbind 面前不回收）；webview_bind 本体经 dispatch
 * 到 UI 线程（内部改 bindings map + eval，非线程安全）。 */
static TVal nm_bind(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_BIND);
    if (n < 2)
        RAISE(vm, "ArgumentCountError", "%s expects (string $name, callable $cb)",
              WHO_BIND);
    if (args[0].t != T_STR)
        RAISE(vm, "TypeError", "%s: argument $name must be string", WHO_BIND);
    switch (args[1].t) {
        case T_CLOSURE: case T_OBJ: case T_ARR: case T_STR: break;
        default:
            RAISE(vm, "TypeError", "%s: argument $cb must be callable", WHO_BIND);
    }
    const char *name = tstr_val((TStr *)args[0].u.p);

    uv_mutex_lock(&st->mu);
    for (BindReg *r = st->regs; r; r = r->next) {
        if (strcmp(r->name, name) == 0) {
            uv_mutex_unlock(&st->mu);
            RAISE(vm, "Error", "%s: name already bound: %s", WHO_BIND, name);
        }
    }
    BindReg *reg = (BindReg *)tm_malloc(sizeof *reg);
    if (!reg) {
        uv_mutex_unlock(&st->mu);
        RAISE(vm, "RuntimeException", "%s: out of memory", WHO_BIND);
    }
    reg->st = st;
    reg->name = tv_strdup(name);
    reg->cb = args[1];
    tval_addref(reg->cb);
    reg->next = st->regs;
    st->regs = reg;
    bool dispatched = ui_dispatch(st, job_do_bind, reg, false);
    uv_mutex_unlock(&st->mu);
    if (!reg->name || !dispatched) {
        /* 注册表回滚（仅主线程动过，锁内已出） */
        st->regs = reg->next;
        tval_release(&reg->cb);
        tm_free(reg->name);
        tm_free(reg);
        RAISE(vm, "Error", "%s: window is closed", WHO_BIND);
    }
    return fluent(o);
}

/* run(): void —— teb 同款主线程 drain：等 bind 队列 + 关窗事件；
 * 回调内阻塞（如 sleep）期间窗口仍响应（泵在 UI 线程）。 */
static TVal nm_run(VM *vm, void *self, TVal *args, int n) {
    (void)args;
    (void)n;
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_RUN);
    for (;;) {
        bool done = false;
        BindReq *q = NULL;
        uv_mutex_lock(&st->mu);
        while (!st->q_head && !st->quit && !st->closing)
            uv_cond_timedwait(&st->cv, &st->mu, 10 * 1000 * 1000);   /* 10ms */
        q = st->q_head;
        st->q_head = st->q_tail = NULL;
        done = st->quit || st->closing;
        uv_mutex_unlock(&st->mu);
        while (q) {                       /* 锁外分发：回调内可再调 setter */
            BindReq *nx = q->next;
            run_dispatch_one(vm, st, q);
            q = nx;
        }
        if (done) break;
    }
    return TV_NULL;
}

static TVal nm_destroy(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    (void)args;
    (void)n;
    TObj *o = (TObj *)self;
    WinState *st = w_of(o);
    if (st) {
        st_close_request(st);
        st_teardown(st);
        w_clear(o);
    }
    return TV_NULL;
}

/* native_dtor：对象释放兜底；无 VM，不抛异常 */
static void window_native_dtor(TObj *o) {
    WinState *st = w_of(o);
    if (st) {
        st_close_request(st);
        st_teardown(st);
        w_clear(o);
    }
}

#ifdef _WIN32
/* show/hide：PebView window_*（HWND 直调，跨线程安全）；
 * get_window 读 m_window（create 时定死）——closing 后不再取。 */
static TVal nm_show(VM *vm, void *self, TVal *args, int n) {
    (void)args;
    (void)n;
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_SHOW);
    void *hwnd = NULL;
    uv_mutex_lock(&st->mu);
    if (st->w_ok && !st->closing && !st->quit)
        hwnd = webview_get_window(st->w);
    uv_mutex_unlock(&st->mu);
    if (!hwnd)
        RAISE(vm, "Error", "%s: window is closed", WHO_SHOW);
    window_show(hwnd);
    return fluent(o);
}

static TVal nm_hide(VM *vm, void *self, TVal *args, int n) {
    (void)args;
    (void)n;
    TObj *o = (TObj *)self;
    WinState *st = need_w(vm, o, WHO_HIDE);
    void *hwnd = NULL;
    uv_mutex_lock(&st->mu);
    if (st->w_ok && !st->closing && !st->quit)
        hwnd = webview_get_window(st->w);
    uv_mutex_unlock(&st->mu);
    if (!hwnd)
        RAISE(vm, "Error", "%s: window is closed", WHO_HIDE);
    window_hide(hwnd);
    return fluent(o);
}
#endif

/* ---- 类构建（tui_util 同款 helpers，包内自包含） ---- */

static TClass *tv_cls_new(const char *fqcn) {
    TClass *cls = (TClass *)tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern(fqcn, strlen(fqcn));
    cls->kind = CK_CLASS;
    cls->flattened = true;
    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =
        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =
        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;
    return cls;
}

static void tv_cls_add_native(TClass *cls, const char *name, void *fn,
                              bool is_static);

static void tv_cls_add_ctor(TClass *cls, void *fn) {
    cls->m_ctor = (int16_t)cls->n_methods;
    tv_cls_add_native(cls, "__construct", fn, false);
}

static void tv_cls_add_prop(TClass *cls, const char *name) {
    TProp *p = (TProp *)tm_realloc(cls->props,
                                   sizeof(TProp) * (size_t)(cls->n_props + 1));
    TProp *np = &p[cls->n_props];
    memset(np, 0, sizeof *np);
    np->name = tstr_intern(name, strlen(name));
    np->defval = TV_NULL;
    np->visibility = 2;           /* CVIS_PRIVATE */
    np->slot = (uint16_t)cls->n_props;
    cls->props = p;
    cls->n_props++;
}

static void tv_cls_add_native(TClass *cls, const char *name, void *fn,
                              bool is_static) {
    TMethod *ms = (TMethod *)tm_realloc(
        cls->methods, sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    TMethod *m = &ms[cls->n_methods];
    memset(m, 0, sizeof *m);
    m->name = tstr_intern(name, strlen(name));
    m->visibility = 0;            /* CVIS_PUBLIC */
    m->native = fn;
    m->is_static = is_static;
    cls->methods = ms;
    cls->n_methods++;
}

static void tv_register_window_class(void) {
    TClass *cls = tv_cls_new("Tphp\\Tebview\\Window");
    tv_cls_add_prop(cls, "w");
    tv_cls_add_ctor(cls, (void *)nm_window_construct);
    tv_cls_add_native(cls, "setTitle", (void *)nm_set_title, false);
    tv_cls_add_native(cls, "setSize", (void *)nm_set_size, false);
    tv_cls_add_native(cls, "navigate", (void *)nm_navigate, false);
    tv_cls_add_native(cls, "setHtml", (void *)nm_set_html, false);
    tv_cls_add_native(cls, "initJs", (void *)nm_init_js, false);
    tv_cls_add_native(cls, "evalJs", (void *)nm_eval_js, false);
    tv_cls_add_native(cls, "bind", (void *)nm_bind, false);
    tv_cls_add_native(cls, "setTransparent", (void *)nm_set_transparent, false);
    tv_cls_add_native(cls, "run", (void *)nm_run, false);
    tv_cls_add_native(cls, "destroy", (void *)nm_destroy, false);
#ifdef _WIN32
    tv_cls_add_native(cls, "show", (void *)nm_show, false);
    tv_cls_add_native(cls, "hide", (void *)nm_hide, false);
#endif
    cls->native_dtor = window_native_dtor;
    tphp_class_register_compile(cls);
}

/* ---- init：ext registry 在首脚本编译前调用一次 ----
 * 命名约定（PLAN/ext-packages.md §2.3）：tphp/tebview → tphp_ext_tphp_tebview_init */
void tphp_ext_tphp_tebview_init(void) {
    tphp_ext_reg_const_str("Tphp\\Tebview\\VERSION", "0.0.1");
    tphp_ext_reg_const_int("Tphp\\Tebview\\SIZE_HINT_NONE", 0);
    tphp_ext_reg_const_int("Tphp\\Tebview\\SIZE_HINT_MIN", 1);
    tphp_ext_reg_const_int("Tphp\\Tebview\\SIZE_HINT_MAX", 2);
    tphp_ext_reg_const_int("Tphp\\Tebview\\SIZE_HINT_FIXED", 3);
    tv_register_window_class();
}
