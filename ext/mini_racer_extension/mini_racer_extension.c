#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ruby.h"
#include "ruby/encoding.h"
#include "ruby/version.h"
#include "ruby/thread.h"
#include "serde.c"
#include "mini_racer_v8.h"

#if RUBY_API_VERSION_CODE < 3*10000+4*100 // 3.4.0
static inline void rb_thread_lock_native_thread(void)
{
    // Without rb_thread_lock_native_thread, V8 in single-threaded mode is
    // prone to crash with debug checks like this...
    //
    // # Fatal error in ../deps/v8/src/base/platform/platform-posix.cc, line 1350
    // # Debug check failed: MainThreadIsCurrentThread().
    //
    // ...because the Ruby runtime clobbers thread-local variables when it
    // context-switches threads. You have been warned.
}
#endif

#define countof(x)  (sizeof(x) / sizeof(*(x)))
#define endof(x)    ((x) + countof(x))

// mostly RO: assigned once by platform_set_flag1 while holding |flags_mtx|,
// from then on read-only and accessible without holding locks
int single_threaded;

// work around missing pthread_barrier_t on macOS
typedef struct Barrier
{
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    int count, in, out;
} Barrier;

static inline int barrier_init(Barrier *b, int count)
{
    int r;

    if ((r = pthread_mutex_init(&b->mtx, NULL)))
        return r;
    if ((r = pthread_cond_init(&b->cv, NULL))) {
        pthread_mutex_destroy(&b->mtx);
        return r;
    }
    b->count = count;
    b->out = 0;
    b->in = 0;
    return 0;
}

static inline void barrier_destroy(Barrier *b)
{
    pthread_mutex_destroy(&b->mtx);
    pthread_cond_destroy(&b->cv);
}

static inline int barrier_wait(Barrier *b)
{
    int last;

    pthread_mutex_lock(&b->mtx);
    while (b->out)
        pthread_cond_wait(&b->cv, &b->mtx);
    if (++b->in == b->count) {
        b->in = 0;
        b->out = b->count;
        pthread_cond_broadcast(&b->cv);
    } else {
        do
            pthread_cond_wait(&b->cv, &b->mtx);
        while (b->in);
    }
    last = (--b->out == 0);
    if (last)
        pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mtx);
    return last;
}

typedef struct Context
{
    int depth;     // call depth, protected by |rr_mtx|
    // protected by |mtx|; RW for ruby threads, RO for v8 thread;
    // atomic because context_stop (which can be called from other ruby
    // threads) writes it without holding |mtx|, to avoid deadlocking
    // 1=shut down v8, 2=free memory; note that only the v8 thread
    // frees the memory and it intentionally stays around until
    // the ruby object is gc'd, otherwise lifecycle management
    // gets too complicated
    atomic_int quit;
    int verbose_exceptions;
    int64_t idle_gc, max_memory, timeout;
    struct State *pst; // used by v8 thread
    VALUE procs;       // array of js -> ruby callbacks
    VALUE exception;   // pending exception or Qnil
    Buf req, res;      // ruby->v8 request/response, mediated by |mtx| and |cv|
    Buf snapshot;
    // |rr_mtx| stands for "recursive ruby mutex"; it's used to exclude
    // other ruby threads but allow reentrancy from the same ruby thread
    // (think ruby->js->ruby->js calls)
    pthread_mutex_t rr_mtx;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    struct {
        pthread_mutex_t mtx;
        pthread_cond_t cv;
        int cancel;
    } wd; // watchdog
    Barrier early_init, late_init;
} Context;

typedef struct Snapshot {
    VALUE blob;
} Snapshot;

static void context_destroy(Context *c);
static void context_free(void *arg);
static void context_mark(void *arg);
static size_t context_size(const void *arg);

static const rb_data_type_t context_type = {
    .wrap_struct_name   =  "mini_racer/context",
    .function           = {
        .dfree = context_free,
        .dmark = context_mark,
        .dsize = context_size,
    },
};

static void snapshot_free(void *arg);
static void snapshot_mark(void *arg);
static size_t snapshot_size(const void *arg);

static const rb_data_type_t snapshot_type = {
    .wrap_struct_name   =  "mini_racer/snapshot",
    .function           = {
        .dfree = snapshot_free,
        .dmark = snapshot_mark,
        .dsize = snapshot_size,
    },
};

static VALUE platform_init_error;
static VALUE context_disposed_error;
static VALUE parse_error;
static VALUE memory_error;
static VALUE runtime_error;
static VALUE internal_error;
static VALUE snapshot_error;
static VALUE terminated_error;
static VALUE context_class;
static VALUE snapshot_class;
static VALUE date_time_class;
static VALUE js_function_class;

static pthread_mutex_t flags_mtx = PTHREAD_MUTEX_INITIALIZER;
static Buf flags; // protected by |flags_mtx|

// arg == &(struct rendezvous_nogvl){...}
static void *rendezvous_callback(void *arg);

// note: must be stack-allocated or VALUEs won't be visible to ruby's GC
typedef struct State
{
    VALUE   a, b;
    uint8_t verbatim_keys:1;
} State;

// note: must be stack-allocated or VALUEs won't be visible to ruby's GC
typedef struct DesCtx
{
    State   *tos;
    VALUE   refs; // object refs array
    uint8_t transcode_latin1:1;
    char    err[64];
    State   stack[512];
} DesCtx;

struct rendezvous_nogvl
{
    Context *context;
    Buf *req, *res;
};

struct rendezvous_des
{
    DesCtx *d;
    Buf *res;
};

static void DesCtx_init(DesCtx *c)
{
    c->tos  = c->stack;
    c->refs = rb_ary_new();
    *c->tos = (State){Qundef, Qundef, /*verbatim_keys*/0};
    *c->err = '\0';
    c->transcode_latin1 = 1; // convert to utf8
}

static void put(DesCtx *c, VALUE v)
{
    VALUE *a, *b;

    if (*c->err)
        return;
    a = &c->tos->a;
    b = &c->tos->b;
    switch (TYPE(*a)) {
    case T_ARRAY:
        rb_ary_push(*a, v);
        break;
    case T_HASH:
        if (*b == Qundef) {
            *b = v;
        } else {
            if (!c->tos->verbatim_keys)
                *b = rb_funcall(*b, rb_intern("to_s"), 0);
            rb_hash_aset(*a, *b, v);
            *b = Qundef;
        }
        break;
    case T_UNDEF:
        *a = v;
        break;
    default:
        snprintf(c->err, sizeof(c->err), "bad state");
        return;
    }
}

static void push(DesCtx *c, VALUE v)
{
    if (*c->err)
        return;
    if (c->tos == endof(c->stack)) {
        snprintf(c->err, sizeof(c->err), "stack overflow");
        return;
    }
    *++c->tos = (State){v, Qundef, /*verbatim_keys*/0};
    rb_ary_push(c->refs, v);
}

// see also des_named_props_end
static void pop(DesCtx *c)
{
    if (*c->err)
        return;
    if (c->tos == c->stack) {
        snprintf(c->err, sizeof(c->err), "stack underflow");
        return;
    }
    put(c, (*c->tos--).a);
}

static void des_null(void *arg)
{
    put(arg, Qnil);
}

static void des_undefined(void *arg)
{
    put(arg, Qnil);
}

static void des_bool(void *arg, int v)
{
    put(arg, v ? Qtrue : Qfalse);
}

static void des_int(void *arg, int64_t v)
{
    put(arg, LONG2FIX(v));
}

static void des_num(void *arg, double v)
{
    put(arg, DBL2NUM(v));
}

static void des_date(void *arg, double v)
{
    double sec, usec;

    if (!isfinite(v))
        rb_raise(rb_eRangeError, "invalid Date");
    sec = v/1e3;
    usec = 1e3 * fmod(v, 1e3);
    put(arg, rb_time_new(sec, usec));
}

// note: v8 stores bigints in 1's complement, ruby in 2's complement,
// so we have to take additional steps to ensure correct conversion
static void des_bigint(void *arg, const void *p, size_t n, int sign)
{
    VALUE v;
    size_t i;
    DesCtx *c;
    unsigned long *a, t, limbs[65]; // +1 to suppress sign extension

    c = arg;
    if (*c->err)
        return;
    if (n > sizeof(limbs) - sizeof(*limbs)) {
        snprintf(c->err, sizeof(c->err), "bigint too big");
        return;
    }
    a = limbs;
    t = 0;
    for (i = 0; i < n; a++, i += sizeof(*a)) {
        memcpy(a, (char *)p + i, sizeof(*a));
        t = *a;
    }
    if (t >> 63)
        *a++ = 0; // suppress sign extension
    v = rb_big_unpack(limbs, a-limbs);
    if (sign < 0)
        v = rb_big_mul(v, LONG2FIX(-1));
    put(c, v);
}

static void des_string(void *arg, const char *s, size_t n)
{
    put(arg, rb_utf8_str_new(s, n));
}

static VALUE str_encode_bang(VALUE v)
{
    // TODO cache these? this function can get called often
    return rb_funcall(v, rb_intern("encode!"), 1, rb_str_new_cstr("UTF-8"));
}

static void des_string8(void *arg, const uint8_t *s, size_t n)
{
    rb_encoding *e;
    DesCtx *c;
    VALUE v;

    c = arg;
    if (*c->err)
        return;
    if (c->transcode_latin1) {
        e = rb_enc_find("ISO-8859-1"); // TODO cache?
        if (!e) {
            snprintf(c->err, sizeof(c->err), "no ISO-8859-1 encoding");
            return;
        }
        v = rb_enc_str_new((char *)s, n, e);
        v = str_encode_bang(v); // cannot fail
    } else {
        v = rb_enc_str_new((char *)s, n, rb_ascii8bit_encoding());
    }
    put(c, v);
}

// des_string16: |s| is not word aligned
// des_string16: |n| is in bytes, not code points
static void des_string16(void *arg, const void *s, size_t n)
{
    rb_encoding *e;
    VALUE v, r;
    DesCtx *c;
    int exc;

    c = arg;
    if (*c->err)
        return;
    // TODO(bnoordhuis) replace this hack with something more principled
    if (n == sizeof(js_function_marker) && !memcmp(js_function_marker, s, n))
        return put(c, rb_funcall(js_function_class, rb_intern("new"), 0));
    e = rb_enc_find("UTF-16LE"); // TODO cache?
    if (!e) {
        snprintf(c->err, sizeof(c->err), "no UTF16-LE encoding");
        return;
    }
    v = rb_enc_str_new((char *)s, n, e);
    // JS strings can contain unmatched or illegal surrogate pairs
    // that Ruby won't decode; return the string as-is in that case
    r = rb_protect(str_encode_bang, v, &exc);
    if (exc) {
        rb_set_errinfo(Qnil);
        r = v;
    }
    put(c, r);
}

// ruby doesn't really have a concept of a byte array so store it as
// an 8-bit string instead; it's either that or a regular array of
// numbers, but the latter is markedly less efficient, storage-wise
static void des_arraybuffer(void *arg, const void *s, size_t n)
{
    put(arg, rb_enc_str_new((char *)s, n, rb_ascii8bit_encoding()));
}

static void des_array_begin(void *arg)
{
    push(arg, rb_ary_new());
}

static void des_array_end(void *arg)
{
    pop(arg);
}

static void des_named_props_begin(void *arg)
{
    push(arg, rb_hash_new());
}

// see also pop
static void des_named_props_end(void *arg)
{
    DesCtx *c;

    c = arg;
    if (*c->err)
        return;
    if (c->tos == c->stack) {
        snprintf(c->err, sizeof(c->err), "stack underflow");
        return;
    }
    c->tos--; // dropped, no way to represent in ruby
}

static void des_object_begin(void *arg)
{
    push(arg, rb_hash_new());
}

static void des_object_end(void *arg)
{
    pop(arg);
}

static void des_map_begin(void *arg)
{
    DesCtx *c;

    c = arg;
    push(c, rb_hash_new());
    c->tos->verbatim_keys = 1; // don't stringify or intern keys
}

static void des_map_end(void *arg)
{
    pop(arg);
}

static void des_object_ref(void *arg, uint32_t id)
{
    DesCtx *c;
    VALUE v;

    c = arg;
    v = rb_ary_entry(c->refs, id);
    put(c, v);
}

static void des_error_begin(void *arg)
{
    push(arg, rb_class_new_instance(0, NULL, rb_eRuntimeError));
}

static void des_error_end(void *arg)
{
    pop(arg);
}

static int collect(VALUE k, VALUE v, VALUE a)
{
    rb_ary_push(a, k);
    rb_ary_push(a, v);
    return ST_CONTINUE;
}

static void add_string(Ser *s, VALUE v)
{
    rb_encoding *e;
    const void *p;
    size_t n;

    Check_Type(v, T_STRING);
    e = rb_enc_get(v);
    p = RSTRING_PTR(v);
    n = RSTRING_LEN(v);
    if (e) {
        if (!strcmp(e->name, "ISO-8859-1"))
            return ser_string8(s, p, n);
        if (!strcmp(e->name, "UTF-16LE"))
            return ser_string16(s, p, n);
    }
    return ser_string(s, p, n);
}

static int serialize1(Ser *s, VALUE refs, VALUE v)
{
    unsigned long limbs[64];
    VALUE a, t, id;
    size_t i, n;
    int sign;

    if (*s->err)
        return -1;
    switch (TYPE(v)) {
    case T_ARRAY:
        id = rb_hash_lookup(refs, v);
        if (NIL_P(id)) {
            n = RARRAY_LENINT(v);
            i = rb_hash_size_num(refs);
            rb_hash_aset(refs, v, LONG2FIX(i));
            ser_array_begin(s, n);
            for (i = 0; i < n; i++)
                if (serialize1(s, refs, rb_ary_entry(v, i)))
                    return -1;
            ser_array_end(s, n);
        } else {
            ser_object_ref(s, FIX2LONG(id));
        }
        break;
    case T_HASH:
        id = rb_hash_lookup(refs, v);
        if (NIL_P(id)) {
            a = rb_ary_new();
            i = rb_hash_size_num(refs);
            n = rb_hash_size_num(v);
            rb_hash_aset(refs, v, LONG2FIX(i));
            rb_hash_foreach(v, collect, a);
            for (i = 0; i < 2*n; i += 2) {
                t = rb_ary_entry(a, i);
                switch (TYPE(t)) {
                case T_FIXNUM:
                case T_STRING:
                case T_SYMBOL:
                    continue;
                }
                break;
            }
            if (i == 2*n) {
                ser_object_begin(s);
                for (i = 0; i < 2*n; i += 2) {
                    if (serialize1(s, refs, rb_ary_entry(a, i+0)))
                        return -1;
                    if (serialize1(s, refs, rb_ary_entry(a, i+1)))
                        return -1;
                }
                ser_object_end(s, n);
            } else {
                return bail(&s->err, "TODO serialize as Map");
            }
        } else {
            ser_object_ref(s, FIX2LONG(id));
        }
        break;
    case T_DATA:
        if (date_time_class == CLASS_OF(v)) {
            v = rb_funcall(v, rb_intern("to_time"), 0);
        }
        if (rb_cTime == CLASS_OF(v)) {
            struct timeval tv = rb_time_timeval(v);
            ser_date(s, tv.tv_sec*1e3 + tv.tv_usec/1e3);
        } else {
            static const char undefined_conversion[] = "Undefined Conversion";
            ser_string(s, undefined_conversion, sizeof(undefined_conversion)-1);
        }
        break;
    case T_NIL:
        ser_null(s);
        break;
    case T_UNDEF:
        ser_undefined(s);
        break;
    case T_TRUE:
        ser_bool(s, 1);
        break;
    case T_FALSE:
        ser_bool(s, 0);
        break;
    case T_BIGNUM:
        // note: v8 stores bigints in 1's complement, ruby in 2's complement,
        // so we have to take additional steps to ensure correct conversion
        memset(limbs, 0, sizeof(limbs));
        sign = rb_big_sign(v) ? 1 : -1;
        if (sign < 0)
            v = rb_big_mul(v, LONG2FIX(-1));
        rb_big_pack(v, limbs, countof(limbs));
        ser_bigint(s, limbs, countof(limbs), sign);
        break;
    case T_FIXNUM:
        ser_int(s, FIX2LONG(v));
        break;
    case T_FLOAT:
        ser_num(s, NUM2DBL(v));
        break;
    case T_SYMBOL:
        v = rb_sym2str(v);
        // fallthru
    case T_STRING:
        add_string(s, v);
        break;
    case T_OBJECT:
        // this is somewhat wide, but we have active support which creates
        // entirely new objects
        if (rb_respond_to(v, rb_intern("to_time"))) {
            v = rb_funcall(v, rb_intern("to_time"), 0);
        }
        if (rb_obj_is_kind_of(v, rb_cTime)) {
            struct timeval tv = rb_time_timeval(v);
            ser_date(s, tv.tv_sec*1e3 + tv.tv_usec/1e3);
        } else {
	    snprintf(s->err, sizeof(s->err), "unsupported type %s", rb_class2name(CLASS_OF(v)));
	    return -1;
        }
        break;
    default:
        snprintf(s->err, sizeof(s->err), "unsupported type %x", TYPE(v));
        return -1;
    }
    return 0;
}

// don't mix with ser_array_begin/ser_object_begin because
// that will throw off the object reference count
static int serialize(Ser *s, VALUE v)
{
    return serialize1(s, rb_hash_new(), v);
}

static struct timespec deadline_ms(int ms)
{
    static const int64_t ns_per_sec = 1000*1000*1000;
    struct timespec t;

#ifdef __APPLE__
    clock_gettime(CLOCK_REALTIME, &t);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    t.tv_sec += ms/1000;
    t.tv_nsec += ms%1000 * ns_per_sec/1000;
    while (t.tv_nsec >= ns_per_sec) {
        t.tv_nsec -= ns_per_sec;
        t.tv_sec++;
    }
    return t;
}

static int timespec_le(struct timespec a, struct timespec b)
{
    if (a.tv_sec < b.tv_sec) return 1;
    return a.tv_sec == b.tv_sec && a.tv_nsec <= b.tv_nsec;
}

static int deadline_exceeded(struct timespec deadline)
{
    return timespec_le(deadline, deadline_ms(0));
}

static void *v8_watchdog(void *arg)
{
    struct timespec deadline;
    Context *c;

    c = arg;
    deadline = deadline_ms(c->timeout);
    pthread_mutex_lock(&c->wd.mtx);
    for (;;) {
        if (c->wd.cancel)
            break;
        pthread_cond_timedwait(&c->wd.cv, &c->wd.mtx, &deadline);
        if (c->wd.cancel)
            break;
        if (deadline_exceeded(deadline)) {
            v8_terminate_execution(c->pst);
            break;
        }
    }
    pthread_mutex_unlock(&c->wd.mtx);
    return NULL;
}

static void v8_timedwait(Context *c, const uint8_t *p, size_t n,
                         void (*func)(struct State *pst, const uint8_t *p, size_t n))
{
    pthread_t thr;
    int r;

    r = -1;
    if (c->timeout > 0 && (r = pthread_create(&thr, NULL, v8_watchdog, c))) {
        fprintf(stderr, "mini_racer: watchdog: pthread_create: %s\n", strerror(r));
        fflush(stderr);
    }
    func(c->pst, p, n);
    if (r)
        return;
    pthread_mutex_lock(&c->wd.mtx);
    c->wd.cancel = 1;
    pthread_cond_signal(&c->wd.cv);
    pthread_mutex_unlock(&c->wd.mtx);
    pthread_join(thr, NULL);
    c->wd.cancel = 0;
}

static void dispatch1(Context *c, const uint8_t *p, size_t n)
{
    uint8_t b;

    assert(n > 0);
    switch (*p) {
    case 'A': return v8_attach(c->pst, p+1, n-1);
    case 'C': return v8_timedwait(c, p+1, n-1, v8_call);
    case 'E': return v8_timedwait(c, p+1, n-1, v8_eval);
    case 'H': return v8_heap_snapshot(c->pst);
    case 'P': return v8_pump_message_loop(c->pst);
    case 'S': return v8_heap_stats(c->pst);
    case 'T': return v8_snapshot(c->pst, p+1, n-1);
    case 'W': return v8_warmup(c->pst, p+1, n-1);
    case 'L':
        b = 0;
        v8_reply(c, &b, 1); // doesn't matter what as long as it's not empty
        return v8_low_memory_notification(c->pst);
    }
    fprintf(stderr, "mini_racer: bad request %02x\n", *p);
    fflush(stderr);
}

static void dispatch(Context *c)
{
    buf_reset(&c->res);
    dispatch1(c, c->req.buf, c->req.len);
    buf_reset(&c->req);
}

// called by v8_isolate_and_context
void v8_thread_main(Context *c, struct State *pst)
{
    struct timespec deadline;
    bool issued_idle_gc = true;

    c->pst = pst;
    barrier_wait(&c->late_init);
    pthread_mutex_lock(&c->mtx);
    while (!c->quit) {
        if (!c->req.len) {
            if (c->idle_gc > 0) {
                deadline = deadline_ms(c->idle_gc);
                pthread_cond_timedwait(&c->cv, &c->mtx, &deadline);
                if (deadline_exceeded(deadline) && !issued_idle_gc) {
                    v8_low_memory_notification(c->pst);
                    issued_idle_gc = true;
                }
            } else {
                pthread_cond_wait(&c->cv, &c->mtx);
            }
        }
        if (!c->req.len)
            continue; // spurious wakeup or quit signal from other thread
        dispatch(c);
        issued_idle_gc = false;
        pthread_cond_signal(&c->cv);
    }
}

// called by v8_thread_main and from mini_racer_v8.cc,
// in all cases with Context.mtx held
void v8_dispatch(Context *c)
{
    dispatch1(c, c->req.buf, c->req.len);
    buf_reset(&c->req);
}

// called from mini_racer_v8.cc with Context.mtx held
// only called when inside v8_call, v8_eval, or v8_pump_message_loop
void v8_roundtrip(Context *c, const uint8_t **p, size_t *n)
{
    struct rendezvous_nogvl *args;

    buf_reset(&c->req);
    if (single_threaded) {
        assert(*c->res.buf == 'c'); // js -> ruby callback
        args = &(struct rendezvous_nogvl){c, &c->req, &c->res};
        rb_thread_call_with_gvl(rendezvous_callback, args);
    } else {
        pthread_cond_signal(&c->cv);
        while (!c->req.len)
            pthread_cond_wait(&c->cv, &c->mtx);
    }
    buf_reset(&c->res);
    *p = c->req.buf;
    *n = c->req.len;
}

// called from mini_racer_v8.cc with Context.mtx held
void v8_reply(Context *c, const uint8_t *p, size_t n)
{
    buf_put(&c->res, p, n);
}

static void v8_once_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, v8_global_init);
}

static void *v8_thread_start(void *arg)
{
    Context *c;

    c = arg;
    barrier_wait(&c->early_init);
    v8_once_init();
    v8_thread_init(c, c->snapshot.buf, c->snapshot.len, c->max_memory, c->verbose_exceptions);
    while (c->quit < 2)
        pthread_cond_wait(&c->cv, &c->mtx);
    context_destroy(c);
    return NULL;
}

static VALUE deserialize1(DesCtx *d, const uint8_t *p, size_t n)
{
    char err[64];

    if (des(&err, p, n, d))
        rb_raise(runtime_error, "%s", err);
    if (d->tos != d->stack) // should not happen
        rb_raise(runtime_error, "parse stack not empty");
    return d->tos->a;
}

static VALUE deserialize(VALUE arg)
{
    struct rendezvous_des *a;
    Buf *b;

    a = (void *)arg;
    b = a->res;
    return deserialize1(a->d, b->buf, b->len);
}

// called with |rr_mtx| and GVL held; can raise exception
static VALUE rendezvous_callback_do(VALUE arg)
{
    struct rendezvous_nogvl *a;
    VALUE func, args;
    Context *c;
    DesCtx d;
    Buf *b;

    a = (void *)arg;
    b = a->res;
    c = a->context;
    assert(b->len > 0);
    assert(*b->buf == 'c');
    DesCtx_init(&d);
    args = deserialize1(&d, b->buf+1, b->len-1); // skip 'c' marker
    func = rb_ary_pop(args); // callback id
    func = rb_ary_entry(c->procs, FIX2LONG(func));
    return rb_funcall2(func, rb_intern("call"), RARRAY_LENINT(args), RARRAY_PTR(args));
}

// called with |rr_mtx| and GVL held; |mtx| is unlocked
// callback data is in |a->res|, serialized result goes in |a->req|
static void *rendezvous_callback(void *arg)
{
    struct rendezvous_nogvl *a;
    Context *c;
    int exc;
    VALUE r;
    Ser s;

    a = arg;
    c = a->context;
    r = rb_protect(rendezvous_callback_do, (VALUE)a, &exc);
    if (exc) {
        c->exception = rb_errinfo();
        rb_set_errinfo(Qnil);
        goto fail;
    }
    ser_init1(&s, 'c'); // callback reply
    if (serialize(&s, r)) { // should not happen
        c->exception = rb_exc_new_cstr(internal_error, s.err);
        ser_reset(&s);
        goto fail;
    }
out:
    buf_move(&s.b, a->req);
    return NULL;
fail:
    ser_init1(&s, 'e'); // exception pending
    goto out;
}

static inline void *rendezvous_nogvl(void *arg)
{
    struct rendezvous_nogvl *a;
    Context *c;

    a = arg;
    c = a->context;
    pthread_mutex_lock(&c->rr_mtx);
    if (c->depth > 0 && c->depth%50 == 0) { // TODO stop steep recursion
        fprintf(stderr, "mini_racer: deep js->ruby->js recursion, depth=%d\n", c->depth);
        fflush(stderr);
    }
    c->depth++;
next:
    pthread_mutex_lock(&c->mtx);
    assert(c->req.len == 0);
    assert(c->res.len == 0);
    buf_move(a->req, &c->req); // v8 thread takes ownership of req
    if (single_threaded) {
        v8_single_threaded_enter(c->pst, c, dispatch);
    } else {
        pthread_cond_signal(&c->cv);
        do pthread_cond_wait(&c->cv, &c->mtx); while (!c->res.len);
    }
    buf_move(&c->res, a->res);
    pthread_mutex_unlock(&c->mtx);
    if (*a->res->buf == 'c') { // js -> ruby callback?
        rb_thread_call_with_gvl(rendezvous_callback, a);
        goto next;
    }
    c->depth--;
    pthread_mutex_unlock(&c->rr_mtx);
    return NULL;
}

static void rendezvous_no_des(Context *c, Buf *req, Buf *res)
{
    if (atomic_load(&c->quit)) {
        buf_reset(req);
        rb_raise(context_disposed_error, "disposed context");
    }
    rb_nogvl(rendezvous_nogvl, &(struct rendezvous_nogvl){c, req, res},
             NULL, NULL, 0);
}

// send request to & receive reply from v8 thread; takes ownership of |req|
// can raise exceptions and longjmp away but won't leak |req|
static VALUE rendezvous1(Context *c, Buf *req, DesCtx *d)
{
    VALUE r;
    Buf res;
    int exc;

    rendezvous_no_des(c, req, &res); // takes ownership of |req|
    r = rb_protect(deserialize, (VALUE)&(struct rendezvous_des){d, &res}, &exc);
    buf_reset(&res);
    if (exc) {
        r = rb_errinfo();
        rb_set_errinfo(Qnil);
        rb_exc_raise(r);
    }
    if (!NIL_P(c->exception)) {
        r = c->exception;
        c->exception = Qnil;
        rb_exc_raise(r);
    }
    return r;
}

static VALUE rendezvous(Context *c, Buf *req)
{
    DesCtx d;

    DesCtx_init(&d);
    return rendezvous1(c, req, &d);
}

static void handle_exception(VALUE e)
{
    const char *s;
    VALUE klass;

    if (NIL_P(e))
        return;
    StringValue(e);
    s = RSTRING_PTR(e);
    switch (*s) {
    case NO_ERROR:
        return;
    case INTERNAL_ERROR:
        klass = internal_error;
        break;
    case MEMORY_ERROR:
        klass = memory_error;
        break;
    case PARSE_ERROR:
        klass = parse_error;
        break;
    case RUNTIME_ERROR:
        klass = runtime_error;
        break;
    case TERMINATED_ERROR:
        klass = terminated_error;
        break;
    default:
        rb_raise(internal_error, "bad error class %02x", *s);
    }
    rb_raise(klass, "%s", s+1);
}

static VALUE context_alloc(VALUE klass)
{
    pthread_mutexattr_t mattr;
    pthread_condattr_t cattr;
    const char *cause;
    Context *c;
    VALUE f, a;
    int r;

    // Safe to lazy init because we hold the GVL
    if (NIL_P(date_time_class)) {
        f = rb_intern("const_defined?");
        a = rb_str_new_cstr("DateTime");
        if (Qtrue == rb_funcall(rb_cObject, f, 1, a))
            date_time_class = rb_const_get(rb_cObject, rb_intern("DateTime"));
    }
    c = ruby_xmalloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    c->exception = Qnil;
    c->procs = rb_ary_new();
    buf_init(&c->snapshot);
    buf_init(&c->req);
    buf_init(&c->res);
    cause = "pthread_condattr_init";
    if ((r = pthread_condattr_init(&cattr)))
        goto fail0;
#ifndef __APPLE__
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
#endif
    cause = "pthread_mutexattr_init";
    if ((r = pthread_mutexattr_init(&mattr)))
        goto fail1;
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    cause = "pthread_mutex_init";
    r = pthread_mutex_init(&c->rr_mtx, &mattr);
    pthread_mutexattr_destroy(&mattr);
    if (r)
        goto fail1;
    if (pthread_mutex_init(&c->mtx, NULL))
        goto fail2;
    cause = "pthread_cond_init";
    if ((r = pthread_cond_init(&c->cv, &cattr)))
        goto fail3;
    cause = "pthread_mutex_init";
    if ((r = pthread_mutex_init(&c->wd.mtx, NULL)))
        goto fail4;
    cause = "pthread_cond_init";
    if (pthread_cond_init(&c->wd.cv, &cattr))
        goto fail5;
    cause = "barrier_init";
    if ((r = barrier_init(&c->early_init, 2)))
        goto fail6;
    cause = "barrier_init";
    if ((r = barrier_init(&c->late_init, 2)))
        goto fail7;
    pthread_condattr_destroy(&cattr);
    return TypedData_Wrap_Struct(klass, &context_type, c);
fail7:
    barrier_destroy(&c->early_init);
fail6:
    pthread_cond_destroy(&c->wd.cv);
fail5:
    pthread_mutex_destroy(&c->wd.mtx);
fail4:
    pthread_cond_destroy(&c->cv);
fail3:
    pthread_mutex_destroy(&c->mtx);
fail2:
    pthread_mutex_destroy(&c->rr_mtx);
fail1:
    pthread_condattr_destroy(&cattr);
fail0:
    ruby_xfree(c);
    rb_raise(runtime_error, "%s: %s", cause, strerror(r));
    return Qnil; // pacify compiler
}

static void *context_free_thread_do(void *arg)
{
    Context *c;

    c = arg;
    v8_single_threaded_dispose(c->pst);
    context_destroy(c);
    return NULL;
}

static void context_free_thread(Context *c)
{
    pthread_t thr;
    int r;

    // dispose on another thread so we don't block when trying to
    // enter an isolate that's in a stuck state; that *should* be
    // impossible but apparently it happened regularly before the
    // rewrite and I'm carrying it over out of an abundance of caution
    if ((r = pthread_create(&thr, NULL, context_free_thread_do, c))) {
        fprintf(stderr, "mini_racer: pthread_create: %s", strerror(r));
        fflush(stderr);
        context_free_thread_do(c);
    } else {
        pthread_detach(thr);
    }
}

static void context_free(void *arg)
{
    Context *c;

    c = arg;
    if (single_threaded) {
        context_free_thread(c);
    } else {
        pthread_mutex_lock(&c->mtx);
        c->quit = 2; // 2 = v8 thread frees
        pthread_cond_signal(&c->cv);
        pthread_mutex_unlock(&c->mtx);
    }
}

static void context_destroy(Context *c)
{
    pthread_mutex_unlock(&c->mtx);
    pthread_mutex_destroy(&c->mtx);
    pthread_cond_destroy(&c->cv);
    barrier_destroy(&c->early_init);
    barrier_destroy(&c->late_init);
    pthread_mutex_destroy(&c->wd.mtx);
    pthread_cond_destroy(&c->wd.cv);
    buf_reset(&c->snapshot);
    buf_reset(&c->req);
    buf_reset(&c->res);
    ruby_xfree(c);
}

static void context_mark(void *arg)
{
    Context *c;

    c = arg;
    rb_gc_mark(c->procs);
    rb_gc_mark(c->exception);
}

static size_t context_size(const void *arg)
{
    const Context *c = arg;
    return sizeof(*c);
}

static VALUE context_attach(VALUE self, VALUE name, VALUE proc)
{
    Context *c;
    VALUE e;
    Ser s;

    TypedData_Get_Struct(self, Context, &context_type, c);
    // request is (A)ttach, [name, id] array
    ser_init1(&s, 'A');
    ser_array_begin(&s, 2);
    add_string(&s, name);
    ser_int(&s, RARRAY_LENINT(c->procs));
    ser_array_end(&s, 2);
    rb_ary_push(c->procs, proc);
    // response is an exception or undefined
    e = rendezvous(c, &s.b);
    handle_exception(e);
    return Qnil;
}

static void *context_dispose_do(void *arg)
{
    Context *c;

    c = arg;
    if (single_threaded) {
        atomic_store(&c->quit, 1);   // disposed
        // intentionally a no-op for now
    } else {
        pthread_mutex_lock(&c->mtx);
        while (c->req.len || c->res.len)
            pthread_cond_wait(&c->cv, &c->mtx);
        atomic_store(&c->quit, 1);   // disposed
        pthread_cond_signal(&c->cv); // wake up v8 thread
        pthread_mutex_unlock(&c->mtx);
    }
    return NULL;
}

static VALUE context_dispose(VALUE self)
{
    Context *c;

    TypedData_Get_Struct(self, Context, &context_type, c);
    rb_thread_call_without_gvl(context_dispose_do, c, NULL, NULL);
    return Qnil;
}

static VALUE context_stop(VALUE self)
{
    Context *c;

    // does not grab |mtx| because Context.stop can be called from another
    // thread and then we deadlock if e.g. the V8 thread busy-loops in JS
    TypedData_Get_Struct(self, Context, &context_type, c);
    if (atomic_load(&c->quit))
        rb_raise(context_disposed_error, "disposed context");
    v8_terminate_execution(c->pst);
    return Qnil;
}

static VALUE context_call(int argc, VALUE *argv, VALUE self)
{
    VALUE name, args;
    VALUE a, e;
    Context *c;
    Ser s;

    TypedData_Get_Struct(self, Context, &context_type, c);
    rb_scan_args(argc, argv, "1*", &name, &args);
    Check_Type(name, T_STRING);
    rb_ary_unshift(args, name);
    // request is (C)all, [name, args...] array
    ser_init1(&s, 'C');
    if (serialize(&s, args)) {
        ser_reset(&s);
        rb_raise(runtime_error, "Context.call: %s", s.err);
    }
    // response is [result, err] array
    a = rendezvous(c, &s.b); // takes ownership of |s.b|
    e = rb_ary_pop(a);
    handle_exception(e);
    return rb_ary_pop(a);
}

static VALUE context_eval(int argc, VALUE *argv, VALUE self)
{
    VALUE a, e, source, filename, kwargs;
    Context *c;
    Ser s;

    TypedData_Get_Struct(self, Context, &context_type, c);
    filename = Qnil;
    rb_scan_args(argc, argv, "1:", &source, &kwargs);
    Check_Type(source, T_STRING);
    if (!NIL_P(kwargs))
        filename = rb_hash_aref(kwargs, rb_id2sym(rb_intern("filename")));
    if (NIL_P(filename))
        filename = rb_str_new_cstr("<eval>");
    Check_Type(filename, T_STRING);
    // request is (E)val, [filename, source] array
    ser_init1(&s, 'E');
    ser_array_begin(&s, 2);
    add_string(&s, filename);
    add_string(&s, source);
    ser_array_end(&s, 2);
    // response is [result, errname] array
    a = rendezvous(c, &s.b); // takes ownership of |s.b|
    e = rb_ary_pop(a);
    handle_exception(e);
    return rb_ary_pop(a);
}

static VALUE context_heap_stats(VALUE self)
{
    VALUE a, h, k, v;
    Context *c;
    int i, n;
    Buf b;

    TypedData_Get_Struct(self, Context, &context_type, c);
    buf_init(&b);
    buf_putc(&b, 'S');     // (S)tats, returns object
    h = rendezvous(c, &b); // takes ownership of |b|
    a = rb_ary_new();
    rb_hash_foreach(h, collect, a);
    for (i = 0, n = RARRAY_LENINT(a); i < n; i += 2) {
        k = rb_ary_entry(a, i+0);
        v = rb_ary_entry(a, i+1);
        rb_hash_delete(h, k);
        rb_hash_aset(h, rb_str_intern(k), v); // turn "key" into :key
    }
    return h;
}

static VALUE context_heap_snapshot(VALUE self)
{
    Buf req, res;
    Context *c;

    TypedData_Get_Struct(self, Context, &context_type, c);
    buf_init(&req);
    buf_putc(&req, 'H');              // (H)eap snapshot, returns plain bytes
    rendezvous_no_des(c, &req, &res); // takes ownership of |req|
    return rb_utf8_str_new((char *)res.buf, res.len);
}

static VALUE context_pump_message_loop(VALUE self)
{
    Context *c;
    Buf b;

    TypedData_Get_Struct(self, Context, &context_type, c);
    buf_init(&b);
    buf_putc(&b, 'P');        // (P)ump, returns bool
    return rendezvous(c, &b); // takes ownership of |b|
}

static VALUE context_low_memory_notification(VALUE self)
{
    Buf req, res;
    Context *c;

    TypedData_Get_Struct(self, Context, &context_type, c);
    buf_init(&req);
    buf_putc(&req, 'L');              // (L)ow memory notification, returns nothing
    rendezvous_no_des(c, &req, &res); // takes ownership of |req|
    return Qnil;
}

static int platform_set_flag1(VALUE k, VALUE v)
{
    char *p, *q, buf[256];
    int ok;

    k = rb_funcall(k, rb_intern("to_s"), 0);
    Check_Type(k, T_STRING);
    if (!NIL_P(v)) {
        v = rb_funcall(v, rb_intern("to_s"), 0);
        Check_Type(v, T_STRING);
    }
    p = RSTRING_PTR(k);
    if (!strncmp(p, "--", 2))
        p += 2;
    if (NIL_P(v)) {
        snprintf(buf, sizeof(buf), "--%s", p);
    } else {
        snprintf(buf, sizeof(buf), "--%s=%s", p, RSTRING_PTR(v));
    }
    p = buf;
    pthread_mutex_lock(&flags_mtx);
    if (!flags.buf)
        buf_init(&flags);
    ok = (*flags.buf != 1);
    if (ok) {
        buf_put(&flags, p, 1+strlen(p)); // include trailing \0
        // strip dashes and underscores to reduce the number of variant
        // spellings (--no-single-threaded, --nosingle-threaded,
        // --no_single_threaded, etc.)
        p = q = buf;
        for (;;) {
            if (*p != '-')
                if (*p != '_')
                    *q++ = *p;
            if (!*p++)
                break;
        }
        if (!strcmp(buf, "singlethreaded")) {
            single_threaded = 1;
        } else if (!strcmp(buf, "nosinglethreaded")) {
            single_threaded = 0;
        }
    }
    pthread_mutex_unlock(&flags_mtx);
    return ok;
}

static VALUE platform_set_flags(int argc, VALUE *argv, VALUE klass)
{
    VALUE args, kwargs, k, v;
    int i, n;

    (void)&klass;
    rb_scan_args(argc, argv, "*:", &args, &kwargs);
    Check_Type(args, T_ARRAY);
    for (i = 0, n = RARRAY_LENINT(args); i < n; i++) {
        k = rb_ary_entry(args, i);
        v = Qnil;
        if (!platform_set_flag1(k, v))
            goto fail;
    }
    if (NIL_P(kwargs))
        return Qnil;
    Check_Type(kwargs, T_HASH);
    args = rb_ary_new();
    rb_hash_foreach(kwargs, collect, args);
    for (i = 0, n = RARRAY_LENINT(args); i < n; i += 2) {
        k = rb_ary_entry(args, i+0);
        v = rb_ary_entry(args, i+1);
        if (!platform_set_flag1(k, v))
            goto fail;
    }
    return Qnil;
fail:
    rb_raise(platform_init_error, "platform already initialized");
}

// called by v8_global_init; caller must free |*p| with free()
void v8_get_flags(char **p, size_t *n)
{
    *p = NULL;
    *n = 0;
    pthread_mutex_lock(&flags_mtx);
    if (!flags.len)
        goto out;
    *p = malloc(flags.len);
    if (!*p)
        goto out;
    *n = flags.len;
    memcpy(*p, flags.buf, *n);
    buf_reset(&flags);
out:
    buf_init(&flags);
    buf_putc(&flags, 1); // marker to indicate it's been cleared
    pthread_mutex_unlock(&flags_mtx);
    if (single_threaded)
        rb_thread_lock_native_thread();
}

static VALUE context_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE kwargs, a, k, v;
    pthread_attr_t attr;
    const char *cause;
    pthread_t thr;
    Snapshot *ss;
    Context *c;
    char *s;
    int r;

    TypedData_Get_Struct(self, Context, &context_type, c);
    rb_scan_args(argc, argv, ":", &kwargs);
    if (NIL_P(kwargs))
        goto init;
    a = rb_ary_new();
    rb_hash_foreach(kwargs, collect, a);
    while (RARRAY_LENINT(a)) {
        v = rb_ary_pop(a);
        k = rb_ary_pop(a);
        k = rb_sym2str(k);
        s = RSTRING_PTR(k);
        if (!strcmp(s, "ensure_gc_after_idle")) {
            Check_Type(v, T_FIXNUM);
            c->idle_gc = FIX2LONG(v);
            if (c->idle_gc < 0 || c->idle_gc > INT32_MAX)
                rb_raise(rb_eArgError, "bad ensure_gc_after_idle");
        } else if (!strcmp(s, "max_memory")) {
            Check_Type(v, T_FIXNUM);
            c->max_memory = FIX2LONG(v);
            if (c->max_memory < 0 || c->max_memory >= UINT32_MAX)
                rb_raise(rb_eArgError, "bad max_memory");
        } else if (!strcmp(s, "marshal_stack_depth")) { // backcompat, ignored
            Check_Type(v, T_FIXNUM);
        } else if (!strcmp(s, "timeout")) {
            Check_Type(v, T_FIXNUM);
            c->timeout = FIX2LONG(v);
            if (c->timeout < 0 || c->timeout > INT32_MAX)
                rb_raise(rb_eArgError, "bad timeout");
        } else if (!strcmp(s, "snapshot")) {
            if (NIL_P(v))
                continue;
            TypedData_Get_Struct(v, Snapshot, &snapshot_type, ss);
            if (buf_put(&c->snapshot, RSTRING_PTR(ss->blob), RSTRING_LENINT(ss->blob)))
                rb_raise(runtime_error, "out of memory");
        } else if (!strcmp(s, "verbose_exceptions")) {
            c->verbose_exceptions = !(v == Qfalse || v == Qnil);
        } else {
            rb_raise(runtime_error, "bad keyword: %s", s);
        }
    }
init:
    if (single_threaded) {
        v8_once_init();
        c->pst = v8_thread_init(c, c->snapshot.buf, c->snapshot.len, c->max_memory, c->verbose_exceptions);
    } else {
        cause = "pthread_attr_init";
        if ((r = pthread_attr_init(&attr)))
            goto fail;
        pthread_attr_setstacksize(&attr, 2<<20); // 2 MiB
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        // v8 thread takes ownership of |c|
        cause = "pthread_create";
        r = pthread_create(&thr, &attr, v8_thread_start, c);
        pthread_attr_destroy(&attr);
        if (r)
            goto fail;
        barrier_wait(&c->early_init);
        barrier_wait(&c->late_init);
    }
    return Qnil;
fail:
    rb_raise(runtime_error, "Context.initialize: %s: %s", cause, strerror(r));
    return Qnil; // pacify compiler
}

static VALUE snapshot_alloc(VALUE klass)
{
    Snapshot *ss;

    ss = ruby_xmalloc(sizeof(*ss));
    ss->blob = rb_enc_str_new("", 0, rb_ascii8bit_encoding());
    return TypedData_Wrap_Struct(klass, &snapshot_type, ss);
}

static void snapshot_free(void *arg)
{
    ruby_xfree(arg);
}

static void snapshot_mark(void *arg)
{
    Snapshot *ss;

    ss = arg;
    rb_gc_mark(ss->blob);
}

static size_t snapshot_size(const void *arg)
{
    const Snapshot *ss;

    ss = arg;
    return sizeof(*ss) + RSTRING_LENINT(ss->blob);
}

static VALUE snapshot_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE a, e, code, cv;
    Snapshot *ss;
    Context *c;
    DesCtx d;
    Ser s;

    TypedData_Get_Struct(self, Snapshot, &snapshot_type, ss);
    rb_scan_args(argc, argv, "01", &code);
    if (NIL_P(code))
        code = rb_str_new_cstr("");
    Check_Type(code, T_STRING);
    cv = context_alloc(context_class);
    context_initialize(0, NULL, cv);
    TypedData_Get_Struct(cv, Context, &context_type, c);
    // request is snapsho(T), "code"
    ser_init1(&s, 'T');
    add_string(&s, code);
    // response is [arraybuffer, error]
    DesCtx_init(&d);
    d.transcode_latin1 = 0; // don't mangle snapshot binary data
    a = rendezvous1(c, &s.b, &d);
    e = rb_ary_pop(a);
    context_dispose(cv);
    if (*RSTRING_PTR(e))
        rb_raise(snapshot_error, "%s", RSTRING_PTR(e)+1);
    ss->blob = rb_ary_pop(a);
    return Qnil;
}

static VALUE snapshot_warmup(VALUE self, VALUE arg)
{
    VALUE a, e, cv;
    Snapshot *ss;
    Context *c;
    DesCtx d;
    Ser s;

    TypedData_Get_Struct(self, Snapshot, &snapshot_type, ss);
    Check_Type(arg, T_STRING);
    cv = context_alloc(context_class);
    context_initialize(0, NULL, cv);
    TypedData_Get_Struct(cv, Context, &context_type, c);
    // request is (W)armup, [snapshot, "warmup code"]
    ser_init1(&s, 'W');
    ser_array_begin(&s, 2);
    ser_string8(&s, (const uint8_t *)RSTRING_PTR(ss->blob), RSTRING_LENINT(ss->blob));
    add_string(&s, arg);
    ser_array_end(&s, 2);
    // response is [arraybuffer, error]
    DesCtx_init(&d);
    d.transcode_latin1 = 0; // don't mangle snapshot binary data
    a = rendezvous1(c, &s.b, &d);
    e = rb_ary_pop(a);
    context_dispose(cv);
    if (*RSTRING_PTR(e))
        rb_raise(snapshot_error, "%s", RSTRING_PTR(e)+1);
    ss->blob = rb_ary_pop(a);
    return self;
}

static VALUE snapshot_dump(VALUE self)
{
    Snapshot *ss;

    TypedData_Get_Struct(self, Snapshot, &snapshot_type, ss);
    return ss->blob;
}

static VALUE snapshot_size0(VALUE self)
{
    Snapshot *ss;

    TypedData_Get_Struct(self, Snapshot, &snapshot_type, ss);
    return LONG2FIX(RSTRING_LENINT(ss->blob));
}

__attribute__((visibility("default")))
void Init_mini_racer_extension(void)
{
    VALUE c, m;

    m = rb_define_module("MiniRacer");
    c = rb_define_class_under(m, "Error", rb_eStandardError);
    snapshot_error = rb_define_class_under(m, "SnapshotError", c);
    platform_init_error = rb_define_class_under(m, "PlatformAlreadyInitialized", c);
    context_disposed_error = rb_define_class_under(m, "ContextDisposedError", c);

    c = rb_define_class_under(m, "EvalError", c);
    parse_error = rb_define_class_under(m, "ParseError", c);
    memory_error = rb_define_class_under(m, "V8OutOfMemoryError", c);
    runtime_error = rb_define_class_under(m, "RuntimeError", c);
    internal_error = rb_define_class_under(m, "InternalError", c);
    terminated_error = rb_define_class_under(m, "ScriptTerminatedError", c);

    c = context_class = rb_define_class_under(m, "Context", rb_cObject);
    rb_define_method(c, "initialize", context_initialize, -1);
    rb_define_method(c, "attach", context_attach, 2);
    rb_define_method(c, "dispose", context_dispose, 0);
    rb_define_method(c, "stop", context_stop, 0);
    rb_define_method(c, "call", context_call, -1);
    rb_define_method(c, "eval", context_eval, -1);
    rb_define_method(c, "heap_stats", context_heap_stats, 0);
    rb_define_method(c, "heap_snapshot", context_heap_snapshot, 0);
    rb_define_method(c, "pump_message_loop", context_pump_message_loop, 0);
    rb_define_method(c, "low_memory_notification", context_low_memory_notification, 0);
    rb_define_alloc_func(c, context_alloc);

    c = snapshot_class = rb_define_class_under(m, "Snapshot", rb_cObject);
    rb_define_method(c, "initialize", snapshot_initialize, -1);
    rb_define_method(c, "warmup!", snapshot_warmup, 1);
    rb_define_method(c, "dump", snapshot_dump, 0);
    rb_define_method(c, "size", snapshot_size0, 0);
    rb_define_alloc_func(c, snapshot_alloc);

    c = rb_define_class_under(m, "Platform", rb_cObject);
    rb_define_singleton_method(c, "set_flags!", platform_set_flags, -1);

    date_time_class = Qnil; // lazy init
    js_function_class = rb_define_class_under(m, "JavaScriptFunction", rb_cObject);
}
