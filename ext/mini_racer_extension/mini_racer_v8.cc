#include "v8.h"
#include "v8-profiler.h"
#include "libplatform/libplatform.h"
#include "mini_racer_v8.h"
#include <atomic>
#include <memory>
#include <vector>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

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

enum : unsigned
{
    NON_WATCHDOG_TERMINATION = 1,
    WATCHDOG_TERMINATION = 2,
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
    v8::Persistent<v8::Value> ruby_exception;
    Context *ruby_context;
    int64_t max_memory;
    int err_reason;
    // TerminateExecution() while idle doesn't make IsExecutionTerminating() true
    std::atomic<unsigned> terminate_requested;
    // Armed before checking terminate_requested so a concurrent terminator
    // either gets observed or posts a task. Uses sequential consistency.
    std::atomic<bool> waiting_for_task;
    // Tracks reentrant call/eval dispatches so nested await calls can fail
    // instead of deadlocking and nested pumps preserve outer termination.
    int javascript_call_depth;
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

void append_bytes(std::vector<char>& out, const char *p, size_t n)
{
    out.insert(out.end(), p, p + n);
}

void append_literal(std::vector<char>& out, const char *s)
{
    append_bytes(out, s, strlen(s));
}

bool append_utf8(std::vector<char>& out, const v8::String::Utf8Value& s)
{
    if (!*s) return false;
    append_bytes(out, *s, s.length());
    return true;
}

void append_format(std::vector<char>& out, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (static_cast<size_t>(n) < sizeof(buf)) {
        append_bytes(out, buf, n);
        return;
    }

    std::vector<char> tmp(static_cast<size_t>(n) + 1);
    va_start(ap, fmt);
    vsnprintf(tmp.data(), tmp.size(), fmt, ap);
    va_end(ap);
    append_bytes(out, tmp.data(), n);
}

v8::Local<v8::String> string_from_bytes(v8::Isolate *isolate, const std::vector<char>& bytes)
{
    v8::Local<v8::String> s;
    auto type = v8::NewStringType::kNormal;
    if (bytes.size() <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
        v8::String::NewFromUtf8(isolate, bytes.data(), type,
                                static_cast<int>(bytes.size())).ToLocal(&s)) {
        return s;
    }

    char fallback[] = {
        bytes.empty() ? static_cast<char>(INTERNAL_ERROR) : bytes[0],
        'u', 'n', 'e', 'x', 'p', 'e', 'c', 't', 'e', 'd', ' ', 'e', 'r', 'r', 'o', 'r'
    };
    if (v8::String::NewFromUtf8(isolate, fallback, type, static_cast<int>(sizeof(fallback))).ToLocal(&s)) return s;
    return v8::String::Empty(isolate);
}

void set_error_message(std::vector<char>& out, int cause, const char *message)
{
    out.clear();
    out.push_back(static_cast<char>(cause));
    append_literal(out, message);
}

bool set_error_message(std::vector<char>& out, int cause, const v8::String::Utf8Value& message)
{
    out.clear();
    out.push_back(static_cast<char>(cause));
    return append_utf8(out, message);
}

void set_fallback_error(State& st, v8::TryCatch *try_catch, int cause,
                        std::vector<char>& out)
{
    v8::String::Utf8Value s(st.isolate, try_catch->Exception());
    const char *message = *s ? nullptr : "unexpected failure";
    if (cause == MEMORY_ERROR) message = "out of memory";
    if (cause == TERMINATED_ERROR) message = "terminated";
    if (message) {
        set_error_message(out, cause, message);
    } else if (!set_error_message(out, cause, s)) {
        set_error_message(out, cause, "unexpected failure");
    }
}

bool read_path_key(State& st, const char *&p, const char *pe,
                   v8::Local<v8::String> *key, bool *last)
{
    const char *dot;
    size_t n;

    if (p > pe) return false;
    dot = static_cast<const char*>(memchr(p, '.', pe - p));
    *last = (dot == nullptr);
    n = *last ? static_cast<size_t>(pe - p) : static_cast<size_t>(dot - p);
    if (n > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    auto type = v8::NewStringType::kNormal;
    if (!v8::String::NewFromUtf8(st.isolate, p, type, static_cast<int>(n)).ToLocal(key)) return false;
    p = *last ? pe : dot + 1;
    return true;
}

bool bubble_up_ruby_exception(State& st, v8::TryCatch *try_catch)
{
    auto exception = try_catch->Exception();
    if (exception.IsEmpty()) return false;
    auto ruby_exception = v8::Local<v8::Value>::New(st.isolate, st.ruby_exception);
    if (ruby_exception.IsEmpty()) return false;
    if (!ruby_exception->SameValue(exception)) return false;
    // signal that the ruby thread should reraise the exception
    // that it caught earlier when executing a js->ruby callback
    uint8_t c = 'e';
    v8_reply(st.ruby_context, &c, 1);
    return true;
}

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
    else
        try_catch.ReThrow();
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

// for when a reply is not expected to fail because of serialization
// errors but can still fail when preempted by isolate termination;
// temporarily cancels the termination exception so it can send the reply
void reply_retry(State& st, v8::Local<v8::Value> response)
{
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    bool ok = reply(st, response);
    while (!ok) {
        assert(try_catch.HasCaught());
        assert(try_catch.HasTerminated());
        if (!try_catch.HasTerminated()) abort();
        st.isolate->CancelTerminateExecution();
        ok = reply(st, response);
        st.isolate->TerminateExecution();
    }
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

v8::Local<v8::Value> to_error(State& st, v8::TryCatch *try_catch, int cause)
{
    std::vector<char> buf;

    if (cause == NO_ERROR) {
        return v8::String::Empty(st.isolate);
    } else if (cause == PARSE_ERROR) {
        auto message = try_catch->Message();
        v8::String::Utf8Value s(st.isolate, message->Get());
        v8::String::Utf8Value name(st.isolate, message->GetScriptResourceName());
        if (*s && *name) {
            buf.push_back(static_cast<char>(cause));
            append_utf8(buf, s);
            append_literal(buf, " at ");
            append_utf8(buf, name);
            auto line = message->GetLineNumber(st.context).FromMaybe(0);
            auto column = message->GetStartColumn(st.context).FromMaybe(0);
            append_format(buf, ":%d:%d", line, column);
        } else {
            set_fallback_error(st, try_catch, cause, buf);
        }
    } else {
        v8::Local<v8::Value> t;
        if (try_catch->StackTrace(st.context).ToLocal(&t)) {
            v8::String::Utf8Value s(st.isolate, t);
            if (*s) {
                if (cause == RUNTIME_ERROR) {
                    auto exception_message = try_catch->Message();
                    auto captured = exception_message.IsEmpty()
                        ? v8::String::Empty(st.isolate) : exception_message->Get();
                    v8::String::Utf8Value message(st.isolate, captured);
                    // Use V8's captured text without calling exception.toString().
                    const char *text = *message;
                    size_t length = text ? message.length() : 0;
                    static const char prefix[] = "Uncaught ";
                    if (length >= sizeof(prefix)-1 &&
                        !memcmp(text, prefix, sizeof(prefix)-1)) {
                        text += sizeof(prefix)-1;
                        length -= sizeof(prefix)-1;
                    }
                    size_t slen = s.length();
                    bool matched = !exception_message.IsEmpty() && text &&
                        slen >= length && !memcmp(*s, text, length) &&
                        (slen == length || (*s)[length] == '\n');
                    if (!matched) {
                        // Custom stack headers may differ from V8's captured
                        // text. Retain their legacy first-line formatting.
                        const char *nl = static_cast<const char*>(memchr(*s, '\n', slen));
                        text = *s;
                        length = nl ? static_cast<size_t>(nl - *s) : slen;
                    }
                    buf.push_back(static_cast<char>(cause));
                    append_bytes(buf, text, length);
                    auto error = string_from_bytes(st.isolate, buf);
                    // A matching header can contain any number of message
                    // newlines; only the remaining stack belongs in backtrace.
                    size_t offset = length < slen ? length + 1 : slen;
                    buf.clear();
                    append_bytes(buf, *s + offset, slen - offset);
                    auto stack = string_from_bytes(st.isolate, buf);
                    v8::Local<v8::Value> fields[] = {error, stack};
                    v8::Context::Scope context_scope(st.safe_context);
                    return v8::Array::New(st.isolate, fields, 2);
                }
                buf.push_back(static_cast<char>(cause));
                append_utf8(buf, s);
            } else {
                set_fallback_error(st, try_catch, cause, buf);
            }
        } else {
            set_fallback_error(st, try_catch, cause, buf);
        }
    }
    return string_from_bytes(st.isolate, buf);
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
        st.terminate_requested.fetch_or(NON_WATCHDOG_TERMINATION);
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
        if (*p == 'e') { // ruby exception pending
            v8::Local<v8::String> message;
            auto type = v8::NewStringType::kNormal;
            if (!v8::String::NewFromOneByte(st.isolate, p+1, type, n-1).ToLocal(&message)) {
                message = v8::String::NewFromUtf8Literal(st.isolate, "Ruby exception");
            }
            auto exception = v8::Exception::Error(message);
            st.ruby_exception.Reset(st.isolate, exception);
            st.isolate->ThrowException(exception);
            return;
        }
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
        if (!id_v->IsInt32()) goto fail;
        id = id_v.As<v8::Int32>()->Value();
        // support foo.bar.baz paths
        v8::String::Utf8Value path(st.isolate, name);
        if (!*path) goto fail;
        const char *p = *path;
        const char *pe = p + path.length();
        v8::Local<v8::Object> obj = st.context->Global();
        v8::Local<v8::String> key;
        for (;;) {
            bool last;
            if (!read_path_key(st, p, pe, &key, &last)) goto fail;
            if (last) break;
            v8::Local<v8::Value> val;
            if (!obj->Get(st.context, key).ToLocal(&val)) goto fail;
            if (!val->IsObject() && !val->IsFunction()) {
                val = v8::Object::New(st.isolate);
                if (!obj->Set(st.context, key, val).FromMaybe(false)) goto fail;
            }
            obj = val.As<v8::Object>();
        }
        std::unique_ptr<Callback> cb(new Callback{pst, id});
        v8::Local<v8::External> ext = v8::External::New(st.isolate, cb.get());
        v8::Local<v8::Function> function;
        if (!v8::Function::New(st.context, v8_api_callback, ext).ToLocal(&function)) goto fail;
        st.callbacks.push_back(cb.release());
        if (!obj->Set(st.context, key, function).FromMaybe(false)) goto fail;
    }
    cause = NO_ERROR;
fail:
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    auto err = to_error(st, &try_catch, cause);
    reply_retry(st, err);
}

struct JavascriptCallScope
{
    int& depth;

    explicit JavascriptCallScope(int& depth) : depth(depth) { depth++; }
    ~JavascriptCallScope() { depth--; }
};

void throw_nested_await_call(State& st)
{
    // V8 does not run microtask checkpoints recursively. A nested await call
    // from an attached Ruby callback can therefore deadlock, so reject it
    // before entering JavaScript.
    auto message = v8::String::NewFromUtf8Literal(
        st.isolate, "nested call_await/eval_await is not supported");
    st.isolate->ThrowException(v8::Exception::Error(message));
}

// awaits |*result| if it's a promise; false means an exception is pending
bool await_promise(State& st, v8::Local<v8::Value> *result)
{
    if (!(*result)->IsPromise()) return true;
    auto promise = result->As<v8::Promise>();
    for (;;) {
        v8::MicrotasksScope::PerformCheckpoint(st.isolate);
        switch (promise->State()) {
        case v8::Promise::kFulfilled:
            *result = promise->Result();
            return true;
        case v8::Promise::kRejected:
            st.isolate->ThrowException(promise->Result());
            return false;
        case v8::Promise::kPending:
            break;
        }
        st.waiting_for_task.store(true);
        if (st.terminate_requested.load() || st.isolate->IsExecutionTerminating()) {
            st.waiting_for_task.store(false);
            return false;
        }
        // blocks until the next task; a concurrent v8_terminate_execution
        // observes waiting_for_task and posts one to end the wait
        v8::platform::PumpMessageLoop(
            platform, st.isolate,
            v8::platform::MessageLoopBehavior::kWaitForWork);
        st.waiting_for_task.store(false);
    }
}

// Nested calls report termination but leave it pending for the enclosing call.
// Outermost calls consume it as before.
bool suspend_termination(State& st, bool nested, int& cause)
{
    unsigned requests = nested ? st.terminate_requested.load()
                               : st.terminate_requested.exchange(0);
    if (!requests && !st.isolate->IsExecutionTerminating()) return false;
    st.isolate->CancelTerminateExecution();
    cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
    if (!nested) st.err_reason = NO_ERROR;
    return nested;
}

void restore_termination(State& st, bool suspended)
{
    if (suspended)
        st.isolate->TerminateExecution();
}

// response is errback [result, err] array
void v8_call_impl(State *pst, const uint8_t *p, size_t n, bool await)
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
    bool preserve_termination = false;
    bool nested = st.javascript_call_depth > 0;
    JavascriptCallScope call_scope(st.javascript_call_depth);
    if (await && nested) {
        throw_nested_await_call(st);
        cause = RUNTIME_ERROR;
        goto fail;
    }
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
        const char *p = *path;
        const char *pe = p + path.length();
        v8::Local<v8::Object> obj = st.context->Global();
        v8::Local<v8::String> key;
        for (;;) {
            bool last;
            if (!read_path_key(st, p, pe, &key, &last)) goto fail;
            if (last) break;
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
        if (await && !await_promise(st, &result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    preserve_termination = suspend_termination(st, nested, cause);
    if (bubble_up_ruby_exception(st, &try_catch)) {
        restore_termination(st, preserve_termination);
        return;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    // Error formatting can run user JS (for example Error.prepareStackTrace)
    // which may call Context#stop. Do not let that termination leak into the
    // reply or the next evaluation.
    bool terminated_during_error = false;
    if (st.terminate_requested.load() || st.isolate->IsExecutionTerminating()) {
        terminated_during_error = true;
        preserve_termination =
            suspend_termination(st, nested, cause) || preserve_termination;
    }
    if (terminated_during_error) {
        result = v8::Undefined(st.isolate);
        std::vector<char> buf;
        set_error_message(buf, cause,
                          cause == MEMORY_ERROR ? "out of memory" : "terminated");
        err = string_from_bytes(st.isolate, buf);
    }
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
    if (!nested &&
        (st.terminate_requested.load() || st.isolate->IsExecutionTerminating())) {
        // A callback invoked by the serializer (for example a getter) can
        // request termination after the last suspend_termination check. Do not
        // leak that request into the next outermost evaluation.
        st.terminate_requested.store(0);
        st.isolate->CancelTerminateExecution();
        st.err_reason = NO_ERROR;
    }
    restore_termination(st, preserve_termination);
}

extern "C" void v8_call(State *pst, const uint8_t *p, size_t n)
{
    v8_call_impl(pst, p, n, false);
}

extern "C" void v8_call_await(State *pst, const uint8_t *p, size_t n)
{
    v8_call_impl(pst, p, n, true);
}

// response is errback [result, err] array
void v8_eval_impl(State *pst, const uint8_t *p, size_t n, bool await)
{
    State& st = *pst;
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::ValueDeserializer des(st.isolate, p, n);
    des.ReadHeader(st.context).Check();
    v8::Local<v8::Value> result;
    int cause = INTERNAL_ERROR;
    bool preserve_termination = false;
    bool nested = st.javascript_call_depth > 0;
    JavascriptCallScope call_scope(st.javascript_call_depth);
    if (await && nested) {
        throw_nested_await_call(st);
        cause = RUNTIME_ERROR;
        goto fail;
    }
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
        if (await && !await_promise(st, &result_v)) goto fail;
        result = sanitize(st, result_v);
    }
    cause = NO_ERROR;
fail:
    preserve_termination = suspend_termination(st, nested, cause);
    if (bubble_up_ruby_exception(st, &try_catch)) {
        restore_termination(st, preserve_termination);
        return;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    auto err = to_error(st, &try_catch, cause);
    // Error formatting can run user JS (for example Error.prepareStackTrace)
    // which may call Context#stop. Do not let that termination leak into the
    // reply or the next evaluation.
    bool terminated_during_error = false;
    if (st.terminate_requested.load() || st.isolate->IsExecutionTerminating()) {
        terminated_during_error = true;
        preserve_termination =
            suspend_termination(st, nested, cause) || preserve_termination;
    }
    if (terminated_during_error) {
        result = v8::Undefined(st.isolate);
        std::vector<char> buf;
        set_error_message(buf, cause,
                          cause == MEMORY_ERROR ? "out of memory" : "terminated");
        err = string_from_bytes(st.isolate, buf);
    }
    if (!reply(st, result, err)) {
        assert(try_catch.HasCaught());
        goto fail; // retry; can be termination exception
    }
    if (!nested &&
        (st.terminate_requested.load() || st.isolate->IsExecutionTerminating())) {
        // A callback invoked by the serializer (for example a getter) can
        // request termination after the last suspend_termination check. Do not
        // leak that request into the next outermost evaluation.
        st.terminate_requested.store(0);
        st.isolate->CancelTerminateExecution();
        st.err_reason = NO_ERROR;
    }
    restore_termination(st, preserve_termination);
}

extern "C" void v8_eval(State *pst, const uint8_t *p, size_t n)
{
    v8_eval_impl(pst, p, n, false);
}

extern "C" void v8_eval_await(State *pst, const uint8_t *p, size_t n)
{
    v8_eval_impl(pst, p, n, true);
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
    reply_retry(st, response);
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
    const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
    v8_reply(st.ruby_context, os.buf.data(), os.buf.size()); // not serialized because big
}

extern "C" void v8_perform_microtask_checkpoint(State *pst)
{
    // Leave any termination active so the enclosing v8_call/v8_eval frame
    // surfaces OOM (set by v8_gc_callback) or watchdog termination to Ruby.
    State& st = *pst;
    JavascriptCallScope call_scope(st.javascript_call_depth);
    v8::TryCatch try_catch(st.isolate);
    try_catch.SetVerbose(st.verbose_exceptions);
    v8::HandleScope handle_scope(st.isolate);
    v8::MicrotasksScope::PerformCheckpoint(st.isolate);
    reply_retry(st, v8::Undefined(st.isolate));
}

extern "C" void v8_pump_message_loop(State *pst)
{
    State& st = *pst;
    bool nested = st.javascript_call_depth > 0;
    JavascriptCallScope call_scope(st.javascript_call_depth);
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
    // A nested pump must leave termination active for the enclosing call/eval.
    if (!nested &&
        (st.terminate_requested.exchange(0) ||
         st.isolate->IsExecutionTerminating())) {
        st.isolate->CancelTerminateExecution();
        st.err_reason = NO_ERROR;
    }
    auto result = v8::Boolean::New(st.isolate, ran_task);
    reply_retry(st, result);
}

static uint32_t read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0])
         | static_cast<uint32_t>(p[1]) << 8
         | static_cast<uint32_t>(p[2]) << 16
         | static_cast<uint32_t>(p[3]) << 24;
}

static bool snapshot_checksum_matches(const uint8_t *data, size_t n)
{
    if (n < 13)
        return false;
    uint32_t expected = read_le32(data + 8);

    // V8 uses zlib's adler32 when built with V8_USE_ZLIB, otherwise a
    // Fletcher-32 variant. Accept either so this check remains portable
    // across libv8-node builds. This is a corruption check, not a sandbox:
    // snapshots are still trusted-input-only.
    uint32_t s1 = 0, s2 = 0;
    for (size_t i = 12; i < n; i++) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    if ((s2 << 16 | s1) == expected)
        return true;

    s1 = s2 = 0;
    for (size_t i = 12; i < n; i++) {
        s1 = (s1 + data[i]) % 65535;
        s2 = (s2 + s1) % 65535;
    }
    return (s2 << 16 | s1) == expected;
}

// The header/section offsets below mirror V8 13.6's SnapshotImpl. Keep them
// in sync with libv8-node; the fallback is intentionally limited to preventing
// V8's fatal deserializer checks, not to sandboxing untrusted snapshots.
extern "C" int v8_snapshot_validate(const uint8_t *data, size_t n,
                                     char *err, size_t errlen)
{
#define SNAPSHOT_INVALID(msg)                                           \
    do {                                                                \
        if (err && errlen)                                              \
            snprintf(err, errlen, "%s", msg);                           \
        return -1;                                                      \
    } while (0)

    if (n == 0)
        return 0; // Snapshot.load("") means "use V8's built-in snapshot".
    if (n > static_cast<size_t>(std::numeric_limits<int>::max()))
        SNAPSHOT_INVALID("snapshot is too large");
    if (n < 88)
        SNAPSHOT_INVALID("snapshot is too short");

    v8::StartupData blob{reinterpret_cast<const char*>(data),
                         static_cast<int>(n)};
    if (!blob.IsValid())
        SNAPSHOT_INVALID("snapshot version mismatch");

    uint32_t num_contexts = read_le32(data);
    uint32_t rehashability = read_le32(data + 4);
    if (rehashability > 1)
        SNAPSHOT_INVALID("snapshot header is invalid");
    if (num_contexts == 0)
        SNAPSHOT_INVALID("snapshot has no contexts");
    if (num_contexts > (n - 88) / 4)
        SNAPSHOT_INVALID("snapshot context table is invalid");

    const size_t pointer_size = sizeof(void*);
    size_t header_size = 88 + static_cast<size_t>(num_contexts) * 4;
    size_t startup_offset =
        (header_size + pointer_size - 1) & ~(pointer_size - 1);
    if (startup_offset > n)
        SNAPSHOT_INVALID("snapshot context table is invalid");

    uint32_t read_only_offset = read_le32(data + 80);
    uint32_t shared_heap_offset = read_le32(data + 84);
    if (read_only_offset <= startup_offset || read_only_offset > n)
        SNAPSHOT_INVALID("snapshot read-only section is invalid");
    if (shared_heap_offset <= read_only_offset || shared_heap_offset > n)
        SNAPSHOT_INVALID("snapshot shared-heap section is invalid");

    size_t previous_offset = shared_heap_offset;
    for (uint32_t i = 0; i < num_contexts; i++) {
        uint32_t offset = read_le32(data + 88 + static_cast<size_t>(i) * 4);
        if (offset <= previous_offset || offset >= n)
            SNAPSHOT_INVALID("snapshot context section is invalid");
        previous_offset = offset;
    }

    if (!snapshot_checksum_matches(data, n))
        SNAPSHOT_INVALID("snapshot checksum mismatch");

    return 0;
#undef SNAPSHOT_INVALID
}

// Keep all guard state in a JavaScript closure. The setup script returns a
// query function to C++; do not install a native callback or External global,
// because a retained property descriptor would otherwise make CreateBlob fail
// with "Unknown external reference".
static const char snapshot_intl_setup_source[] = R"js(
;(function() {
    const original = globalThis.Intl
    let used = false
    const getter = function() {
        used = true
        return original
    }
    const setter = function(value) {
        used = true
    }
    if (!original) {
        const query = function() { return used }
        query.getter = getter
        return query
    }
    Object.defineProperty(globalThis, "Intl", {
        configurable: true,
        enumerable: false,
        get: getter,
        set: setter
    })
    const query = function() { return used }
    query.getter = getter
    return query
})()
)js";

static const char snapshot_intl_error[] =
    "Intl is not supported in Snapshot.new scripts";

int snapshot(bool is_warmup, bool verbose_exceptions,
             const v8::String::Utf8Value& code,
             v8::StartupData blob, v8::StartupData *result,
             std::vector<char> *errbuf)
{
    // SnapshotCreator takes ownership of isolate
    v8::Isolate *isolate = v8::Isolate::Allocate();
    // An empty loaded snapshot means "start from V8's built-in snapshot", just
    // like Context.new. Passing a non-null StartupData with raw_size == 0 to
    // SnapshotCreator trips CHECK(!artifacts) during warmup.
    v8::StartupData *existing_blob =
        (is_warmup && blob.data && blob.raw_size > 0) ? &blob : nullptr;
    v8::SnapshotCreator snapshot_creator(isolate, nullptr, existing_blob);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(verbose_exceptions);
    auto mode = is_warmup
        ? v8::SnapshotCreator::FunctionCodeHandling::kKeep
        : v8::SnapshotCreator::FunctionCodeHandling::kClear;
    int cause = INTERNAL_ERROR;
    {
        // Keep script-created values out of the scope that is alive when
        // CreateBlob() runs. V8's own warmup implementation uses separate
        // scopes; sharing one scope leaves Intl foreign handles visible to the
        // snapshot handle checker and aborts with
        // CheckGlobalAndEternalHandles failed.
        v8::HandleScope handle_scope(isolate);
        auto filename = is_warmup
            ? v8::String::NewFromUtf8Literal(isolate, "<warmup>")
            : v8::String::NewFromUtf8Literal(isolate, "<snapshot>");
        auto context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);
        auto intl_key = v8::String::NewFromUtf8Literal(isolate, "Intl");
        v8::Local<v8::Value> intl_original;
        v8::Local<v8::Function> intl_state_fn;
        v8::Local<v8::Function> intl_guard_getter;
        bool intl_guarded = false;
        if (!context->Global()->Get(context, intl_key).ToLocal(&intl_original)) {
            set_error_message(*errbuf, INTERNAL_ERROR,
                              "failed to inspect snapshot Intl");
            return INTERNAL_ERROR;
        }
        if (!is_warmup && intl_original->IsObject()) {
            auto intl_setup_source = v8::String::NewFromUtf8Literal(
                isolate, snapshot_intl_setup_source);
            v8::Local<v8::Script> intl_setup_script;
            v8::Local<v8::Value> intl_state;
            if (!v8::Script::Compile(context, intl_setup_source)
                     .ToLocal(&intl_setup_script) ||
                !intl_setup_script->Run(context).ToLocal(&intl_state) ||
                !intl_state->IsFunction()) {
                set_error_message(*errbuf, INTERNAL_ERROR,
                                  "failed to initialize snapshot Intl guard");
                return INTERNAL_ERROR;
            }
            intl_state_fn = intl_state.As<v8::Function>();
            v8::Local<v8::Value> getter;
            auto getter_key =
                v8::String::NewFromUtf8Literal(isolate, "getter");
            if (!intl_state_fn->Get(context, getter_key).ToLocal(&getter) ||
                !getter->IsFunction()) {
                set_error_message(*errbuf, INTERNAL_ERROR,
                                  "failed to initialize snapshot Intl guard");
                return INTERNAL_ERROR;
            }
            intl_guard_getter = getter.As<v8::Function>();
            intl_guarded = true;
        }
        v8::Local<v8::String> source;
        auto type = v8::NewStringType::kNormal;
        if (!v8::String::NewFromUtf8(isolate, *code, type, code.length()).ToLocal(&source)) {
            v8::String::Utf8Value s(isolate, try_catch.Exception());
            if (!set_error_message(*errbuf, cause, s))
                set_error_message(*errbuf, cause, "unexpected failure");
            return cause;
        }
        v8::ScriptOrigin origin(filename);
        v8::Local<v8::Script> script;
        cause = PARSE_ERROR;
        if (!v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
            auto m = try_catch.Message();
            if (m.IsEmpty()) {
                set_error_message(*errbuf, cause, "unexpected failure");
                return cause;
            }
            v8::String::Utf8Value s(isolate, m->Get());
            v8::String::Utf8Value name(isolate, m->GetScriptResourceName());
            auto line = m->GetLineNumber(context).FromMaybe(0);
            auto column = m->GetStartColumn(context).FromMaybe(0);
            if (*s && *name) {
                errbuf->clear();
                errbuf->push_back(static_cast<char>(cause));
                append_utf8(*errbuf, s);
                append_literal(*errbuf, "\n");
                append_utf8(*errbuf, name);
                append_format(*errbuf, ":%d:%d", line, column);
            } else {
                set_error_message(*errbuf, cause, "unexpected failure");
            }
            return cause;
        }
        cause = RUNTIME_ERROR;
        if (script->Run(context).IsEmpty()) {
            auto m = try_catch.Message();
            if (m.IsEmpty()) {
                set_error_message(*errbuf, cause, "unexpected failure");
                return cause;
            }
            v8::String::Utf8Value s(isolate, m->Get());
            v8::String::Utf8Value name(isolate, m->GetScriptResourceName());
            auto line = m->GetLineNumber(context).FromMaybe(0);
            auto column = m->GetStartColumn(context).FromMaybe(0);
            if (*s && *name) {
                errbuf->clear();
                errbuf->push_back(static_cast<char>(cause));
                append_utf8(*errbuf, s);
                append_literal(*errbuf, "\n");
                append_utf8(*errbuf, name);
                append_format(*errbuf, ":%d:%d", line, column);
            } else {
                set_error_message(*errbuf, cause, "unexpected failure");
            }
            return cause;
        }
        if (intl_guarded) {
            v8::Local<v8::Value> used;
            auto recv = v8::Undefined(isolate);
            if (!intl_state_fn->Call(context, recv, 0, nullptr)
                     .ToLocal(&used)) {
                set_error_message(*errbuf, INTERNAL_ERROR,
                                  "failed to inspect snapshot Intl guard");
                return INTERNAL_ERROR;
            }
            if (used->IsTrue()) {
                set_error_message(*errbuf, RUNTIME_ERROR, snapshot_intl_error);
                return RUNTIME_ERROR;
            }
        }
        if (intl_guarded) {
            // Only replace our accessor. If the script deliberately replaced
            // or deleted Intl, leave that change in the snapshot.
            v8::Local<v8::Value> descriptor;
            if (!context->Global()
                     ->GetOwnPropertyDescriptor(context, intl_key)
                     .ToLocal(&descriptor)) {
                set_error_message(*errbuf, INTERNAL_ERROR,
                                  "failed to inspect Intl after snapshot script");
                return INTERNAL_ERROR;
            }
            bool restore_intl = false;
            if (descriptor->IsObject()) {
                auto get_key =
                    v8::String::NewFromUtf8Literal(isolate, "get");
                v8::Local<v8::Value> getter_descriptor;
                if (!descriptor.As<v8::Object>()
                         ->GetOwnPropertyDescriptor(context, get_key)
                         .ToLocal(&getter_descriptor)) {
                    set_error_message(*errbuf, INTERNAL_ERROR,
                                      "failed to inspect Intl after snapshot script");
                    return INTERNAL_ERROR;
                }
                if (getter_descriptor->IsObject()) {
                    v8::Local<v8::Value> getter;
                    auto value_key =
                        v8::String::NewFromUtf8Literal(isolate, "value");
                    if (!getter_descriptor.As<v8::Object>()
                             ->Get(context, value_key)
                             .ToLocal(&getter)) {
                        set_error_message(*errbuf, INTERNAL_ERROR,
                                          "failed to inspect Intl after snapshot script");
                        return INTERNAL_ERROR;
                    }
                    restore_intl =
                        getter->IsFunction() &&
                        getter->SameValue(intl_guard_getter);
                }
            }
            if (restore_intl &&
                !context->Global()
                     ->DefineOwnProperty(
                         context, intl_key, intl_original,
                         v8::PropertyAttribute::DontEnum)
                     .FromMaybe(false)) {
                set_error_message(*errbuf, INTERNAL_ERROR,
                                  "failed to restore Intl after snapshot script");
                return INTERNAL_ERROR;
            }
        }
        if (!is_warmup) snapshot_creator.SetDefaultContext(context);
    }
    if (is_warmup) {
        v8::HandleScope handle_scope(isolate);
        isolate->ContextDisposedNotification(false);
        auto context = v8::Context::New(isolate);
        snapshot_creator.SetDefaultContext(context);
    }
    *result = snapshot_creator.CreateBlob(mode);
    return NO_ERROR;
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
    std::vector<char> errbuf;
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
    if (st.terminate_requested.exchange(0) ||
        st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    v8::Local<v8::Value> err;
    if (!errbuf.empty()) {
        err = string_from_bytes(st.isolate, errbuf);
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
    std::vector<char> errbuf;
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
    if (st.terminate_requested.exchange(0) ||
        st.isolate->IsExecutionTerminating()) {
        st.isolate->CancelTerminateExecution();
        cause = st.err_reason ? st.err_reason : TERMINATED_ERROR;
        st.err_reason = NO_ERROR;
    }
    if (!cause && try_catch.HasCaught()) cause = RUNTIME_ERROR;
    if (cause) result = v8::Undefined(st.isolate);
    v8::Local<v8::Value> err;
    if (!errbuf.empty()) {
        err = string_from_bytes(st.isolate, errbuf);
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

struct WakeupTask : public v8::Task
{
    void Run() final {}
};

// called from ruby or watchdog thread
void request_termination(State *pst, unsigned request)
{
    pst->terminate_requested.fetch_or(request);
    pst->isolate->TerminateExecution();
    // Wake await_promise if it may be blocked. A racing early return can leave
    // one harmless no-op task queued.
    if (pst->waiting_for_task.exchange(false)) {
        platform->GetForegroundTaskRunner(pst->isolate)
            ->PostTask(std::make_unique<WakeupTask>());
    }
}

extern "C" void v8_terminate_execution(State *pst)
{
    request_termination(pst, NON_WATCHDOG_TERMINATION);
}

extern "C" void v8_terminate_watchdog(State *pst)
{
    request_termination(pst, WATCHDOG_TERMINATION);
}

extern "C" void v8_cancel_watchdog_termination(State *pst)
{
    unsigned requests = pst->terminate_requested.fetch_and(~WATCHDOG_TERMINATION);
    if (requests == WATCHDOG_TERMINATION) {
        pst->isolate->CancelTerminateExecution();
        if (pst->terminate_requested.load())
            pst->isolate->TerminateExecution();
    }
}

// called from ruby thread
extern "C" void v8_cancel_terminate_execution(State *pst)
{
    // TerminateExecution can race with V8 completing and queue a termination
    // for the next entry without IsExecutionTerminating() becoming true.
    pst->terminate_requested.store(0);
    pst->isolate->CancelTerminateExecution();
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
        persistent_safe_context_function.Reset();
        persistent_safe_context.Reset();
        persistent_context.Reset();
        ruby_exception.Reset();
    }
    isolate->Dispose();
    for (Callback *cb : callbacks)
        delete cb;
}
