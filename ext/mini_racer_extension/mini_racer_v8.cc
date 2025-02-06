#include "v8.h"
#include "v8-profiler.h"
#include "libplatform/libplatform.h"
#include "mini_racer_v8.h"
#include <memory>
#include <vector>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// note: the filter function gets called inside the safe context,
// i.e., the context that has not been tampered with by user JS
// convention: $-prefixed identifiers signify objects from the
// user JS context and should be handled with special care
static const char safe_context_script_source[] = R"js(
;(function($globalThis) {
    const {Map: $Map, Set: $Set} = $globalThis
    const sentinel = {}
    return function filter(v) {
        if (typeof v === "function")
            return sentinel
        if (typeof v !== "object" || v === null)
            return v
        if (v instanceof $Map) {
            const m = new Map()
            for (let [k, t] of Map.prototype.entries.call(v)) {
                t = filter(t)
                if (t !== sentinel)
                    m.set(k, t)
            }
            return m
        } else if (v instanceof $Set) {
            const s = new Set()
            for (let t of Set.prototype.values.call(v)) {
                t = filter(t)
                if (t !== sentinel)
                    s.add(t)
            }
            return s
        } else {
            const o = Array.isArray(v) ? [] : {}
            const pds = Object.getOwnPropertyDescriptors(v)
            for (const [k, d] of Object.entries(pds)) {
                if (!d.enumerable)
                    continue
                let t = d.value
                if (d.get) {
                    // *not* d.get.call(...), may have been tampered with
                    t = Function.prototype.call.call(d.get, v, k)
                }
                t = filter(t)
                if (t !== sentinel)
                    Object.defineProperty(o, k, {value: t, enumerable: true})
            }
            return o
        }
    }
})
)js";

struct Callback
{
    struct State *st;
    int32_t id;
};

// NOTE: do *not* use thread_locals to store state. In single-threaded
// mode, V8 runs on the same thread as Ruby and the Ruby runtime clobbers
// thread-locals when it context-switches threads. Ruby 3.4.0 has a new
// API rb_thread_lock_native_thread() that pins the thread but I don't
// think we're quite ready yet to drop support for older versions, hence
// this inelegant "everything" struct.
struct State
{
    v8::Isolate *isolate;
    // declaring as Local is safe because we take special care
    // to ensure it's rooted in a HandleScope before being used
    v8::Local<v8::Context> context;
    // extra context for when we need access to built-ins like Array
    // and want to be sure they haven't been tampered with by JS code
    v8::Local<v8::Context> safe_context;
    v8::Local<v8::Function> safe_context_function;
    v8::Persistent<v8::Context> persistent_context;         // single-thread mode only
    v8::Persistent<v8::Context> persistent_safe_context;    // single-thread mode only
    v8::Persistent<v8::Function> persistent_safe_context_function;  // single-thread mode only
    Context *ruby_context;
    int64_t max_memory;
    int err_reason;
    bool verbose_exceptions;
    std::vector<Callback*> callbacks;
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    inline ~State();
};

namespace {

// deliberately leaked on program exit,
// not safe to destroy after main() returns
v8::Platform *platform;

struct Serialized
{
    uint8_t *data = nullptr;
    size_t   size = 0;

    Serialized(State& st, v8::Local<v8::Value> v)
    {
        v8::ValueSerializer ser(st.isolate);
        ser.WriteHeader();
        if (!ser.WriteValue(st.context, v).FromMaybe(false)) return; // exception pending
        auto pair = ser.Release();
        data = pair.first;
        size = pair.second;
    }

    ~Serialized()
    {
        free(data);
    }
};

// throws JS exception on serialization error
bool reply(State& st, v8::Local<v8::Value> v)
{
    v8::TryCatch try_catch(st.isolate);
    {
        Serialized serialized(st, v);
        if (serialized.data) {
            v8_reply(st.ruby_context, serialized.data, serialized.size);
            return true;
        }
    }
    if (!try_catch.CanContinue()) {
        try_catch.ReThrow();
        return false;
    }
    auto recv = v8::Undefined(st.isolate);
    if (!st.safe_context_function->Call(st.safe_context, recv, 1, &v).ToLocal(&v)) {
        try_catch.ReThrow();
        return false;
    }
    Serialized serialized(st, v);
    if (serialized.data)
        v8_reply(st.ruby_context, serialized.data, serialized.size);
    return serialized.data != nullptr; // exception pending if false
}

bool reply(State& st, v8::Local<v8::Value> result, v8::Local<v8::Value> err)
{
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::Local<v8::Array> response;
    {
        v8::Context::Scope context_scope(st.safe_context);
        response = v8::Array::New(st.isolate, 2);
    }
    response->Set(st.context, 0, result).Check();
    response->Set(st.context, 1, err).Check();
    if (reply(st, response)) return true;
    if (!try_catch.CanContinue()) { // termination exception?
        try_catch.ReThrow();
        return false;
    }
    v8::String::Utf8Value s(st.isolate, try_catch.Exception());
    const char *message = *s ? *s : "unexpected failure";
    // most serialization errors will be DataCloneErrors but not always
    // DataCloneErrors are not directly detectable so use a heuristic
    if (!strstr(message, "could not be cloned")) {
        try_catch.ReThrow();
        return false;
    }
    // return an {"error": "foo could not be cloned"} object
    v8::Local<v8::Object> error;
    {
        v8::Context::Scope context_scope(st.safe_context);
        error = v8::Object::New(st.isolate);
    }
    auto key = v8::String::NewFromUtf8Literal(st.isolate, "error");
    v8::Local<v8::String> val;
    if (!v8::String::NewFromUtf8(st.isolate, message).ToLocal(&val)) {
        val = v8::String::NewFromUtf8Literal(st.isolate, "unexpected error");
    }
    error->Set(st.context, key, val).Check();
    response->Set(st.context, 0, error).Check();
    if (!reply(st, response)) {
        try_catch.ReThrow();
        return false;
    }
    return true;
}

v8::Local<v8::Value> sanitize(State& st, v8::Local<v8::Value> v)
{
    // punch through proxies
    while (v->IsProxy()) v = v8::Proxy::Cast(*v)->GetTarget();
    // V8's serializer doesn't accept symbols
    if (v->IsSymbol()) return v8::Symbol::Cast(*v)->Description(st.isolate);
    // TODO(bnoordhuis) replace this hack with something more principled
    if (v->IsFunction()) {
        auto type = v8::NewStringType::kNormal;
        const size_t size = sizeof(js_function_marker) / sizeof(*js_function_marker);
        return v8::String::NewFromTwoByte(st.isolate, js_function_marker, type, size).ToLocalChecked();
    }
    if (v->IsWeakMap() || v->IsWeakSet() || v->IsMapIterator() || v->IsSetIterator()) {
        bool is_key_value;
        v8::Local<v8::Array> array;
        if (v8::Object::Cast(*v)->PreviewEntries(&is_key_value).ToLocal(&array)) {
            return array;
        }
    }
    return v;
}

v8::Local<v8::String> to_error(State& st, v8::TryCatch *try_catch, int cause)
{
    v8::Local<v8::Value> t;
    char buf[1024];

    *buf = '\0';
    if (cause == NO_ERROR) {
        // nothing to do
    } else if (cause == PARSE_ERROR) {
        auto message = try_catch->Message();
        v8::String::Utf8Value s(st.isolate, message->Get());
        v8::String::Utf8Value name(st.isolate, message->GetScriptResourceName());
        if (!*s || !*name) goto fallback;
        auto line = message->GetLineNumber(st.context).FromMaybe(0);
        auto column = message->GetStartColumn(st.context).FromMaybe(0);
        snprintf(buf, sizeof(buf), "%c%s at %s:%d:%d", cause, *s, *name, line, column);
    } else if (try_catch->StackTrace(st.context).ToLocal(&t)) {
        v8::String::Utf8Value s(st.isolate, t);
        if (!*s) goto fallback;
        snprintf(buf, sizeof(buf), "%c%s", cause, *s);
    } else {
    fallback:
        v8::String::Utf8Value s(st.isolate, try_catch->Exception());
        const char *message = *s ? *s : "unexpected failure";
        if (cause == MEMORY_ERROR) message = "out of memory";
        if (cause == TERMINATED_ERROR) message = "terminated";
        snprintf(buf, sizeof(buf), "%c%s", cause, message);
    }
    v8::Local<v8::String> s;
    if (v8::String::NewFromUtf8(st.isolate, buf).ToLocal(&s)) return s;
    return v8::String::Empty(st.isolate);
}

extern "C" void v8_global_init(void)
{
    char *p;
    size_t n;

    v8_get_flags(&p, &n);
    if (p) {
        for (char *s = p; s < p+n; s += 1 + strlen(s)) {
            v8::V8::SetFlagsFromString(s);
        }
        free(p);
    }
    v8::V8::InitializeICU();
    if (single_threaded) {
        platform = v8::platform::NewSingleThreadedDefaultPlatform().release();
    } else {
        platform = v8::platform::NewDefaultPlatform().release();
    }
    v8::V8::InitializePlatform(platform);
    v8::V8::Initialize();
}

void v8_gc_callback(v8::Isolate*, v8::GCType, v8::GCCallbackFlags, void *data)
{
    State& st = *static_cast<State*>(data);
    v8::HeapStatistics s;
    st.isolate->GetHeapStatistics(&s);
    int64_t used_heap_size = static_cast<int64_t>(s.used_heap_size());
    if (used_heap_size > st.max_memory) {
        st.err_reason = MEMORY_ERROR;
        st.isolate->TerminateExecution();
    }
}

extern "C" State *v8_thread_init(Context *c, const uint8_t *snapshot_buf,
                                 size_t snapshot_len, int64_t max_memory,
                                 int verbose_exceptions)
{
    State *pst = new State{};
    State& st = *pst;
    st.verbose_exceptions = (verbose_exceptions != 0);
    st.ruby_context = c;
    st.allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::StartupData blob{nullptr, 0};
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = st.allocator.get();
    if (snapshot_len) {
        blob.data = reinterpret_cast<const char*>(snapshot_buf);
        blob.raw_size = snapshot_len;
        params.snapshot_blob = &blob;
    }
    st.isolate = v8::Isolate::New(params);
    st.max_memory = max_memory;
    if (st.max_memory > 0)
        st.isolate->AddGCEpilogueCallback(v8_gc_callback, pst);
    {
        v8::Locker locker(st.isolate);
        v8::Isolate::Scope isolate_scope(st.isolate);
        v8::HandleScope handle_scope(st.isolate);
        st.safe_context = v8::Context::New(st.isolate);
        st.context = v8::Context::New(st.isolate);
        v8::Context::Scope context_scope(st.context);
        {
            v8::Context::Scope context_scope(st.safe_context);
            auto source = v8::String::NewFromUtf8Literal(st.isolate, safe_context_script_source);
            auto filename = v8::String::NewFromUtf8Literal(st.isolate, "safe_context_script.js");
            v8::ScriptOrigin origin(filename);
            auto script =
                v8::Script::Compile(st.safe_context, source, &origin)
                .ToLocalChecked();
            auto function_v = script->Run(st.safe_context).ToLocalChecked();
            auto function = v8::Function::Cast(*function_v);
            auto recv = v8::Undefined(st.isolate);
            v8::Local<v8::Value> arg = st.context->Global();
            // grant the safe context access to the user context's globalThis
            st.safe_context->SetSecurityToken(st.context->GetSecurityToken());
            function_v =
                function->Call(st.safe_context, recv, 1, &arg)
                .ToLocalChecked();
            // revoke access again now that the script did its one-time setup
            st.safe_context->UseDefaultSecurityToken();
            st.safe_context_function = v8::Local<v8::Function>::Cast(function_v);
        }
        if (single_threaded) {
            st.persistent_safe_context_function.Reset(st.isolate, st.safe_context_function);
            st.persistent_safe_context.Reset(st.isolate, st.safe_context);
            st.persistent_context.Reset(st.isolate, st.context);
            return pst; // intentionally returning early and keeping alive
        }
        v8_thread_main(c, pst);
    }
    delete pst;
    return nullptr;
}

void v8_api_callback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto ext = v8::External::Cast(*info.Data());
    Callback *cb = static_cast<Callback*>(ext->Value());
    State& st = *cb->st;
    v8::Local<v8::Array> request;
    {
        v8::Context::Scope context_scope(st.safe_context);
        request = v8::Array::New(st.isolate, 1 + info.Length());
    }
    for (int i = 0, n = info.Length(); i < n; i++) {
        request->Set(st.context, i, sanitize(st, info[i])).Check();
    }
    auto id = v8::Int32::New(st.isolate, cb->id);
    request->Set(st.context, info.Length(), id).Check(); // callback id
    {
        Serialized serialized(st, request);
        if (!serialized.data) return; // exception pending
        uint8_t marker = 'c'; // callback marker
        v8_reply(st.ruby_context, &marker, 1);
        v8_reply(st.ruby_context, serialized.data, serialized.size);
    }
    const uint8_t *p;
    size_t n;
    for (;;) {
        v8_roundtrip(st.ruby_context, &p, &n);
        if (*p == 'c') // callback reply
            break;
        if (*p == 'e') // ruby exception pending
            return st.isolate->TerminateExecution();
        v8_dispatch(st.ruby_context);
    }
    v8::ValueDeserializer des(st.isolate, p+1, n-1);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    if (!des.ReadValue(st.context).ToLocal(&result)) return; // exception pending
    info.GetReturnValue().Set(result);
}

// response is err or empty string
extern "C" void v8_attach(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request; // [name, id]
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> name_v;
        if (!request->Get(st.context, 0).ToLocal(&name_v)) goto fail;
        v8::Local<v8::Value> id_v;
        if (!request->Get(st.context, 1).ToLocal(&id_v)) goto fail;
        v8::Local<v8::String> name;
        if (!name_v->ToString(st.context).ToLocal(&name)) goto fail;
        int32_t id;
        if (!id_v->Int32Value(st.context).To(&id)) goto fail;
        Callback *cb = new Callback{pst, id};
        st.callbacks.push_back(cb);
        v8::Local<v8::External> ext = v8::External::New(st.isolate, cb);
        v8::Local<v8::Function> function;
        if (!v8::Function::New(st.context, v8_api_callback, ext).ToLocal(&function)) goto fail;
        // support foo.bar.baz paths
        v8::String::Utf8Value path(st.isolate, name);
        if (!*path) goto fail;
        v8::Local<v8::Object> obj = st.context->Global();
        v8::Local<v8::String> key;
        for (const char *p = *path;;) {
            size_t n = strcspn(p, ".");
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromUtf8(st.isolate, p, type, n).ToLocal(&key)) goto fail;
            if (p[n] == '\0') break;
            p += n + 1;
            v8::Local<v8::Value> val;
            if (!obj->Get(st.context, key).ToLocal(&val)) goto fail;
            if (!val->IsObject() && !val->IsFunction()) {
                val = v8::Object::New(st.isolate);
                if (!obj->Set(st.context, key, val).FromMaybe(false)) goto fail;
            }
            obj = val.As<v8::Object>();
        }
        if (!obj->Set(st.context, key, function).FromMaybe(false)) goto fail;
    }
    cause = NO_ERROR;
fail:
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, err)) abort();
}

// response is errback [result, err] array
extern "C" void v8_call(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    std::vector<v8::Local<v8::Value>> args;
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request;
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> name_v;
        if (!request->Get(st.context, 0).ToLocal(&name_v)) goto fail;
        v8::Local<v8::String> name;
        if (!name_v->ToString(st.context).ToLocal(&name)) goto fail;
        cause = RUNTIME_ERROR;
        // support foo.bar.baz paths
        v8::String::Utf8Value path(st.isolate, name);
        if (!*path) goto fail;
        v8::Local<v8::Object> obj = st.context->Global();
        v8::Local<v8::String> key;
        for (const char *p = *path;;) {
            size_t n = strcspn(p, ".");
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromUtf8(st.isolate, p, type, n).ToLocal(&key)) goto fail;
            if (p[n] == '\0') break;
            p += n + 1;
            v8::Local<v8::Value> val;
            if (!obj->Get(st.context, key).ToLocal(&val)) goto fail;
            if (!val->ToObject(st.context).ToLocal(&obj)) goto fail;
        }
        v8::Local<v8::Value> function_v;
        if (!obj->Get(st.context, key).ToLocal(&function_v)) goto fail;
        if (!function_v->IsFunction()) {
            // XXX it's technically possible for |function_v| to be a callable
            // object but those are effectively extinct; regexp objects used
            // to be callable but not anymore
            auto message = v8::String::NewFromUtf8Literal(st.isolate, "not a function");
            auto exception = v8::Exception::TypeError(message);
            st.isolate->ThrowException(exception);
            goto fail;
        }
        auto function = v8::Function::Cast(*function_v);
        assert(request->IsArray());
        int n = v8::Array::Cast(*request)->Length();
        for (int i = 1; i < n; i++) {
            v8::Local<v8::Value> val;
            if (!request->Get(st.context, i).ToLocal(&val)) goto fail;
            args.push_back(val);
        }
        auto maybe_result_v = function->Call(st.context, obj, args.size(), args.data());
        v8::Local<v8::Value> result_v;
        if (!maybe_result_v.ToLocal(&result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

// response is errback [result, err] array
extern "C" void v8_eval(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request; // [filename, source]
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> filename;
        if (!request->Get(st.context, 0).ToLocal(&filename)) goto fail;
        v8::Local<v8::Value> source_v;
        if (!request->Get(st.context, 1).ToLocal(&source_v)) goto fail;
        v8::Local<v8::String> source;
        if (!source_v->ToString(st.context).ToLocal(&source)) goto fail;
        v8::ScriptOrigin origin(filename);
        v8::Local<v8::Script> script;
        cause = PARSE_ERROR;
        if (!v8::Script::Compile(st.context, source, &origin).ToLocal(&script)) goto fail;
        v8::Local<v8::Value> result_v;
        cause = RUNTIME_ERROR;
        auto maybe_result_v = script->Run(st.context);
        if (!maybe_result_v.ToLocal(&result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

extern "C" void v8_heap_stats(State *pst)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    v8::HeapStatistics s;
    st.isolate->GetHeapStatistics(&s);
    v8::Local<v8::Object> response = v8::Object::New(st.isolate);
#define PROP(name)                                                      \
    do {                                                                \
        auto key = v8::String::NewFromUtf8Literal(st.isolate, #name);   \
        auto val = v8::Number::New(st.isolate, s.name());               \
        response->Set(st.context, key, val).Check();                    \
    } while (0)
    PROP(total_heap_size);
    PROP(total_heap_size);
    PROP(total_heap_size_executable);
    PROP(total_physical_size);
    PROP(total_available_size);
    PROP(total_global_handles_size);
    PROP(used_global_handles_size);
    PROP(used_heap_size);
    PROP(heap_size_limit);
    PROP(malloced_memory);
    PROP(external_memory);
    PROP(peak_malloced_memory);
    PROP(number_of_native_contexts);
    PROP(number_of_detached_contexts);
#undef PROP
    if (!reply(st, response)) abort();
}

struct OutputStream : public v8::OutputStream
{
    std::vector<uint8_t> buf;

    void EndOfStream() final {}
    int GetChunkSize() final { return 65536; }

    WriteResult WriteAsciiChunk(char* data, int size)
    {
        const uint8_t *p = reinterpret_cast<uint8_t*>(data);
        buf.insert(buf.end(), p, p+size);
        return WriteResult::kContinue;
    }
};

extern "C" void v8_heap_snapshot(State *pst)
{
    State& st = *pst;
    v8::HandleScope handle_scope(st.isolate);
    auto snapshot = st.isolate->GetHeapProfiler()->TakeHeapSnapshot();
    OutputStream os;
    snapshot->Serialize(&os, v8::HeapSnapshot::kJSON);
    v8_reply(st.ruby_context, os.buf.data(), os.buf.size()); // not serialized because big
}

extern "C" void v8_pump_message_loop(State *pst)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    bool ran_task = v8::platform::PumpMessageLoop(platform, st.isolate);
    if (st.isolate->IsExecutionTerminating()) goto fail;
    if (try_catch.HasCaught()) goto fail;
    if (ran_task) v8::MicrotasksScope::PerformCheckpoint(st.isolate);
    if (st.isolate->IsExecutionTerminating()) goto fail;
    if (platform->IdleTasksEnabled(st.isolate)) {
        double idle_time_in_seconds = 1.0 / 50;
        v8::platform::RunIdleTasks(platform, st.isolate, idle_time_in_seconds);
        if (st.isolate->IsExecutionTerminating()) goto fail;
        if (try_catch.HasCaught()) goto fail;
    }
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        st.err_reason = NO_ERROR;
    }
    auto result = v8::Boolean::New(st.isolate, ran_task);
    if (!reply(st, result)) abort();
}

int snapshot(bool is_warmup, bool verbose_exceptions,
             const v8::String::Utf8Value& code,
             v8::StartupData blob, v8::StartupData *result,
             char (*errbuf)[512])
{
    // SnapshotCreator takes ownership of isolate
    v8::Isolate *isolate = v8::Isolate::Allocate();
    v8::StartupData *existing_blob = is_warmup ? &blob : nullptr;
    v8::SnapshotCreator snapshot_creator(isolate, nullptr, existing_blob);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(verbose_exceptions);
    auto filename = is_warmup
        ? v8::String::NewFromUtf8Literal(isolate, "<warmup>")
        : v8::String::NewFromUtf8Literal(isolate, "<snapshot>");
    auto mode = is_warmup
        ? v8::SnapshotCreator::FunctionCodeHandling::kKeep
        : v8::SnapshotCreator::FunctionCodeHandling::kClear;
    int cause = INTERNAL_ERROR;
    {
        auto context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);
        v8::Local<v8::String> source;
        auto type = v8::NewStringType::kNormal;
        if (!v8::String::NewFromUtf8(isolate, *code, type, code.length()).ToLocal(&source)) {
            v8::String::Utf8Value s(isolate, try_catch.Exception());
            if (*s) snprintf(*errbuf, sizeof(*errbuf), "%c%s", cause, *s);
            goto fail;
        }
        v8::ScriptOrigin origin(filename);
        v8::Local<v8::Script> script;
        cause = PARSE_ERROR;
        if (!v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
            goto err;
        }
        cause = RUNTIME_ERROR;
        if (script->Run(context).IsEmpty()) {
        err:
            auto m = try_catch.Message();
            v8::String::Utf8Value s(isolate, m->Get());
            v8::String::Utf8Value name(isolate, m->GetScriptResourceName());
            auto line = m->GetLineNumber(context).FromMaybe(0);
            auto column = m->GetStartColumn(context).FromMaybe(0);
            snprintf(*errbuf, sizeof(*errbuf), "%c%s\n%s:%d:%d",
                     cause, *s, *name, line, column);
            goto fail;
        }
        cause = INTERNAL_ERROR;
        if (!is_warmup) snapshot_creator.SetDefaultContext(context);
    }
    if (is_warmup) {
        isolate->ContextDisposedNotification(false);
        auto context = v8::Context::New(isolate);
        snapshot_creator.SetDefaultContext(context);
    }
    *result = snapshot_creator.CreateBlob(mode);
    cause = NO_ERROR;
fail:
    return cause;
}

// response is errback [result, err] array
// note: currently needs --stress_snapshot in V8 debug builds
// to work around a buggy check in the snapshot deserializer
extern "C" void v8_snapshot(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    v8::StartupData blob{nullptr, 0};
    int cause = INTERNAL_ERROR;
    char errbuf[512] = {0};
    {
        v8::Local<v8::Value> code_v;
        if (!des.ReadValue(st.context).ToLocal(&code_v)) goto fail;
        v8::String::Utf8Value code(st.isolate, code_v);
        if (!*code) goto fail;
        v8::StartupData init{nullptr, 0};
        cause = snapshot(/*is_warmup*/false, st.verbose_exceptions, code, init, &blob, &errbuf);
        if (cause) goto fail;
    }
    if (blob.data) {
        auto data = reinterpret_cast<const uint8_t*>(blob.data);
        auto type = v8::NewStringType::kNormal;
        bool ok = v8::String::NewFromOneByte(st.isolate, data, type,
                                             blob.raw_size).ToLocal(&result);
        delete[] blob.data;
        blob = v8::StartupData{nullptr, 0};
        if (!ok) goto fail;
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    v8::Local<v8::Value> err;
    if (*errbuf) {
        if (!v8::String::NewFromUtf8(st.isolate, errbuf).ToLocal(&err)) {
            err = v8::String::NewFromUtf8Literal(st.isolate, "unexpected error");
        }
    } else {
        err = to_error(st, &try_catch, cause);
    }
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

extern "C" void v8_warmup(State *pst, const uint8_t *p, size_t n)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    std::vector<uint8_t> storage;
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    v8::StartupData blob{nullptr, 0};
    int cause = INTERNAL_ERROR;
    char errbuf[512] = {0};
    {
        v8::Local<v8::Value> request_v;
        if (!des.ReadValue(st.context).ToLocal(&request_v)) goto fail;
        v8::Local<v8::Object> request; // [snapshot, warmup_code]
        if (!request_v->ToObject(st.context).ToLocal(&request)) goto fail;
        v8::Local<v8::Value> blob_data_v;
        if (!request->Get(st.context, 0).ToLocal(&blob_data_v)) goto fail;
        v8::Local<v8::String> blob_data;
        if (!blob_data_v->ToString(st.context).ToLocal(&blob_data)) goto fail;
        assert(blob_data->IsOneByte());
        assert(blob_data->ContainsOnlyOneByte());
        if (const size_t len = blob_data->Length()) {
            auto flags = v8::String::NO_NULL_TERMINATION
                       | v8::String::PRESERVE_ONE_BYTE_NULL;
            storage.resize(len);
            blob_data->WriteOneByte(st.isolate, storage.data(), 0, len, flags);
        }
        v8::Local<v8::Value> code_v;
        if (!request->Get(st.context, 1).ToLocal(&code_v)) goto fail;
        v8::String::Utf8Value code(st.isolate, code_v);
        if (!*code) goto fail;
        auto data = reinterpret_cast<const char*>(storage.data());
        auto size = static_cast<int>(storage.size());
        v8::StartupData init{data, size};
        cause = snapshot(/*is_warmup*/true, st.verbose_exceptions, code, init, &blob, &errbuf);
        if (cause) goto fail;
    }
    if (blob.data) {
        auto data = reinterpret_cast<const uint8_t*>(blob.data);
        auto type = v8::NewStringType::kNormal;
        bool ok = v8::String::NewFromOneByte(st.isolate, data, type,
                                             blob.raw_size).ToLocal(&result);
        delete[] blob.data;
        blob = v8::StartupData{nullptr, 0};
        if (!ok) goto fail;
    }
    cause = NO_ERROR;
fail:
    if (st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    v8::Local<v8::Value> err;
    if (*errbuf) {
        if (!v8::String::NewFromUtf8(st.isolate, errbuf).ToLocal(&err)) {
            err = v8::String::NewFromUtf8Literal(st.isolate, "unexpected error");
        }
    } else {
        err = to_error(st, &try_catch, cause);
    }
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
}

extern "C" void v8_low_memory_notification(State *pst)
{
    pst->isolate->LowMemoryNotification();
}

// called from ruby thread
extern "C" void v8_terminate_execution(State *pst)
{
    pst->isolate->TerminateExecution();
}

extern "C" void v8_single_threaded_enter(State *pst, Context *c, void (*f)(Context *c))
{
    State& st = *pst;
    v8::Locker locker(st.isolate);
    v8::Isolate::Scope isolate_scope(st.isolate);
    v8::HandleScope handle_scope(st.isolate);
    {
        st.safe_context_function = v8::Local<v8::Function>::New(st.isolate, st.persistent_safe_context_function);
        st.safe_context = v8::Local<v8::Context>::New(st.isolate, st.persistent_safe_context);
        st.context = v8::Local<v8::Context>::New(st.isolate, st.persistent_context);
        v8::Context::Scope context_scope(st.context);
        f(c);
        st.context = v8::Local<v8::Context>();
        st.safe_context = v8::Local<v8::Context>();
        st.safe_context_function = v8::Local<v8::Function>();
    }
}

extern "C" void v8_single_threaded_dispose(struct State *pst)
{
    delete pst; // see State::~State() below
}

} // namespace anonymous

State::~State()
{
    {
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolate_scope(isolate);
        persistent_safe_context.Reset();
        persistent_context.Reset();
    }
    isolate->Dispose();
    for (Callback *cb : callbacks)
        delete cb;
}
