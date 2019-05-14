#include <stdio.h>
#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/io.h>
#include <v8.h>
#include <v8-profiler.h>
#include <libplatform/libplatform.h>
#include <ruby/encoding.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <atomic>
#include <math.h>

using namespace v8;

typedef struct {
    const char* data;
    int raw_size;
} SnapshotInfo;

class IsolateInfo {
public:
    Isolate* isolate;
    ArrayBuffer::Allocator* allocator;
    StartupData* startup_data;
    bool interrupted;
    pid_t pid;
    VALUE mutex;

    class Lock {
        VALUE &mutex;

    public:
        Lock(VALUE &mutex) : mutex(mutex) {
            rb_mutex_lock(mutex);
        }
        ~Lock() {
            rb_mutex_unlock(mutex);
        }
    };


    IsolateInfo() : isolate(nullptr), allocator(nullptr), startup_data(nullptr),
        interrupted(false), pid(getpid()), refs_count(0) {
        VALUE cMutex = rb_const_get(rb_cThread, rb_intern("Mutex"));
        mutex = rb_class_new_instance(0, nullptr, cMutex);
    }

    ~IsolateInfo() {
        void free_isolate(IsolateInfo*);
        free_isolate(this);
    }

    void init(SnapshotInfo* snapshot_info = nullptr);

    void mark() {
        rb_gc_mark(mutex);
    }

    Lock createLock() {
        Lock lock(mutex);
        return lock;
    }

    void hold() {
        refs_count++;
    }
    void release() {
        if (--refs_count <= 0) {
            delete this;
        }
    }

    int refs() {
        return refs_count;
    }

    static void* operator new(size_t size) {
        return ruby_xmalloc(size);
    }

    static void operator delete(void *block) {
        xfree(block);
    }
private:
    // how many references to this isolate exist
    // we can't rely on Ruby's GC for this, because Ruby could destroy the
    // isolate before destroying the contexts that depend on them. We'd need to
    // keep a list of linked contexts in the isolate to destroy those first when
    // isolate destruction was requested. Keeping such a list would require
    // notification from the context VALUEs when they are constructed and
    // destroyed. With a ref count, those notifications are still needed, but
    // we keep a simple int rather than a list of pointers.
    std::atomic_int refs_count;
};

typedef struct {
    IsolateInfo* isolate_info;
    Persistent<Context>* context;
} ContextInfo;

typedef struct {
    bool parsed;
    bool executed;
    bool terminated;
    bool json;
    Persistent<Value>* value;
    Persistent<Value>* message;
    Persistent<Value>* backtrace;
} EvalResult;

typedef struct {
    ContextInfo* context_info;
    Local<String>* eval;
    Local<String>* filename;
    useconds_t timeout;
    EvalResult* result;
    size_t max_memory;
} EvalParams;

typedef struct {
    ContextInfo *context_info;
    char *function_name;
    int argc;
    bool error;
    Local<Function> fun;
    Local<Value> *argv;
    EvalResult result;
} FunctionCall;

enum IsolateFlags {
    IN_GVL,
    DO_TERMINATE,
    MEM_SOFTLIMIT_VALUE,
    MEM_SOFTLIMIT_REACHED,
};

static VALUE rb_cContext;
static VALUE rb_cSnapshot;
static VALUE rb_cIsolate;

static VALUE rb_eScriptTerminatedError;
static VALUE rb_eV8OutOfMemoryError;
static VALUE rb_eParseError;
static VALUE rb_eScriptRuntimeError;
static VALUE rb_cJavaScriptFunction;
static VALUE rb_eSnapshotError;
static VALUE rb_ePlatformAlreadyInitializedError;
static VALUE rb_mJSON;

static VALUE rb_cFailedV8Conversion;
static VALUE rb_cDateTime = Qnil;

static Platform* current_platform = NULL;
static std::mutex platform_lock;

static VALUE rb_platform_set_flag_as_str(VALUE _klass, VALUE flag_as_str) {
    bool platform_already_initialized = false;

    if(TYPE(flag_as_str) != T_STRING) {
        rb_raise(rb_eArgError, "wrong type argument %" PRIsVALUE" (should be a string)",
                rb_obj_class(flag_as_str));
    }

    platform_lock.lock();

    if (current_platform == NULL) {
        V8::SetFlagsFromString(RSTRING_PTR(flag_as_str), (int)RSTRING_LEN(flag_as_str));
    } else {
        platform_already_initialized = true;
    }

    platform_lock.unlock();

    // important to raise outside of the lock
    if (platform_already_initialized) {
        rb_raise(rb_ePlatformAlreadyInitializedError, "The V8 platform is already initialized");
    }

    return Qnil;
}

static void init_v8() {
    // no need to wait for the lock if already initialized
    if (current_platform != NULL) return;

    platform_lock.lock();

    if (current_platform == NULL) {
        V8::InitializeICU();
        current_platform = platform::CreateDefaultPlatform();
        V8::InitializePlatform(current_platform);
        V8::Initialize();
    }

    platform_lock.unlock();
}

static void gc_callback(Isolate *isolate, GCType type, GCCallbackFlags flags) {
    if((bool)isolate->GetData(MEM_SOFTLIMIT_REACHED)) return;

    size_t softlimit = *(size_t*) isolate->GetData(MEM_SOFTLIMIT_VALUE);

    HeapStatistics stats;
    isolate->GetHeapStatistics(&stats);
    size_t used = stats.used_heap_size();

    if(used > softlimit) {
        isolate->SetData(MEM_SOFTLIMIT_REACHED, (void*)true);
        isolate->TerminateExecution();
    }
}

// to be called with active lock and scope
static void prepare_result(MaybeLocal<Value> v8res,
                           TryCatch& trycatch,
                           Isolate* isolate,
                           Local<Context> context,
                           EvalResult& evalRes /* out */) {

    // just don't touch .parsed
    evalRes.terminated = false;
    evalRes.json = false;
    evalRes.value = nullptr;
    evalRes.message = nullptr;
    evalRes.backtrace = nullptr;
    evalRes.executed = !v8res.IsEmpty();

    if (evalRes.executed) {
        // arrays and objects get converted to json
        Local<Value> local_value = v8res.ToLocalChecked();
        if ((local_value->IsObject() || local_value->IsArray()) &&
                !local_value->IsDate() && !local_value->IsFunction()) {
            Local<Object> JSON = context->Global()->Get(String::NewFromUtf8(isolate, "JSON"))
              ->ToObject(context).ToLocalChecked();

            Local<Function> stringify = JSON->Get(v8::String::NewFromUtf8(isolate, "stringify"))
                    .As<Function>();

            Local<Object> object = local_value->ToObject(context).ToLocalChecked();
            const unsigned argc = 1;
            Local<Value> argv[argc] = { object };
            MaybeLocal<Value> json = stringify->Call(JSON, argc, argv);

            if (json.IsEmpty()) {
                evalRes.executed = false;
            } else {
                evalRes.json = true;
                Persistent<Value>* persistent = new Persistent<Value>();
                persistent->Reset(isolate, json.ToLocalChecked());
                evalRes.value = persistent;
            }

        } else {
            Persistent<Value>* persistent = new Persistent<Value>();
            persistent->Reset(isolate, local_value);
            evalRes.value = persistent;
        }
    }

    if (!evalRes.executed || !evalRes.parsed) {
        if (trycatch.HasCaught()) {
            if (!trycatch.Exception()->IsNull()) {
                evalRes.message = new Persistent<Value>();
                Local<Message> message = trycatch.Message();
                char buf[1000];
                int len, line, column;

                if (!message->GetLineNumber(context).To(&line)) {
                  line = 0;
                }

                if (!message->GetStartColumn(context).To(&column)) {
                  column = 0;
                }

                len = snprintf(buf, sizeof(buf), "%s at %s:%i:%i", *String::Utf8Value(isolate, message->Get()),
                               *String::Utf8Value(isolate, message->GetScriptResourceName()->ToString(context).ToLocalChecked()),
                               line,
                               column);

                if ((size_t) len >= sizeof(buf)) {
                    len = sizeof(buf) - 1;
                    buf[len] = '\0';
                }

                Local<String> v8_message = String::NewFromUtf8(isolate, buf, NewStringType::kNormal, len).ToLocalChecked();
                evalRes.message->Reset(isolate, v8_message);
            } else if(trycatch.HasTerminated()) {
                evalRes.terminated = true;
                evalRes.message = new Persistent<Value>();
                Local<String> tmp = String::NewFromUtf8(isolate, "JavaScript was terminated (either by timeout or explicitly)");
                evalRes.message->Reset(isolate, tmp);
            }
            if (!trycatch.StackTrace(context).IsEmpty()) {
                evalRes.backtrace = new Persistent<Value>();
                evalRes.backtrace->Reset(isolate,
                                         trycatch.StackTrace(context).ToLocalChecked()->ToString(context).ToLocalChecked());
            }
        }
    }
}

void*
nogvl_context_eval(void* arg) {

    EvalParams* eval_params = (EvalParams*)arg;
    EvalResult* result = eval_params->result;
    Isolate* isolate = eval_params->context_info->isolate_info->isolate;

    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    TryCatch trycatch(isolate);
    Local<Context> context = eval_params->context_info->context->Get(isolate);
    Context::Scope context_scope(context);
    v8::ScriptOrigin *origin = NULL;

    // in gvl flag
    isolate->SetData(IN_GVL, (void*)false);
    // terminate ASAP
    isolate->SetData(DO_TERMINATE, (void*)false);
    // Memory softlimit
    isolate->SetData(MEM_SOFTLIMIT_VALUE, (void*)false);
    // Memory softlimit hit flag
    isolate->SetData(MEM_SOFTLIMIT_REACHED, (void*)false);

    MaybeLocal<Script> parsed_script;

    if (eval_params->filename) {
        origin = new v8::ScriptOrigin(*eval_params->filename);
    }

    parsed_script = Script::Compile(context, *eval_params->eval, origin);

    if (origin) {
        delete origin;
    }

    result->parsed = !parsed_script.IsEmpty();
    result->executed = false;
    result->terminated = false;
    result->json = false;
    result->value = NULL;

    MaybeLocal<Value> maybe_value;
    if (!result->parsed) {
        result->message = new Persistent<Value>();
        result->message->Reset(isolate, trycatch.Exception());
    } else {
        // parsing successful
        if (eval_params->max_memory > 0) {
            isolate->SetData(MEM_SOFTLIMIT_VALUE, &eval_params->max_memory);
            isolate->AddGCEpilogueCallback(gc_callback);
        }

        maybe_value = parsed_script.ToLocalChecked()->Run(context);
    }

    prepare_result(maybe_value, trycatch, isolate, context, *result);

    isolate->SetData(IN_GVL, (void*)true);

    return NULL;
}

// assumes isolate locking is in place
static VALUE convert_v8_to_ruby(Isolate* isolate, Local<Context> context,
                                Local<Value> value) {

    Isolate::Scope isolate_scope(isolate);
    HandleScope scope(isolate);

    if (value->IsNull() || value->IsUndefined()){
        return Qnil;
    }

    if (value->IsInt32()) {
        return INT2FIX(value->Int32Value(context).ToChecked());
    }

    if (value->IsNumber()) {
        return rb_float_new(value->NumberValue(context).ToChecked());
    }

    if (value->IsTrue()) {
        return Qtrue;
    }

    if (value->IsFalse()) {
        return Qfalse;
    }

    if (value->IsArray()) {
      VALUE rb_array = rb_ary_new();
      Local<Array> arr = Local<Array>::Cast(value);
      for(uint32_t i=0; i < arr->Length(); i++) {
          Local<Value> element = arr->Get(i);
          VALUE rb_elem = convert_v8_to_ruby(isolate, context, element);
          if (rb_funcall(rb_elem, rb_intern("class"), 0) == rb_cFailedV8Conversion) {
            return rb_elem;
          }
          rb_ary_push(rb_array, rb_elem);
      }
      return rb_array;
    }

    if (value->IsFunction()){
        return rb_funcall(rb_cJavaScriptFunction, rb_intern("new"), 0);
    }

    if (value->IsDate()){
        double ts = Local<Date>::Cast(value)->ValueOf();
        double secs = ts/1000;
        long nanos = round((secs - floor(secs)) * 1000000);

        return rb_time_new(secs, nanos);
    }

    if (value->IsObject()) {
        VALUE rb_hash = rb_hash_new();
        TryCatch trycatch(isolate);

        Local<Object> object = value->ToObject(context).ToLocalChecked();
        auto maybe_props = object->GetOwnPropertyNames(context);
        if (!maybe_props.IsEmpty()) {
            Local<Array> props = maybe_props.ToLocalChecked();
            for(uint32_t i=0; i < props->Length(); i++) {
             Local<Value> key = props->Get(i);
             VALUE rb_key = convert_v8_to_ruby(isolate, context, key);
             Local<Value> prop_value = object->Get(key);
             // this may have failed due to Get raising

             if (trycatch.HasCaught()) {
             // TODO isolate code that translates execption to ruby
             // exception so we can properly return it
             return rb_funcall(rb_cFailedV8Conversion, rb_intern("new"), 1, rb_str_new2(""));
             }

             VALUE rb_value = convert_v8_to_ruby(isolate, context, prop_value);
             rb_hash_aset(rb_hash, rb_key, rb_value);
            }
        }
        return rb_hash;
    }

    Local<String> rstr = value->ToString(context).ToLocalChecked();
    return rb_enc_str_new(*String::Utf8Value(isolate, rstr), rstr->Utf8Length(isolate), rb_enc_find("utf-8"));
}

static VALUE convert_v8_to_ruby(Isolate* isolate,
                                const Persistent<Context>& context,
                                Local<Value> value) {
    HandleScope scope(isolate);
    return convert_v8_to_ruby(isolate,
                              Local<Context>::New(isolate, context),
                              value);
}

static VALUE convert_v8_to_ruby(Isolate* isolate,
                                const Persistent<Context>& context,
                                const Persistent<Value>& value) {
    HandleScope scope(isolate);
    return convert_v8_to_ruby(isolate,
                              Local<Context>::New(isolate, context),
                              Local<Value>::New(isolate, value));
}

static Local<Value> convert_ruby_to_v8(Isolate* isolate, Local<Context> context, VALUE value) {
    EscapableHandleScope scope(isolate);

    Local<Array> array;
    Local<Object> object;
    VALUE hash_as_array;
    VALUE pair;
    int i;
    long length;
    long fixnum;
    VALUE klass;

    switch (TYPE(value)) {
    case T_FIXNUM:
        fixnum = NUM2LONG(value);
        if (fixnum > INT_MAX)
        {
            return scope.Escape(Number::New(isolate, (double)fixnum));
        }
        return scope.Escape(Integer::New(isolate, (int)fixnum));
    case T_FLOAT:
	return scope.Escape(Number::New(isolate, NUM2DBL(value)));
    case T_STRING:
	return scope.Escape(String::NewFromUtf8(isolate, RSTRING_PTR(value), NewStringType::kNormal, (int)RSTRING_LEN(value)).ToLocalChecked());
    case T_NIL:
	return scope.Escape(Null(isolate));
    case T_TRUE:
	return scope.Escape(True(isolate));
    case T_FALSE:
	return scope.Escape(False(isolate));
    case T_ARRAY:
	length = RARRAY_LEN(value);
	array = Array::New(isolate, (int)length);
	for(i=0; i<length; i++) {
      array->Set(i, convert_ruby_to_v8(isolate, context, rb_ary_entry(value, i)));
	}
	return scope.Escape(array);
    case T_HASH:
	object = Object::New(isolate);
	hash_as_array = rb_funcall(value, rb_intern("to_a"), 0);
	length = RARRAY_LEN(hash_as_array);
	for(i=0; i<length; i++) {
	    pair = rb_ary_entry(hash_as_array, i);
	    object->Set(convert_ruby_to_v8(isolate, context, rb_ary_entry(pair, 0)),
                  convert_ruby_to_v8(isolate, context, rb_ary_entry(pair, 1)));
	}
	return scope.Escape(object);
    case T_SYMBOL:
	value = rb_funcall(value, rb_intern("to_s"), 0);
	return scope.Escape(String::NewFromUtf8(isolate, RSTRING_PTR(value), NewStringType::kNormal, (int)RSTRING_LEN(value)).ToLocalChecked());
    case T_DATA:
        klass = rb_funcall(value, rb_intern("class"), 0);
        if (klass == rb_cTime || klass == rb_cDateTime)
        {
            if (klass == rb_cDateTime)
            {
                value = rb_funcall(value, rb_intern("to_time"), 0);
            }
            value = rb_funcall(value, rb_intern("to_f"), 0);
            return scope.Escape(Date::New(context, NUM2DBL(value) * 1000).ToLocalChecked());
        }
    case T_OBJECT:
    case T_CLASS:
    case T_ICLASS:
    case T_MODULE:
    case T_REGEXP:
    case T_MATCH:
    case T_STRUCT:
    case T_BIGNUM:
    case T_FILE:
    case T_UNDEF:
    case T_NODE:
    default:
      return scope.Escape(String::NewFromUtf8(isolate, "Undefined Conversion"));
    }
}

static void unblock_eval(void *ptr) {
    EvalParams* eval = (EvalParams*)ptr;
    eval->context_info->isolate_info->interrupted = true;
}

/*
 * The implementations of the run_extra_code(), create_snapshot_data_blob() and
 * warm_up_snapshot_data_blob() functions have been derived from V8's test suite.
 */
bool run_extra_code(Isolate *isolate, Local<v8::Context> context,
                    const char *utf8_source, const char *name) {
    Context::Scope context_scope(context);
    TryCatch try_catch(isolate);
    Local<String> source_string;
    if (!String::NewFromUtf8(isolate, utf8_source,
                             NewStringType::kNormal)
             .ToLocal(&source_string)) {
        return false;
    }
    Local<v8::String> resource_name =
        String::NewFromUtf8(isolate, name, NewStringType::kNormal)
            .ToLocalChecked();
    ScriptOrigin origin(resource_name);
    ScriptCompiler::Source source(source_string, origin);
    Local<Script> script;
    if (!ScriptCompiler::Compile(context, &source).ToLocal(&script))
        return false;
    if (script->Run(context).IsEmpty())
        return false;
    // CHECK(!try_catch.HasCaught());
    return true;
}

StartupData
create_snapshot_data_blob(const char *embedded_source = nullptr) {
    // Create a new isolate and a new context from scratch, optionally run
    // a script to embed, and serialize to create a snapshot blob.
    StartupData result = {nullptr, 0};
    {
        SnapshotCreator snapshot_creator;
        Isolate *isolate = snapshot_creator.GetIsolate();
        {
            HandleScope scope(isolate);
            Local<Context> context = Context::New(isolate);
            if (embedded_source != nullptr &&
                !run_extra_code(isolate, context, embedded_source,
                                "<embedded>")) {
                return result;
            }
            snapshot_creator.SetDefaultContext(context);
        }
        result = snapshot_creator.CreateBlob(
            SnapshotCreator::FunctionCodeHandling::kClear);
    }
    return result;
}

StartupData warm_up_snapshot_data_blob(StartupData cold_snapshot_blob,
                                       const char *warmup_source) {
    // Use following steps to create a warmed up snapshot blob from a cold one:
    //  - Create a new isolate from the cold snapshot.
    //  - Create a new context to run the warmup script. This will trigger
    //    compilation of executed functions.
    //  - Create a new context. This context will be unpolluted.
    //  - Serialize the isolate and the second context into a new snapshot blob.
    StartupData result = {nullptr, 0};

    if (cold_snapshot_blob.raw_size > 0 && cold_snapshot_blob.data != nullptr &&
        warmup_source != NULL) {
        SnapshotCreator snapshot_creator(nullptr, &cold_snapshot_blob);
        Isolate *isolate = snapshot_creator.GetIsolate();
        {
            HandleScope scope(isolate);
            Local<Context> context = Context::New(isolate);
            if (!run_extra_code(isolate, context, warmup_source, "<warm-up>")) {
                return result;
            }
        }
        {
            HandleScope handle_scope(isolate);
            isolate->ContextDisposedNotification(false);
            Local<Context> context = Context::New(isolate);
            snapshot_creator.SetDefaultContext(context);
        }
        result = snapshot_creator.CreateBlob(
            SnapshotCreator::FunctionCodeHandling::kKeep);
    }
    return result;
}

static VALUE rb_snapshot_size(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    return INT2NUM(snapshot_info->raw_size);
}

static VALUE rb_snapshot_load(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    if(TYPE(str) != T_STRING) {
        rb_raise(rb_eArgError, "wrong type argument %" PRIsVALUE " (should be a string)",
                rb_obj_class(str));
    }

    init_v8();

    StartupData startup_data = create_snapshot_data_blob(RSTRING_PTR(str));

    if (startup_data.data == NULL && startup_data.raw_size == 0) {
        rb_raise(rb_eSnapshotError, "Could not create snapshot, most likely the source is incorrect");
    }

    snapshot_info->data = startup_data.data;
    snapshot_info->raw_size = startup_data.raw_size;

    return Qnil;
}

static VALUE rb_snapshot_dump(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    return rb_str_new(snapshot_info->data, snapshot_info->raw_size);
}

static VALUE rb_snapshot_warmup_unsafe(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    if(TYPE(str) != T_STRING) {
        rb_raise(rb_eArgError, "wrong type argument %" PRIsVALUE " (should be a string)",
                rb_obj_class(str));
    }

    init_v8();

    StartupData cold_startup_data = {snapshot_info->data, snapshot_info->raw_size};
    StartupData warm_startup_data = warm_up_snapshot_data_blob(cold_startup_data, RSTRING_PTR(str));

    if (warm_startup_data.data == NULL && warm_startup_data.raw_size == 0) {
        rb_raise(rb_eSnapshotError, "Could not warm up snapshot, most likely the source is incorrect");
    } else {
        delete[] snapshot_info->data;

        snapshot_info->data = warm_startup_data.data;
        snapshot_info->raw_size = warm_startup_data.raw_size;
    }

    return self;
}

void IsolateInfo::init(SnapshotInfo* snapshot_info) {
    allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = allocator;

    if (snapshot_info) {
        int raw_size = snapshot_info->raw_size;
        char* data = new char[raw_size];
        memcpy(data, snapshot_info->data, raw_size);

        startup_data = new StartupData;
        startup_data->data = data;
        startup_data->raw_size = raw_size;

        create_params.snapshot_blob = startup_data;
    }

    isolate = Isolate::New(create_params);
}

static VALUE rb_isolate_init_with_snapshot(VALUE self, VALUE snapshot) {
    IsolateInfo* isolate_info;
    Data_Get_Struct(self, IsolateInfo, isolate_info);

    init_v8();

    SnapshotInfo* snapshot_info = nullptr;
    if (!NIL_P(snapshot)) {
        Data_Get_Struct(snapshot, SnapshotInfo, snapshot_info);
    }

    isolate_info->init(snapshot_info);
    isolate_info->hold();

    return Qnil;
}

static VALUE rb_isolate_idle_notification(VALUE self, VALUE idle_time_in_ms) {
    IsolateInfo* isolate_info;
    Data_Get_Struct(self, IsolateInfo, isolate_info);

    if (current_platform == NULL) return Qfalse;

    double duration = NUM2DBL(idle_time_in_ms) / 1000.0;
    double now = current_platform->MonotonicallyIncreasingTime();
    return isolate_info->isolate->IdleNotificationDeadline(now + duration) ? Qtrue : Qfalse;
}

static VALUE rb_context_init_unsafe(VALUE self, VALUE isolate, VALUE snap) {
    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    init_v8();

    IsolateInfo* isolate_info;

    if (NIL_P(isolate) || !rb_obj_is_kind_of(isolate, rb_cIsolate)) {
        isolate_info = new IsolateInfo();

        SnapshotInfo *snapshot_info = nullptr;
        if (!NIL_P(snap) && rb_obj_is_kind_of(snap, rb_cSnapshot)) {
            Data_Get_Struct(snap, SnapshotInfo, snapshot_info);
        }
        isolate_info->init(snapshot_info);
    } else { // given isolate or snapshot
        Data_Get_Struct(isolate, IsolateInfo, isolate_info);
    }

    context_info->isolate_info = isolate_info;
    isolate_info->hold();

    {
        // the ruby lock is needed if this isn't a new isolate
        IsolateInfo::Lock ruby_lock(isolate_info->mutex);
        Locker lock(isolate_info->isolate);
        Isolate::Scope isolate_scope(isolate_info->isolate);
        HandleScope handle_scope(isolate_info->isolate);

        Local<Context> context = Context::New(isolate_info->isolate);

        context_info->context = new Persistent<Context>();
        context_info->context->Reset(isolate_info->isolate, context);
    }

    if (Qnil == rb_cDateTime && rb_funcall(rb_cObject, rb_intern("const_defined?"), 1, rb_str_new2("DateTime")) == Qtrue)
    {
        rb_cDateTime = rb_const_get(rb_cObject, rb_intern("DateTime"));
    }

    return Qnil;
}

static VALUE convert_result_to_ruby(VALUE self /* context */,
                                    EvalResult& result) {
    ContextInfo *context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    Isolate *isolate = context_info->isolate_info->isolate;
    Persistent<Context> *p_ctx = context_info->context;

    VALUE message = Qnil;
    VALUE backtrace = Qnil;
    {
        Locker lock(isolate);
        if (result.message) {
            message = convert_v8_to_ruby(isolate, *p_ctx, *result.message);
            result.message->Reset();
            delete result.message;
            result.message = nullptr;
        }

        if (result.backtrace) {
            backtrace = convert_v8_to_ruby(isolate, *p_ctx, *result.backtrace);
            result.backtrace->Reset();
            delete result.backtrace;
        }
    }

    // NOTE: this is very important, we can not do an rb_raise from within
    // a v8 scope, if we do the scope is never cleaned up properly and we leak
    if (!result.parsed) {
        if(TYPE(message) == T_STRING) {
            rb_raise(rb_eParseError, "%s", RSTRING_PTR(message));
        } else {
            rb_raise(rb_eParseError, "Unknown JavaScript Error during parse");
        }
    }

    if (!result.executed) {
        VALUE ruby_exception = rb_iv_get(self, "@current_exception");
        if (ruby_exception == Qnil) {
            bool mem_softlimit_reached = (bool)isolate->GetData(MEM_SOFTLIMIT_REACHED);
            // If we were terminated or have the memory softlimit flag set
            if (result.terminated || mem_softlimit_reached) {
                ruby_exception = mem_softlimit_reached ? rb_eV8OutOfMemoryError : rb_eScriptTerminatedError;
            } else {
                ruby_exception = rb_eScriptRuntimeError;
            }

            // exception report about what happened
            if (TYPE(backtrace) == T_STRING) {
                rb_raise(ruby_exception, "%s", RSTRING_PTR(backtrace));
            } else if(TYPE(message) == T_STRING) {
                rb_raise(ruby_exception, "%s", RSTRING_PTR(message));
            } else {
                rb_raise(ruby_exception, "Unknown JavaScript Error during execution");
            }
        } else {
            VALUE rb_str = rb_funcall(ruby_exception, rb_intern("to_s"), 0);
            rb_raise(CLASS_OF(ruby_exception), "%s", RSTRING_PTR(rb_str));
        }
    }

    VALUE ret = Qnil;

    // New scope for return value
    {
        Locker lock(isolate);
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Local<Value> tmp = Local<Value>::New(isolate, *result.value);

        if (result.json) {
            Local<String> rstr = tmp->ToString(p_ctx->Get(isolate)).ToLocalChecked();
            VALUE json_string = rb_enc_str_new(*String::Utf8Value(isolate, rstr), rstr->Utf8Length(isolate), rb_enc_find("utf-8"));
            ret = rb_funcall(rb_mJSON, rb_intern("parse"), 1, json_string);
        } else {
            ret = convert_v8_to_ruby(isolate, *p_ctx, tmp);
        }

        result.value->Reset();
        delete result.value;
    }

    if (rb_funcall(ret, rb_intern("class"), 0) == rb_cFailedV8Conversion) {
        // TODO try to recover stack trace from the conversion error
        rb_raise(rb_eScriptRuntimeError, "Error converting JS object to Ruby object");
    }


    return ret;
}

static VALUE rb_context_eval_unsafe(VALUE self, VALUE str, VALUE filename) {

    EvalParams eval_params;
    EvalResult eval_result;
    ContextInfo* context_info;

    Data_Get_Struct(self, ContextInfo, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;

    if(TYPE(str) != T_STRING) {
        rb_raise(rb_eArgError, "wrong type argument %" PRIsVALUE " (should be a string)",
                rb_obj_class(str));
    }
    if(filename != Qnil && TYPE(filename) != T_STRING) {
        rb_raise(rb_eArgError, "wrong type argument %" PRIsVALUE " (should be nil or a string)",
                rb_obj_class(filename));
    }

    {
        Locker lock(isolate);
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Local<String> eval = String::NewFromUtf8(isolate, RSTRING_PTR(str),
                    NewStringType::kNormal, (int)RSTRING_LEN(str)).ToLocalChecked();

        Local<String> local_filename;

        if (filename != Qnil) {
            local_filename = String::NewFromUtf8(isolate, RSTRING_PTR(filename),
                NewStringType::kNormal, (int)RSTRING_LEN(filename)).ToLocalChecked();
            eval_params.filename = &local_filename;
        } else {
            eval_params.filename = NULL;
        }

        eval_params.context_info = context_info;
        eval_params.eval = &eval;
        eval_params.result = &eval_result;
        eval_params.timeout = 0;
        eval_params.max_memory = 0;
        VALUE timeout = rb_iv_get(self, "@timeout");
        if (timeout != Qnil) {
            eval_params.timeout = (useconds_t)NUM2LONG(timeout);
        }

        VALUE mem_softlimit = rb_iv_get(self, "@max_memory");
        if (mem_softlimit != Qnil) {
            eval_params.max_memory = (size_t)NUM2ULONG(mem_softlimit);
        }

        eval_result.message = NULL;
        eval_result.backtrace = NULL;

        rb_thread_call_without_gvl(nogvl_context_eval, &eval_params, unblock_eval, &eval_params);
    }

    return convert_result_to_ruby(self, eval_result);
}

typedef struct {
    VALUE callback;
    int length;
    VALUE ruby_args;
    bool failed;
} protected_callback_data;

static VALUE protected_callback(VALUE rdata) {
    protected_callback_data* data = (protected_callback_data*)rdata;
    VALUE result;

    if (data->length > 0) {
        result = rb_funcall2(data->callback, rb_intern("call"), data->length,
                     RARRAY_PTR(data->ruby_args));
        RB_GC_GUARD(data->ruby_args);
    } else {
        result = rb_funcall(data->callback, rb_intern("call"), 0);
    }
    return result;
}

static
VALUE rescue_callback(VALUE rdata, VALUE exception) {
    protected_callback_data* data = (protected_callback_data*)rdata;
    data->failed = true;
    return exception;
}

void*
gvl_ruby_callback(void* data) {

    FunctionCallbackInfo<Value>* args = (FunctionCallbackInfo<Value>*)data;
    VALUE ruby_args = Qnil;
    int length = args->Length();
    VALUE callback;
    VALUE result;
    VALUE self;
    VALUE parent;
    ContextInfo* context_info;

    {
        HandleScope scope(args->GetIsolate());
        Local<External> external = Local<External>::Cast(args->Data());

        self = *(VALUE*)(external->Value());
        callback = rb_iv_get(self, "@callback");

        parent = rb_iv_get(self, "@parent");
        if (NIL_P(parent) || !rb_obj_is_kind_of(parent, rb_cContext)) {
            return NULL;
        }

        Data_Get_Struct(parent, ContextInfo, context_info);

        if (length > 0) {
            ruby_args = rb_ary_tmp_new(length);
        }

        for (int i = 0; i < length; i++) {
            Local<Value> value = ((*args)[i]).As<Value>();
            VALUE tmp = convert_v8_to_ruby(args->GetIsolate(),
                                           *context_info->context, value);
            rb_ary_push(ruby_args, tmp);
        }
    }

    // may raise exception stay clear of handle scope
    protected_callback_data callback_data;
    callback_data.length = length;
    callback_data.callback = callback;
    callback_data.ruby_args = ruby_args;
    callback_data.failed = false;

    if ((bool)args->GetIsolate()->GetData(DO_TERMINATE) == true) {
        args->GetIsolate()->ThrowException(String::NewFromUtf8(args->GetIsolate(), "Terminated execution during transition from Ruby to JS"));
        args->GetIsolate()->TerminateExecution();
        if (length > 0) {
            rb_ary_clear(ruby_args);
            rb_gc_force_recycle(ruby_args);
        }
        return NULL;
    }

    result = rb_rescue2((VALUE(*)(...))&protected_callback, (VALUE)(&callback_data),
            (VALUE(*)(...))&rescue_callback, (VALUE)(&callback_data), rb_eException, (VALUE)0);

    if(callback_data.failed) {
        rb_iv_set(parent, "@current_exception", result);
        args->GetIsolate()->ThrowException(String::NewFromUtf8(args->GetIsolate(), "Ruby exception"));
    }
    else {
        HandleScope scope(args->GetIsolate());
        Handle<Value> v8_result = convert_ruby_to_v8(args->GetIsolate(), context_info->context->Get(args->GetIsolate()), result);
        args->GetReturnValue().Set(v8_result);
    }

    if (length > 0) {
        rb_ary_clear(ruby_args);
        rb_gc_force_recycle(ruby_args);
    }

    if ((bool)args->GetIsolate()->GetData(DO_TERMINATE) == true) {
      Isolate* isolate = args->GetIsolate();
      isolate->TerminateExecution();
    }

    return NULL;
}

static void ruby_callback(const FunctionCallbackInfo<Value>& args) {
    bool has_gvl = (bool)args.GetIsolate()->GetData(IN_GVL);

    if(has_gvl) {
        gvl_ruby_callback((void*)&args);
    } else {
        args.GetIsolate()->SetData(IN_GVL, (void*)true);
        rb_thread_call_with_gvl(gvl_ruby_callback, (void*)(&args));
        args.GetIsolate()->SetData(IN_GVL, (void*)false);
    }
}


static VALUE rb_external_function_notify_v8(VALUE self) {

    ContextInfo* context_info;

    VALUE parent = rb_iv_get(self, "@parent");
    VALUE name = rb_iv_get(self, "@name");
    VALUE parent_object = rb_iv_get(self, "@parent_object");
    VALUE parent_object_eval = rb_iv_get(self, "@parent_object_eval");

    bool parse_error = false;
    bool attach_error = false;

    Data_Get_Struct(parent, ContextInfo, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;

    {
        Locker lock(isolate);
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Local<Context> context = context_info->context->Get(isolate);
        Context::Scope context_scope(context);

        Local<String> v8_str = String::NewFromUtf8(isolate, RSTRING_PTR(name),
                              NewStringType::kNormal, (int)RSTRING_LEN(name)).ToLocalChecked();

        // copy self so we can access from v8 external
        VALUE* self_copy;
        Data_Get_Struct(self, VALUE, self_copy);
        *self_copy = self;

        Local<Value> external = External::New(isolate, self_copy);

        if (parent_object == Qnil) {
            context->Global()->Set(v8_str, FunctionTemplate::New(isolate, ruby_callback, external)->GetFunction());
        } else {

            Local<String> eval = String::NewFromUtf8(isolate, RSTRING_PTR(parent_object_eval),
                                  NewStringType::kNormal, (int)RSTRING_LEN(parent_object_eval)).ToLocalChecked();

            MaybeLocal<Script> parsed_script = Script::Compile(context, eval);
            if (parsed_script.IsEmpty()) {
            parse_error = true;
            } else {
                MaybeLocal<Value> maybe_value = parsed_script.ToLocalChecked()->Run(context);
                attach_error = true;

                if (!maybe_value.IsEmpty()) {
                    Local<Value> value = maybe_value.ToLocalChecked();
                    if (value->IsObject()){
                    value.As<Object>()->Set(v8_str, FunctionTemplate::New(isolate, ruby_callback, external)->GetFunction());
                    attach_error = false;
                    }
                }
            }
        }
    }

    // always raise out of V8 context
    if (parse_error) {
        rb_raise(rb_eParseError, "Invalid object %" PRIsVALUE, parent_object);
    }

    if (attach_error) {
        rb_raise(rb_eParseError, "Was expecting %" PRIsVALUE" to be an object", parent_object);
    }

    return Qnil;
}

static VALUE rb_context_isolate_mutex(VALUE self) {
    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    if (!context_info->isolate_info) {
        rb_raise(rb_eScriptRuntimeError, "Context has no Isolate available anymore");
    }

    return context_info->isolate_info->mutex;
}

void free_isolate(IsolateInfo* isolate_info) {

    if (isolate_info->isolate) {
        Locker lock(isolate_info->isolate);
    }

    if (isolate_info->isolate) {
        if (isolate_info->interrupted) {
            fprintf(stderr, "WARNING: V8 isolate was interrupted by Ruby, it can not be disposed and memory will not be reclaimed till the Ruby process exits.\n");
        } else {
            
            if (isolate_info->pid != getpid()) {
                fprintf(stderr, "WARNING: V8 isolate was forked, it can not be disposed and memory will not be reclaimed till the Ruby process exits.\n");
            } else {
                isolate_info->isolate->Dispose();
            }
        }
        isolate_info->isolate = NULL;
    }

    if (isolate_info->startup_data) {
        delete[] isolate_info->startup_data->data;
        delete isolate_info->startup_data;
    }

    delete isolate_info->allocator;
}

static void *free_context_raw(void* arg) {
    ContextInfo* context_info = (ContextInfo*)arg;
    IsolateInfo* isolate_info = context_info->isolate_info;
    Persistent<Context>* context = context_info->context;

    if (context && isolate_info && isolate_info->isolate) {
        Locker lock(isolate_info->isolate);
        v8::Isolate::Scope isolate_scope(isolate_info->isolate);
        context->Reset();
        delete context;
    }

    if (isolate_info) {
        isolate_info->release();
    }

    xfree(context_info);
    return NULL;
}

// destroys everything except freeing the ContextInfo struct (see deallocate())
static void free_context(ContextInfo* context_info) {

    IsolateInfo* isolate_info = context_info->isolate_info;

    ContextInfo* context_info_copy = ALLOC(ContextInfo);
    context_info_copy->isolate_info = context_info->isolate_info;
    context_info_copy->context = context_info->context;

    if (isolate_info && isolate_info->refs() > 1) {
    pthread_t free_context_thread;
    if (pthread_create(&free_context_thread, NULL, free_context_raw, (void*)context_info_copy)) {
        fprintf(stderr, "WARNING failed to release memory in MiniRacer, thread to release could not be created, process will leak memory\n");
    }

    } else {
        free_context_raw(context_info_copy);
    }

    if (context_info->context && isolate_info && isolate_info->isolate) {
        context_info->context = NULL;
    }

    if (isolate_info) {
        context_info->isolate_info = NULL;
    }
}

static void deallocate_isolate(void* data) {

    IsolateInfo* isolate_info = (IsolateInfo*) data;

    isolate_info->release();
}

static void mark_isolate(void* data) {
    IsolateInfo* isolate_info = (IsolateInfo*) data;
    isolate_info->mark();
}

void deallocate(void* data) {
    ContextInfo* context_info = (ContextInfo*)data;

    free_context(context_info);

    xfree(data);
}

static void mark_context(void* data) {
    ContextInfo* context_info = (ContextInfo*)data;
    if (context_info->isolate_info) {
        context_info->isolate_info->mark();
    }
}

void deallocate_external_function(void * data) {
    xfree(data);
}

void deallocate_snapshot(void * data) {
    SnapshotInfo* snapshot_info = (SnapshotInfo*)data;
    delete[] snapshot_info->data;
    xfree(snapshot_info);
}

VALUE allocate_external_function(VALUE klass) {
    VALUE* self = ALLOC(VALUE);
    return Data_Wrap_Struct(klass, NULL, deallocate_external_function, (void*)self);
}

VALUE allocate(VALUE klass) {
    ContextInfo* context_info = ALLOC(ContextInfo);
    context_info->isolate_info = NULL;
    context_info->context = NULL;

    return Data_Wrap_Struct(klass, mark_context, deallocate, (void*)context_info);
}

VALUE allocate_snapshot(VALUE klass) {
    SnapshotInfo* snapshot_info = ALLOC(SnapshotInfo);
    snapshot_info->data = NULL;
    snapshot_info->raw_size = 0;

    return Data_Wrap_Struct(klass, NULL, deallocate_snapshot, (void*)snapshot_info);
}

VALUE allocate_isolate(VALUE klass) {
    IsolateInfo* isolate_info = new IsolateInfo();

    return Data_Wrap_Struct(klass, mark_isolate, deallocate_isolate, (void*)isolate_info);
}

static VALUE
rb_heap_stats(VALUE self) {

    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);
    Isolate* isolate;
    v8::HeapStatistics stats;

    isolate = context_info->isolate_info ? context_info->isolate_info->isolate : NULL;

    VALUE rval = rb_hash_new();

    if (!isolate) {

        rb_hash_aset(rval, ID2SYM(rb_intern("total_physical_size")), ULONG2NUM(0));
        rb_hash_aset(rval, ID2SYM(rb_intern("total_heap_size_executable")), ULONG2NUM(0));
        rb_hash_aset(rval, ID2SYM(rb_intern("total_heap_size")), ULONG2NUM(0));
        rb_hash_aset(rval, ID2SYM(rb_intern("used_heap_size")), ULONG2NUM(0));
        rb_hash_aset(rval, ID2SYM(rb_intern("heap_size_limit")), ULONG2NUM(0));

    } else {
        isolate->GetHeapStatistics(&stats);

        rb_hash_aset(rval, ID2SYM(rb_intern("total_physical_size")), ULONG2NUM(stats.total_physical_size()));
        rb_hash_aset(rval, ID2SYM(rb_intern("total_heap_size_executable")), ULONG2NUM(stats.total_heap_size_executable()));
        rb_hash_aset(rval, ID2SYM(rb_intern("total_heap_size")), ULONG2NUM(stats.total_heap_size()));
        rb_hash_aset(rval, ID2SYM(rb_intern("used_heap_size")), ULONG2NUM(stats.used_heap_size()));
        rb_hash_aset(rval, ID2SYM(rb_intern("heap_size_limit")), ULONG2NUM(stats.heap_size_limit()));
    }

    return rval;
}

// https://github.com/bnoordhuis/node-heapdump/blob/master/src/heapdump.cc
class FileOutputStream : public OutputStream {
 public:
  FileOutputStream(FILE* stream) : stream_(stream) {}

  virtual int GetChunkSize() {
    return 65536;
  }

  virtual void EndOfStream() {}

  virtual WriteResult WriteAsciiChunk(char* data, int size) {
    const size_t len = static_cast<size_t>(size);
    size_t off = 0;

    while (off < len && !feof(stream_) && !ferror(stream_))
      off += fwrite(data + off, 1, len - off, stream_);

    return off == len ? kContinue : kAbort;
  }

 private:
  FILE* stream_;
};


static VALUE
rb_heap_snapshot(VALUE self, VALUE file) {

    rb_io_t *fptr;

    fptr = RFILE(file)->fptr;

    if (!fptr) return Qfalse;

    FILE* fp;
    fp = fdopen(fptr->fd, "w");
    if (fp == NULL) return Qfalse;


    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);
    Isolate* isolate;
    isolate = context_info->isolate_info ? context_info->isolate_info->isolate : NULL;

    if (!isolate) return Qfalse;

    Locker lock(isolate);
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);

    HeapProfiler* heap_profiler = isolate->GetHeapProfiler();

    const HeapSnapshot* const snap = heap_profiler->TakeHeapSnapshot();

    FileOutputStream stream(fp);
    snap->Serialize(&stream, HeapSnapshot::kJSON);

    const_cast<HeapSnapshot*>(snap)->Delete();

    return Qtrue;
}

static VALUE
rb_context_stop(VALUE self) {

    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    Isolate* isolate = context_info->isolate_info->isolate;

    // flag for termination
    isolate->SetData(DO_TERMINATE, (void*)true);

    isolate->TerminateExecution();
    rb_funcall(self, rb_intern("stop_attached"), 0);

    return Qnil;
}

static VALUE
rb_context_dispose(VALUE self) {

    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    free_context(context_info);

    return Qnil;
}

static void*
nogvl_context_call(void *args) {

    FunctionCall *call = (FunctionCall *) args;
    if (!call) {
        return NULL;
    }
    Isolate* isolate = call->context_info->isolate_info->isolate;

    // in gvl flag
    isolate->SetData(IN_GVL, (void*)false);
    // terminate ASAP
    isolate->SetData(DO_TERMINATE, (void*)false);

    Isolate::Scope isolate_scope(isolate);
    EscapableHandleScope handle_scope(isolate);
    TryCatch trycatch(isolate);

    Local<Context> context = call->context_info->context->Get(isolate);
    Context::Scope context_scope(context);

    Local<Function> fun = call->fun;

    EvalResult& eval_res = call->result;
    eval_res.parsed = true;

    MaybeLocal<v8::Value> res = fun->Call(context, context->Global(), call->argc, call->argv);
    prepare_result(res, trycatch, isolate, context, eval_res);

    isolate->SetData(IN_GVL, (void*)true);

    return NULL;
}

static void unblock_function(void *args) {
    FunctionCall *call = (FunctionCall *) args;
    call->context_info->isolate_info->interrupted = true;
}

static VALUE rb_context_call_unsafe(int argc, VALUE *argv, VALUE self) {
    ContextInfo* context_info;
    FunctionCall call;
    VALUE *call_argv = NULL;

    Data_Get_Struct(self, ContextInfo, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;

    if (argc < 1) {
        rb_raise(rb_eArgError, "need at least one argument %d", argc);
    }

    VALUE function_name = argv[0];
    if (TYPE(function_name) != T_STRING) {
        rb_raise(rb_eTypeError, "first argument should be a String");
    }

    char *fname = RSTRING_PTR(function_name);
    if (!fname) {
        return Qnil;
    }

    call.context_info = context_info;
    call.error = false;
    call.function_name = fname;
    call.argc = argc - 1;
    call.argv = NULL;
    if (call.argc > 0) {
        // skip first argument which is the function name
        call_argv = argv + 1;
    }

    bool missingFunction = false;
    {
        Locker lock(isolate);
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Local<Context> context = context_info->context->Get(isolate);
        Context::Scope context_scope(context);

        // examples of such usage can be found in
        // https://github.com/v8/v8/blob/36b32aa28db5e993312f4588d60aad5c8330c8a5/test/cctest/test-api.cc#L15711
        Local<String> fname = String::NewFromUtf8(isolate, call.function_name);
        MaybeLocal<v8::Value> val = context->Global()->Get(fname);

        if (val.IsEmpty() || !val.ToLocalChecked()->IsFunction()) {
            missingFunction = true;
        } else {

            Local<v8::Function> fun = Local<v8::Function>::Cast(val.ToLocalChecked());
            call.fun = fun;
            int fun_argc = call.argc;

            if (fun_argc > 0) {
                call.argv = (v8::Local<Value> *) malloc(sizeof(void *) * fun_argc);
                if (!call.argv) {
                    return Qnil;
                }
                for(int i=0; i < fun_argc; i++) {
                    call.argv[i] = convert_ruby_to_v8(isolate, context, call_argv[i]);
                }
            }
            rb_thread_call_without_gvl(nogvl_context_call, &call, unblock_function, &call);
            free(call.argv);

        }
    }

    if (missingFunction) {
    rb_raise(rb_eScriptRuntimeError, "Unknown JavaScript method invoked");
    }

    return convert_result_to_ruby(self, call.result);
}

static VALUE rb_context_create_isolate_value(VALUE self) {
    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);
    IsolateInfo *isolate_info = context_info->isolate_info;

    if (!isolate_info) {
        return Qnil;
    }

    isolate_info->hold();
    return Data_Wrap_Struct(rb_cIsolate, NULL, &deallocate_isolate, isolate_info);
}

extern "C" {

    void Init_mini_racer_extension ( void )
    {
        VALUE rb_mMiniRacer = rb_define_module("MiniRacer");
        rb_cContext = rb_define_class_under(rb_mMiniRacer, "Context", rb_cObject);
        rb_cSnapshot = rb_define_class_under(rb_mMiniRacer, "Snapshot", rb_cObject);
        rb_cIsolate = rb_define_class_under(rb_mMiniRacer, "Isolate", rb_cObject);
        VALUE rb_cPlatform = rb_define_class_under(rb_mMiniRacer, "Platform", rb_cObject);

        VALUE rb_eError = rb_define_class_under(rb_mMiniRacer, "Error", rb_eStandardError);

        VALUE rb_eEvalError = rb_define_class_under(rb_mMiniRacer, "EvalError", rb_eError);
        rb_eScriptTerminatedError = rb_define_class_under(rb_mMiniRacer, "ScriptTerminatedError", rb_eEvalError);
        rb_eV8OutOfMemoryError = rb_define_class_under(rb_mMiniRacer, "V8OutOfMemoryError", rb_eEvalError);
        rb_eParseError = rb_define_class_under(rb_mMiniRacer, "ParseError", rb_eEvalError);
        rb_eScriptRuntimeError = rb_define_class_under(rb_mMiniRacer, "RuntimeError", rb_eEvalError);

        rb_cJavaScriptFunction = rb_define_class_under(rb_mMiniRacer, "JavaScriptFunction", rb_cObject);
        rb_eSnapshotError = rb_define_class_under(rb_mMiniRacer, "SnapshotError", rb_eError);
        rb_ePlatformAlreadyInitializedError = rb_define_class_under(rb_mMiniRacer, "PlatformAlreadyInitialized", rb_eError);
        rb_cFailedV8Conversion = rb_define_class_under(rb_mMiniRacer, "FailedV8Conversion", rb_cObject);
        rb_mJSON = rb_define_module("JSON");

        VALUE rb_cExternalFunction = rb_define_class_under(rb_cContext, "ExternalFunction", rb_cObject);

        rb_define_method(rb_cContext, "stop", (VALUE(*)(...))&rb_context_stop, 0);
        rb_define_method(rb_cContext, "dispose_unsafe", (VALUE(*)(...))&rb_context_dispose, 0);
        rb_define_method(rb_cContext, "heap_stats", (VALUE(*)(...))&rb_heap_stats, 0);
	rb_define_method(rb_cContext, "write_heap_snapshot_unsafe", (VALUE(*)(...))&rb_heap_snapshot, 1);

        rb_define_private_method(rb_cContext, "create_isolate_value",(VALUE(*)(...))&rb_context_create_isolate_value, 0);
        rb_define_private_method(rb_cContext, "eval_unsafe",(VALUE(*)(...))&rb_context_eval_unsafe, 2);
        rb_define_private_method(rb_cContext, "call_unsafe", (VALUE(*)(...))&rb_context_call_unsafe, -1);
        rb_define_private_method(rb_cContext, "isolate_mutex", (VALUE(*)(...))&rb_context_isolate_mutex, 0);
        rb_define_private_method(rb_cContext, "init_unsafe",(VALUE(*)(...))&rb_context_init_unsafe, 2);

        rb_define_alloc_func(rb_cContext, allocate);
        rb_define_alloc_func(rb_cSnapshot, allocate_snapshot);
        rb_define_alloc_func(rb_cIsolate, allocate_isolate);

        rb_define_private_method(rb_cExternalFunction, "notify_v8", (VALUE(*)(...))&rb_external_function_notify_v8, 0);
        rb_define_alloc_func(rb_cExternalFunction, allocate_external_function);

        rb_define_method(rb_cSnapshot, "size", (VALUE(*)(...))&rb_snapshot_size, 0);
        rb_define_method(rb_cSnapshot, "dump", (VALUE(*)(...))&rb_snapshot_dump, 0);
        rb_define_method(rb_cSnapshot, "warmup_unsafe!", (VALUE(*)(...))&rb_snapshot_warmup_unsafe, 1);
        rb_define_private_method(rb_cSnapshot, "load", (VALUE(*)(...))&rb_snapshot_load, 1);

        rb_define_method(rb_cIsolate, "idle_notification", (VALUE(*)(...))&rb_isolate_idle_notification, 1);
        rb_define_private_method(rb_cIsolate, "init_with_snapshot",(VALUE(*)(...))&rb_isolate_init_with_snapshot, 1);

        rb_define_singleton_method(rb_cPlatform, "set_flag_as_str!", (VALUE(*)(...))&rb_platform_set_flag_as_str, 1);
    }
}
