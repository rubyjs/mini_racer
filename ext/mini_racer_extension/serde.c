#include <err.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void des_null(void *arg);
static void des_undefined(void *arg);
static void des_bool(void *arg, int v);
static void des_int(void *arg, int64_t v);
static void des_num(void *arg, double v);
static void des_date(void *arg, double v);
// des_bigint: |p| points to |n|/8 quadwords in little-endian order
// des_bigint: |p| is not quadword aligned
// des_bigint: |n| is in bytes, not quadwords
// des_bigint: |n| is zero when bigint is zero
// des_bigint: |sign| is 1 or -1
static void des_bigint(void *arg, const void *p, size_t n, int sign);
static void des_string(void *arg, const char *s, size_t n);
static void des_string8(void *arg, const uint8_t *s, size_t n);
// des_string16: |s| is not word aligned
// des_string16: |n| is in bytes, not code points
static void des_string16(void *arg, const void *s, size_t n);
static void des_arraybuffer(void *arg, const void *s, size_t n);
static void des_array_begin(void *arg);
static void des_array_end(void *arg);
// called if e.g. an array object has named properties
static void des_named_props_begin(void *arg);
static void des_named_props_end(void *arg);
static void des_object_begin(void *arg);
static void des_object_end(void *arg);
static void des_map_begin(void *arg);
static void des_map_end(void *arg);
static void des_object_ref(void *arg, uint32_t id);
// des_error_begin: followed by des_object_begin + des_object_end calls
static void des_error_begin(void *arg);
static void des_error_end(void *arg);

// dynamically sized buffer with inline storage so we don't
// have to worry about allocation failures for small payloads
typedef struct Buf {
    uint8_t *buf;
    uint32_t len, cap;
    uint8_t buf_s[48];
} Buf;

typedef struct Ser {
    Buf b;
    char err[64];
} Ser;

static const uint8_t the_nan[8] = {0,0,0,0,0,0,0xF8,0x7F}; // canonical nan

// note: returns |v| if v in [0,1,2]
static inline uint32_t next_power_of_two(uint32_t v) {
  v -= 1;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v += 1;
  return v;
}

static inline void buf_init(Buf *b)
{
    b->len = 0;
    b->buf = b->buf_s;
    b->cap = sizeof(b->buf_s);
}

static inline void buf_reset(Buf *b)
{
    if (b->buf != b->buf_s)
        free(b->buf);
    buf_init(b);
}

static inline void buf_move(Buf *s, Buf *d)
{
    if (s == d)
        return;
    *d = *s;
    if (s->buf == s->buf_s)
        d->buf = d->buf_s;
    buf_init(s);
}

static inline int buf_grow(Buf *b, size_t n)
{
    void *p;

    if ((uint64_t)n + b->len > UINT32_MAX)
        return -1;
    n += b->len;
    if (n < b->cap)
        return 0;
    n = next_power_of_two(n);
    p = NULL;
    if (b->buf != b->buf_s)
        p = b->buf;
    p = realloc(p, n);
    if (!p)
        return -1;
    if (b->buf == b->buf_s)
        memcpy(p, b->buf_s, b->len);
    b->buf = p;
    b->cap = n;
    return 0;
}

static inline int buf_put(Buf *b, const void *p, size_t n)
{
    if (n == 0)
        return 0;
    if (buf_grow(b, n))
        return -1;
    memcpy(&b->buf[b->len], p, n);
    b->len += n;
    return 0;
}

static inline int buf_putc(Buf *b, uint8_t c)
{
    return buf_put(b, &c, 1);
}

static inline void w(Ser *s, const void *p, size_t n)
{
    if (*s->err)
        return;
    if (buf_put(&s->b, p, n))
        snprintf(s->err, sizeof(s->err), "out of memory");
}

static inline void w_byte(Ser *s, uint8_t c)
{
    w(s, &c, 1);
}

static inline void w_varint(Ser *s, uint64_t v)
{
    uint8_t b[10]; // 10 == 1 + 64/7
    size_t n;

    for (n = 0; v > 127; v >>= 7)
        b[n++] = 128 | (v & 127);
    b[n++] = v;
    w(s, b, n);
}

static inline void w_zigzag(Ser *s, int64_t v)
{
    uint64_t t;

    if (v < 0) {
        t = -v;
    } else {
        t = v;
    }
    t += t;
    t -= (v < 0);
    w_varint(s, t);
}

static inline int r_varint(const uint8_t **p, const uint8_t *pe, uint64_t *r)
{
    int i, k;

    for (i = 0; i < 5; i++) {
        if (*p+i == pe)
            return -1;
        if ((*p)[i] < 128)
            goto ok;
    }
    return -1;
ok:
    *r = 0;
    for (k = 0; k <= i; k++, (*p)++)
        *r |= (uint64_t)(**p & 127) << 7*k;
    return 0;
}

static inline int r_zigzag(const uint8_t **p, const uint8_t *pe, int64_t *r)
{
    uint64_t v;

    if (r_varint(p, pe, &v))
        return -1;
    *r = v&1 ? -(v/2)-1 : v/2;
    return 0;
}

static inline void ser_init(Ser *s)
{
    memset(s, 0, sizeof(*s));
    buf_init(&s->b);
    w(s, "\xFF\x0F", 2);
}

static void ser_init1(Ser *s, uint8_t c)
{
    memset(s, 0, sizeof(*s));
    buf_init(&s->b);
    w_byte(s, c);
    w(s, "\xFF\x0F", 2);
}

static void ser_reset(Ser *s)
{
    buf_reset(&s->b);
}

static void ser_null(Ser *s)
{
    w_byte(s, '0');
}

static void ser_undefined(Ser *s)
{
    w_byte(s, '_');
}

static void ser_bool(Ser *s, int v)
{
    w_byte(s, "TF"[!v]);
}

static void ser_num(Ser *s, double v)
{
    w_byte(s, 'N');
    if (isnan(v)) {
        w(s, the_nan, sizeof(the_nan));
    } else {
        w(s, &v, sizeof(v));
    }
}

static void ser_int(Ser *s, int64_t v)
{
    if (*s->err)
        return;
    if (v < INT32_MIN || v > INT32_MAX) {
        if (v > INT64_MIN/1024)
            if (v <= INT64_MAX/1024)
                return ser_num(s, v);
        snprintf(s->err, sizeof(s->err), "out of range: %lld", (long long)v);
    } else {
        w_byte(s, 'I');
        w_zigzag(s, v);
    }
}

// |v| is the timestamp in milliseconds since the UNIX epoch
static void ser_date(Ser *s, double v)
{
    w_byte(s, 'D');
    if (isfinite(v)) {
        w(s, &v, sizeof(v));
    } else {
        w(s, the_nan, sizeof(the_nan));
    }
}

// ser_bigint: |n| is in bytes, not quadwords
static void ser_bigint(Ser *s, const uint64_t *p, size_t n, int sign)
{
    if (*s->err)
        return;
    if (n % 8) {
        snprintf(s->err, sizeof(s->err), "bad bigint");
        return;
    }
    w_byte(s, 'Z');
    // chop off high all-zero words
    n /= 8;
    while (n--)
        if (p[n])
            break;
    if (n == (size_t)-1) {
        w_byte(s, 0); // normalized zero
    } else {
        n = 8*n + 8;
        w_varint(s, 2*n + (sign < 0));
        w(s, p, n);
    }
}

// string must be utf8
static void ser_string(Ser *s, const char *p, size_t n)
{
    w_byte(s, 'S');
    w_varint(s, n);
    w(s, p, n);
}

// string must be latin1
static void ser_string8(Ser *s, const uint8_t *p, size_t n)
{
    w_byte(s, '"');
    w_varint(s, n);
    w(s, p, n);
}

// string must be utf16le; |n| is in bytes, not code points
static void ser_string16(Ser *s, const void *p, size_t n)
{
    w_byte(s, 'c');
    w_varint(s, n);
    w(s, p, n);
}

static void ser_object_begin(Ser *s)
{
    w_byte(s, 'o');
}

// |count| is the property count
static void ser_object_end(Ser *s, uint32_t count)
{
    w_byte(s, '{');
    w_varint(s, count);
}

static void ser_object_ref(Ser *s, uint32_t id)
{
    w_byte(s, '^');
    w_varint(s, id);
}

static void ser_array_begin(Ser *s, uint32_t count)
{
    w_byte(s, 'A');     // 'A'=dense, 'a'=sparse
    w_varint(s, count); // element count
}

// |count| is the element count
static void ser_array_end(Ser *s, uint32_t count)
{
    w_byte(s, '$');
    w_varint(s, 0);     // property count, always zero
    w_varint(s, count); // element count
}

static int bail(char (*err)[64], const char *str)
{
    snprintf(*err, sizeof(*err), "%s", str);
    return -1;
}

static int des1_num(const uint8_t **p, const uint8_t *pe, double *d)
{
    if (pe-*p < (int)sizeof(*d))
        return -1;
    memcpy(d, *p, sizeof(*d));
    *p += sizeof(*d);
    if (isnan(*d))
        memcpy(d, the_nan, sizeof(the_nan));
    return 0;
}

static int des1(char (*err)[64], const uint8_t **p, const uint8_t *pe,
                void *arg, int depth)
{
    uint64_t s, t, u;
    uint8_t c;
    int64_t i;
    double d;

    if (depth < 0)
        return bail(err, "too much recursion");
again:
    if (*p >= pe)
        goto too_short;
    switch ((c = *(*p)++)) {
    default:
        if (c > 32 && c < 127) {
            snprintf(*err, sizeof(*err), "bad tag: %c", c);
        } else {
            snprintf(*err, sizeof(*err), "bad tag: %02x", c);
        }
        return -1;
    case '\0': // skip alignment padding for two-byte strings
        if (*p < pe)
            goto again;
        break;
    case '^':
        if (r_varint(p, pe, &u))
            goto bad_varint;
        des_object_ref(arg, u);
        // object refs can (but need not be) followed by a typed array
        // that is a view over the arraybufferview
        goto typed_array;
    case '0':
        des_null(arg);
        break;
    case '_':
        des_undefined(arg);
        break;
    case 'A': // dense array
        if (r_varint(p, pe, &u))
            goto bad_varint;
        t = u;
        des_array_begin(arg);
        while (u--) {
            if (*p >= pe)
                goto too_short;
            // '-' is 'the hole', a marker for representing absent
            // elements that is inserted when a dense array turns
            // sparse during serialization; we replace it with undefined
            if (**p == '-') {
                (*p)++;
                des_undefined(arg);
            } else {
                if (des1(err, p, pe, arg, depth-1))
                    return -1;
            }
        }
        for (s = 0; /*empty*/; s++) {
            if (*p >= pe)
                goto too_short;
            if (**p == '$')
                break;
            if (s < 1)
                des_named_props_begin(arg);
            if (des1(err, p, pe, arg, depth-1)) // key
                return -1;
            if (des1(err, p, pe, arg, depth-1)) // value
                return -1;
        }
        (*p)++;
        if (s > 0)
            des_named_props_end(arg);
        if (r_varint(p, pe, &u))
            goto bad_varint;
        if (s != u)
            return bail(err, "array property count mismatch");
        if (r_varint(p, pe, &u))
            goto bad_varint;
        if (t != u)
            return bail(err, "array element count mismatch");
        des_array_end(arg);
        break;
    case 'B': // arraybuffer
    case '~': // resizable arraybuffer (RAB)
        if (r_varint(p, pe, &u))
            goto bad_varint;
        if (c == '~')
            if (r_varint(p, pe, &t)) // maxByteLength, unused
                goto bad_varint;
        if (pe-*p < (int64_t)u)
            goto too_short;
        des_arraybuffer(arg, *p, u);
        *p += u;
        // arraybuffers can (but need not be) followed by a typed array
        // that is a view over the arraybufferview
        // typed arrays aren't efficiently representable in ruby, and the
        // concept of a memory view is wholly unrepresentable, so we
        // simply skip over them; callers get just the arraybuffer
    typed_array:
        if (pe-*p < 2)
            break;
        if (**p != 'V')
            break;
        (*p)++;
        c = *(*p)++;
        // ? DataView
        // B Uint8Array
        // C Uint8ClampedArray
        // D Uint32Array
        // F Float64Array
        // Q BigUint64Array
        // W Uint16Array
        // b Int8Array
        // d Int32Array
        // f Float32Array
        // h Float16Array
        // q BigInt64Array
        // w Int16Array
        if (!strchr("?BCDFQWbdfhqw", **p))
            return bail(err, "bad typed array");
        if (r_varint(p, pe, &t)) // byteOffset
            goto bad_varint;
        if (r_varint(p, pe, &t)) // byteLength
            goto bad_varint;
        if (r_varint(p, pe, &t)) // flags, only non-zero when backed by RAB
            goto bad_varint;
        break;
    case 'a': // sparse array
        // total element count; ignored because we drop sparse entries
        if (r_varint(p, pe, &t))
            goto bad_varint;
        des_array_begin(arg);
        for (u = s = 0;;) {
            if (*p >= pe)
                goto too_short;
            c = **p;
            if (c == '@')
                break;
            if (c == 'I' && !s) {
                u++, (*p)++;
                if (r_zigzag(p, pe, &i)) // array index, ignored
                    goto bad_varint;
                if (des1(err, p, pe, arg, depth-1))
                    return -1;
            } else {
                if (!s++)
                    des_named_props_begin(arg);
                if (des1(err, p, pe, arg, depth-1)) // key
                    return -1;
                if (des1(err, p, pe, arg, depth-1)) // value
                    return -1;
            }
        }
        (*p)++;
        if (s > 0)
            des_named_props_end(arg);
        if (r_varint(p, pe, &t))
            goto bad_varint;
        if (t != u+s)
            return bail(err, "element count mismatch");
        // total element count; ignored because we drop sparse entries
        if (r_varint(p, pe, &t))
            goto bad_varint;
        des_array_end(arg);
        break;
    case 'D':
        if (des1_num(p, pe, &d))
            goto too_short;
        des_date(arg, d);
        break;
    case 'F': // primitive boolean
    case 'x': // new Boolean(...)
        des_bool(arg, 0);
        break;
    case 'T': // primitive boolean
    case 'y': // new Boolean(...)
        des_bool(arg, 1);
        break;
    case 'I':
        if (r_zigzag(p, pe, &i))
            goto bad_varint;
        des_int(arg, i);
        break;
    case 'N': // primitive number
    case 'n': // new Number(...)
        if (des1_num(p, pe, &d))
            goto too_short;
        des_num(arg, d);
        break;
    case 'Z':
        if (r_varint(p, pe, &u))
            goto bad_varint;
        t = u & 1;
        u = u >> 1;
        if (u & 7)
            return bail(err, "bad bigint");
        // V8's serializer never emits -0n;
        // its deserializer rejects it with DataCloneError
        if (t && !u)
            return bail(err, "negative zero bigint");
        if (pe-*p < (int64_t)u)
            goto too_short;
        des_bigint(arg, *p, u, 1-2*t);
        *p += u;
        break;
    case 'R': // RegExp, deserialized as string
        if (*p >= pe)
            goto too_short;
        switch (**p) {
        default:
            return bail(err, "bad regexp");
        case '"':
        case 'S':
        case 'c':
            break;
        }
        if (des1(err, p, pe, arg, depth-1)) // pattern
            return -1;
        if (r_varint(p, pe, &t)) // flags; ignored
            goto bad_varint;
        break;
    case 's': // string object, decoded as primitive string
        if (*p >= pe)
            goto too_short;
        switch (*(*p)++) {
        case '"':
            goto s_string8;
        case 'S':
            goto s_string;
        case 'c':
            goto s_string16;
        }
        return bail(err, "bad string object");
    case '"': // ascii/latin1
    s_string8:
        if (r_varint(p, pe, &u))
            goto bad_varint;
        if (pe-*p < (int64_t)u)
            goto too_short;
        des_string8(arg, *p, u);
        *p += u;
        break;
    case 'S': // utf8
    s_string:
        if (r_varint(p, pe, &u))
            goto bad_varint;
        if (pe-*p < (int64_t)u)
            goto too_short;
        des_string(arg, (void *)*p, u);
        *p += u;
        break;
    case 'c': // utf16-le
    s_string16:
        if (r_varint(p, pe, &u))
            goto bad_varint;
        if (pe-*p < (int64_t)u)
            goto too_short;
        if (u & 1)
            return bail(err, "bad utf16 string size");
        des_string16(arg, *p, u);
        *p += u;
        break;
    case 'o':
        des_object_begin(arg);
        for (u = 0;; u++) {
            if (pe-*p < 1)
                goto too_short;
            if (**p == '{')
                break;
            if (des1(err, p, pe, arg, depth-1)) // key
                return -1;
            if (des1(err, p, pe, arg, depth-1)) // value
                return -1;
        }
        (*p)++;
        if (r_varint(p, pe, &t))
            goto bad_varint;
        if (t != u)
            return bail(err, "object properties count mismatch");
        des_object_end(arg);
        break;
    case ';': // Map
        des_map_begin(arg);
        for (u = 0; /*empty*/; u++) {
            if (*p >= pe)
                goto too_short;
            if (**p == ':')
                break;
            if (des1(err, p, pe, arg, depth-1)) // key
                return -1;
            if (des1(err, p, pe, arg, depth-1)) // value
                return -1;
        }
        (*p)++;
        if (r_varint(p, pe, &t))
            goto bad_varint;
        if (t != 2*u)
            return bail(err, "map element count mismatch");
        des_map_end(arg);
        break;
    case '\'': // Set
        des_array_begin(arg);
        for (u = 0; /*empty*/; u++) {
            if (*p >= pe)
                goto too_short;
            if (**p == ',')
                break;
            if (des1(err, p, pe, arg, depth-1)) // value
                return -1;
        }
        (*p)++;
        if (r_varint(p, pe, &t))
            goto bad_varint;
        if (t != u)
            return bail(err, "set element count mismatch");
        des_array_end(arg);
        break;
    case 'r':
        // shortest error is /r[.]/ - Error with no message, cause, or stack
        // longest error is /r[EFRSTU]m<string>c<any>s<string>[.]/ where
        // EFRSTU is one of {Eval,Reference,Range,Syntax,Type,URI}Error
        des_error_begin(arg);
        des_object_begin(arg);
        if (*p >= pe)
            goto too_short;
        c = *(*p)++;
        if (!strchr("EFRSTU", c))
            goto r_message;
        if (*p >= pe)
            goto too_short;
        c = *(*p)++;
    r_message:
        if (c != 'm')
            goto r_stack;
        des_string(arg, "message", sizeof("message")-1);
        if (*p >= pe)
            goto too_short;
        if (!strchr("\"Sc", **p))
            return bail(err, "error .message is not a string");
        if (des1(err, p, pe, arg, depth-1))
            return -1;
        if (*p >= pe)
            goto too_short;
        c = *(*p)++;
    r_stack:
        if (c != 's')
            goto r_cause;
        des_string(arg, "stack", sizeof("stack")-1);
        if (*p >= pe)
            goto too_short;
        if (!strchr("\"Sc", **p))
            return bail(err, "error .stack is not a string");
        if (des1(err, p, pe, arg, depth-1))
            return -1;
        if (*p >= pe)
            goto too_short;
        c = *(*p)++;
    r_cause:
        if (c != 'c')
            goto r_end;
        des_string(arg, "cause", sizeof("cause")-1);
        if (des1(err, p, pe, arg, depth-1))
            return -1;
        if (*p >= pe)
            goto too_short;
        c = *(*p)++;
    r_end:
        if (c != '.')
            return bail(err, "bad error object");
        des_object_end(arg);
        des_error_end(arg);
        break;
    }
    return 0;
too_short:
    return bail(err, "input too short");
bad_varint:
    return bail(err, "bad varint");
}

int des(char (*err)[64], const void *b, size_t n, void *arg)
{
    const uint8_t *p, *pe;

    p = b, pe = p + n;
    if (n < 2)
        return bail(err, "input too short");
    if (*p++ != 255)
        return bail(err, "bad header");
    if (*p++ != 15)
        return bail(err, "bad version");
    while (p < pe)
        if (des1(err, &p, pe, arg, /*depth*/96))
            return -1;
    return 0;
}
