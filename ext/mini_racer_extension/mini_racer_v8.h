#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    NO_ERROR         = '\0',
    INTERNAL_ERROR   = 'I',
    MEMORY_ERROR     = 'M',
    PARSE_ERROR      = 'P',
    RUNTIME_ERROR    = 'R',
    TERMINATED_ERROR = 'T',
};

static const uint16_t js_function_marker[] = {0xBFF,'J','a','v','a','S','c','r','i','p','t','F','u','n','c','t','i','o','n'};

// defined in mini_racer_extension.c, opaque to mini_racer_v8.cc
struct Context;

// defined in mini_racer_v8.cc, opaque to mini_racer_extension.c
struct State;

// defined in mini_racer_extension.c
extern int single_threaded;
void v8_get_flags(char **p, size_t *n);
void v8_thread_main(struct Context *c, struct State *pst);
void v8_dispatch(struct Context *c);
void v8_reply(struct Context *c, const uint8_t *p, size_t n);
void v8_roundtrip(struct Context *c, const uint8_t **p, size_t *n);

// defined in mini_racer_v8.cc
void v8_global_init(void);
struct State *v8_thread_init(struct Context *c, const uint8_t *snapshot_buf,
                             size_t snapshot_len, int64_t max_memory,
                             int verbose_exceptions); // calls v8_thread_main
void v8_attach(struct State *pst, const uint8_t *p, size_t n);
void v8_call(struct State *pst, const uint8_t *p, size_t n);
void v8_eval(struct State *pst, const uint8_t *p, size_t n);
void v8_heap_stats(struct State *pst);
void v8_heap_snapshot(struct State *pst);
void v8_pump_message_loop(struct State *pst);
void v8_snapshot(struct State *pst, const uint8_t *p, size_t n);
void v8_warmup(struct State *pst, const uint8_t *p, size_t n);
void v8_low_memory_notification(struct State *pst);
void v8_terminate_execution(struct State *pst); // called from ruby or watchdog thread
void v8_single_threaded_enter(struct State *pst, struct Context *c, void (*f)(struct Context *c));
void v8_single_threaded_dispose(struct State *pst);

#ifdef __cplusplus
}
#endif
