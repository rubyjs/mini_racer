#include <ruby.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

// Load a Ruby extension like Ruby does, only with flags that:
// a) hide symbols from other extensions (RTLD_LOCAL)
// b) bind symbols tightly (RTLD_DEEPBIND, when available)

void Init_mini_racer_loader(void);

static void *_dln_load(const char *file);

static VALUE _load_shared_lib(VALUE self, volatile VALUE fname)
{
    (void) self;

    // check that path is not tainted
    SafeStringValue(fname);

    FilePathValue(fname);
    VALUE path = rb_str_encode_ospath(fname);

    char *loc = StringValueCStr(path);
    void *handle = _dln_load(loc);

    return handle ? Qtrue : Qfalse;
}

// adapted from Ruby's dln.c
#define INIT_FUNC_PREFIX ((char[]) {'I', 'n', 'i', 't', '_'})
#define INIT_FUNCNAME(buf, file) do { \
    const char *base = (file); \
    const size_t flen = _init_funcname(&base); \
    const size_t plen = sizeof(INIT_FUNC_PREFIX); \
    char *const tmp = ALLOCA_N(char, plen + flen + 1); \
    memcpy(tmp, INIT_FUNC_PREFIX, plen); \
    memcpy(tmp+plen, base, flen); \
    tmp[plen+flen] = '\0'; \
    *(buf) = tmp; \
} while(0)

// adapted from Ruby's dln.c
static size_t _init_funcname(const char **file)
{
    const char *p = *file,
               *base,
               *dot = NULL;

    for (base = p; *p; p++) { /* Find position of last '/' */
        if (*p == '.' && !dot) {
            dot = p;
        }
        if (*p == '/') {
            base = p + 1;
            dot = NULL;
        }
    }
    *file = base;
    return (uintptr_t) ((dot ? dot : p) - base);
}

// adapted from Ruby's dln.c
static void *_dln_load(const char *file)
{
    char *buf;
    const char *error;
#define DLN_ERROR() (error = dlerror(), strcpy(ALLOCA_N(char, strlen(error) + 1), error))

    void *handle;
    void (*init_fct)(void);

    INIT_FUNCNAME(&buf, file);

#ifndef RTLD_DEEPBIND
# define RTLD_DEEPBIND 0
#endif
    /* Load file */
    if ((handle = dlopen(file, RTLD_LAZY|RTLD_LOCAL|RTLD_DEEPBIND)) == NULL) {
        DLN_ERROR();
        goto failed;
    }
#if defined(RUBY_EXPORT)
    {
        static const char incompatible[] = "incompatible library version";
        void *ex = dlsym(handle, "ruby_xmalloc");
        if (ex && ex != (void *) &ruby_xmalloc) {

# if defined __APPLE__
            /* dlclose() segfaults */
            rb_fatal("%s - %s", incompatible, file);
# else
            dlclose(handle);
            error = incompatible;
            goto failed;
#endif
        }
    }
# endif

    init_fct = (void (*)(void)) dlsym(handle, buf);
    if (init_fct == NULL) {
        error = DLN_ERROR();
        dlclose(handle);
        goto failed;
    }

    /* Call the init code */
    (*init_fct)();

    return handle;

failed:
    rb_raise(rb_eLoadError, "%s", error);
}

__attribute__((visibility("default"))) void Init_mini_racer_loader()
{
    VALUE mMiniRacer = rb_define_module("MiniRacer");
    VALUE mLoader = rb_define_module_under(mMiniRacer, "Loader");
    rb_define_singleton_method(mLoader, "load", _load_shared_lib, 1);
}
