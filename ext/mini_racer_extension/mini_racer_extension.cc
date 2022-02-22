#include <stdio.h>
#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/io.h>
#include <ruby/version.h>
#include <v8.h>
#include <v8-profiler.h>
#include <libplatform/libplatform.h>
#include <ruby/encoding.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <atomic>
#include <math.h>
#include <errno.h>

/* workaround C Ruby <= 2.x problems w/ clang in C++ mode */
#if defined(ENGINE_IS_CRUBY) && \
	RUBY_API_VERSION_MAJOR == 2 && RUBY_API_VERSION_MINOR <= 6
#  define MR_METHOD_FUNC(fn) RUBY_METHOD_FUNC(fn)
#else
#  define MR_METHOD_FUNC(fn) fn
#endif

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
    bool added_gc_cb;
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
        interrupted(false), added_gc_cb(false), pid(getpid()), refs_count(0) {
        VALUE cMutex = rb_const_get(rb_cThread, rb_intern("Mutex"));
        mutex = rb_class_new_instance(0, nullptr, cMutex);
    }

    ~IsolateInfo();

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
    size_t marshal_stackdepth;
} EvalParams;

typedef struct {
    ContextInfo *context_info;
    char *function_name;
    int argc;
    bool error;
    Local<Function> fun;
    Local<Value> *argv;
    EvalResult result;
    size_t max_memory;
    size_t marshal_stackdepth;
} FunctionCall;

class IsolateData {
public:
    enum Flag {
        // first flags are bitfield
        //  max count: sizeof(uintptr_t) * 8
        IN_GVL, // whether we are inside of ruby gvl or not
        DO_TERMINATE, // terminate as soon as possible
        MEM_SOFTLIMIT_REACHED, // we've hit the memory soft limit
        MEM_SOFTLIMIT_MAX, // maximum memory value
        MARSHAL_STACKDEPTH_REACHED, // we've hit our max stack depth
        MARSHAL_STACKDEPTH_VALUE, // current stackdepth
        MARSHAL_STACKDEPTH_MAX, // maximum stack depth during marshal
    };

    static void Init(Isolate *isolate) {
        // zero out all fields in the bitfield
        isolate->SetData(0, 0);
    }

    static uintptr_t Get(Isolate *isolate, Flag flag) {
        Bitfield u = { reinterpret_cast<uint64_t>(isolate->GetData(0)) };
        switch (flag) {
            case IN_GVL: return u.IN_GVL;
            case DO_TERMINATE: return u.DO_TERMINATE;
            case MEM_SOFTLIMIT_REACHED: return u.MEM_SOFTLIMIT_REACHED;
            case MEM_SOFTLIMIT_MAX: return static_cast<uintptr_t>(u.MEM_SOFTLIMIT_MAX) << 10;
            case MARSHAL_STACKDEPTH_REACHED: return u.MARSHAL_STACKDEPTH_REACHED;
            case MARSHAL_STACKDEPTH_VALUE: return u.MARSHAL_STACKDEPTH_VALUE;
            case MARSHAL_STACKDEPTH_MAX: return u.MARSHAL_STACKDEPTH_MAX;
        }

	// avoid compiler warning
	return u.IN_GVL;
    }

    static void Set(Isolate *isolate, Flag flag, uintptr_t value) {
        Bitfield u = { reinterpret_cast<uint64_t>(isolate->GetData(0)) };
        switch (flag) {
            case IN_GVL: u.IN_GVL = value; break;
            case DO_TERMINATE: u.DO_TERMINATE = value; break;
            case MEM_SOFTLIMIT_REACHED: u.MEM_SOFTLIMIT_REACHED = value; break;
            // drop least significant 10 bits 'store memory amount in kb'
            case MEM_SOFTLIMIT_MAX: u.MEM_SOFTLIMIT_MAX = value >> 10; break;
            case MARSHAL_STACKDEPTH_REACHED: u.MARSHAL_STACKDEPTH_REACHED = value; break;
            case MARSHAL_STACKDEPTH_VALUE: u.MARSHAL_STACKDEPTH_VALUE = value; break;
            case MARSHAL_STACKDEPTH_MAX: u.MARSHAL_STACKDEPTH_MAX = value; break;
        }
        isolate->SetData(0, reinterpret_cast<void*>(u.dataPtr));
    }

private:
    struct Bitfield {
        // WARNING: this would explode on platforms below 64 bit ptrs
        //  compiler will fail here, making it clear for them.
        //  Additionally, using the other part of the union to reinterpret the
        //  memory is undefined behavior according to spec, but is / has been stable
        //  across major compilers for decades.
        static_assert(sizeof(uintptr_t) >= sizeof(uint64_t), "mini_racer not supported on this platform. ptr size must be at least 64 bit.");
        union {
            uint64_t dataPtr: 64;
            // order in this struct matters. For cpu performance keep larger subobjects
            //  aligned on their boundaries (8 16 32), try not to straddle
            struct {
                size_t MEM_SOFTLIMIT_MAX:22;
                bool IN_GVL:1;
                bool DO_TERMINATE:1;
                bool MEM_SOFTLIMIT_REACHED:1;
                bool MARSHAL_STACKDEPTH_REACHED:1;
                uint8_t :0; // align to next 8bit bound
                size_t MARSHAL_STACKDEPTH_VALUE:10;
                uint8_t :0; // align to next 8bit bound
                size_t MARSHAL_STACKDEPTH_MAX:10;
            };
        };
    };
};

struct StackCounter {
    static void Reset(Isolate* isolate) {
        if (IsolateData::Get(isolate, IsolateData::MARSHAL_STACKDEPTH_MAX) > 0) {
            IsolateData::Set(isolate, IsolateData::MARSHAL_STACKDEPTH_VALUE, 0);
            IsolateData::Set(isolate, IsolateData::MARSHAL_STACKDEPTH_REACHED, false);
        }
    }

    static void SetMax(Isolate* isolate, size_t marshalMaxStackDepth) {
        if (marshalMaxStackDepth > 0) {
            IsolateData::Set(isolate, IsolateData::MARSHAL_STACKDEPTH_MAX, marshalMaxStackDepth);
            IsolateData::Set(isolate, IsolateData::MARSHAL_STACKDEPTH_VALUE, 0);
            IsolateData::Set(isolate, IsolateData::MARSHAL_STACKDEPTH_REACHED, false);
        }
    }

    StackCounter(Isolate* isolate) {
        this->isActive = IsolateData::Get(isolate, IsolateData::MARSHAL_STACKDEPTH_MAX) > 0;

        if (this->isActive) {
            this->isolate = isolate;
            this->IncDepth(1);
        }
    }

    bool IsTooDeep() {
        if (!this->IsActive()) {
            return false;
        }

        size_t depth = IsolateData::Get(this->isolate, IsolateData::MARSHAL_STACKDEPTH_VALUE);
        size_t maxDepth = IsolateData::Get(this->isolate, IsolateData::MARSHAL_STACKDEPTH_MAX);
        if (depth > maxDepth) {
            IsolateData::Set(this->isolate, IsolateData::MARSHAL_STACKDEPTH_REACHED, true);
            return true;
        }

        return false;
    }

    bool IsActive() {
        return this->isActive && !IsolateData::Get(this->isolate, IsolateData::DO_TERMINATE);
    }

    ~StackCounter() {
        if (this->IsActive()) {
            this->IncDepth(-1);
        }
    }

private: 
    Isolate* isolate;
    bool isActive;

    void IncDepth(int direction) {
        int inc = direction > 0 ? 1 : -1;

        size_t depth = IsolateData::Get(this->isolate, IsolateData::MARSHAL_STACKDEPTH_VALUE);

        // don't decrement past 0
        if (inc > 0 || depth > 0) {
            depth += inc;
        }

        IsolateData::Set(this->isolate, IsolateData::MARSHAL_STACKDEPTH_VALUE, depth);
    }
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

static std::unique_ptr<Platform> current_platform = NULL;
static std::mutex platform_lock;

static pthread_attr_t *thread_attr_p;
static std::atomic_int ruby_exiting(0);
static bool single_threaded = false;

static void mark_context(void *);
static void deallocate(void *);
static size_t context_memsize(const void *);
static const rb_data_type_t context_type = {
    "mini_racer/context_info",
    { mark_context, deallocate, context_memsize }
};

static void deallocate_snapshot(void *);
static size_t snapshot_memsize(const void *);
static const rb_data_type_t snapshot_type = {
    "mini_racer/snapshot_info",
    { NULL, deallocate_snapshot, snapshot_memsize }
};

static void mark_isolate(void *);
static void deallocate_isolate(void *);
static size_t isolate_memsize(const void *);
static const rb_data_type_t isolate_type = {
    "mini_racer/isolate_info",
    { mark_isolate, deallocate_isolate, isolate_memsize }
};

static VALUE rb_platform_set_flag_as_str(VALUE _klass, VALUE flag_as_str) {
    bool platform_already_initialized = false;

    Check_Type(flag_as_str, T_STRING);

    platform_lock.lock();

    if (current_platform == NULL) {
	if (!strcmp(RSTRING_PTR(flag_as_str), "--single_threaded")) {
	   single_threaded = true;
	}
        V8::SetFlagsFromString(RSTRING_PTR(flag_as_str), RSTRING_LENINT(flag_as_str));
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
	if (single_threaded) {
	    current_platform = platform::NewSingleThreadedDefaultPlatform();
	} else {
	    current_platform = platform::NewDefaultPlatform();
	}
        V8::InitializePlatform(current_platform.get());
        V8::Initialize();
    }

    platform_lock.unlock();
}

static void gc_callback(Isolate *isolate, GCType type, GCCallbackFlags flags) {
    if (IsolateData::Get(isolate, IsolateData::MEM_SOFTLIMIT_REACHED)) {
        return;
    }

    size_t softlimit = IsolateData::Get(isolate, IsolateData::MEM_SOFTLIMIT_MAX);

    HeapStatistics stats;
    isolate->GetHeapStatistics(&stats);
    size_t used = stats.used_heap_size();

    if(used > softlimit) {
        IsolateData::Set(isolate, IsolateData::MEM_SOFTLIMIT_REACHED, true);
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
            MaybeLocal<v8::Value> ml = context->Global()->Get(
                        context, String::NewFromUtf8Literal(isolate, "JSON"));

            if (ml.IsEmpty()) { // exception
                evalRes.executed = false;
            } else {
                Local<Object> JSON = ml.ToLocalChecked().As<Object>();

                Local<Function> stringify = JSON->Get(
                            context, v8::String::NewFromUtf8Literal(isolate, "stringify"))
                        .ToLocalChecked().As<Function>();

                Local<Object> object = local_value->ToObject(context).ToLocalChecked();
                const unsigned argc = 1;
                Local<Value> argv[argc] = { object };
                MaybeLocal<Value> json = stringify->Call(context, JSON, argc, argv);

                if (json.IsEmpty()) {
                    evalRes.executed = false;
                } else {
                    evalRes.json = true;
                    Persistent<Value>* persistent = new Persistent<Value>();
                    persistent->Reset(isolate, json.ToLocalChecked());
                    evalRes.value = persistent;
                }
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
                Local<String> tmp = String::NewFromUtf8Literal(isolate, "JavaScript was terminated (either by timeout or explicitly)");
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

static void*
nogvl_context_eval(void* arg) {

    EvalParams* eval_params = (EvalParams*)arg;
    EvalResult* result = eval_params->result;
    IsolateInfo* isolate_info = eval_params->context_info->isolate_info;
    Isolate* isolate = isolate_info->isolate;

    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    TryCatch trycatch(isolate);
    Local<Context> context = eval_params->context_info->context->Get(isolate);
    Context::Scope context_scope(context);
    v8::ScriptOrigin *origin = NULL;

    IsolateData::Init(isolate);

    if (eval_params->max_memory > 0) {
        IsolateData::Set(isolate, IsolateData::MEM_SOFTLIMIT_MAX, eval_params->max_memory);
        if (!isolate_info->added_gc_cb) {
            isolate->AddGCEpilogueCallback(gc_callback);
            isolate_info->added_gc_cb = true;
        }
    }

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
        if (eval_params->marshal_stackdepth > 0) {
            StackCounter::SetMax(isolate, eval_params->marshal_stackdepth);
        }

        maybe_value = parsed_script.ToLocalChecked()->Run(context);
    }

    prepare_result(maybe_value, trycatch, isolate, context, *result);

    IsolateData::Set(isolate, IsolateData::IN_GVL, true);

    return NULL;
}

static VALUE new_empty_failed_conv_obj() {
    // TODO isolate code that translates execption to ruby
    // exception so we can properly return it
    return rb_funcall(rb_cFailedV8Conversion, rb_intern("new"), 1, rb_str_new2(""));
}

// assumes isolate locking is in place
static VALUE convert_v8_to_ruby(Isolate* isolate, Local<Context> context,
                                Local<Value> value) {

    Isolate::Scope isolate_scope(isolate);
    HandleScope scope(isolate);

    StackCounter stackCounter(isolate);

    if (IsolateData::Get(isolate, IsolateData::MARSHAL_STACKDEPTH_REACHED)) {
        return Qnil;
    }

    if (stackCounter.IsTooDeep()) {
        IsolateData::Set(isolate, IsolateData::DO_TERMINATE, true);
        isolate->TerminateExecution();
        return Qnil;
    }

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
          MaybeLocal<Value> element = arr->Get(context, i);
          if (element.IsEmpty()) {
              continue;
          }
          VALUE rb_elem = convert_v8_to_ruby(isolate, context, element.ToLocalChecked());
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
             MaybeLocal<Value> key = props->Get(context, i);
             if (key.IsEmpty()) {
                return rb_funcall(rb_cFailedV8Conversion, rb_intern("new"), 1, rb_str_new2(""));
             }
             VALUE rb_key = convert_v8_to_ruby(isolate, context, key.ToLocalChecked());

             MaybeLocal<Value> prop_value = object->Get(context, key.ToLocalChecked());
             // this may have failed due to Get raising
             if (prop_value.IsEmpty() || trycatch.HasCaught()) {
                 return new_empty_failed_conv_obj();
             }

             VALUE rb_value = convert_v8_to_ruby(
                         isolate, context, prop_value.ToLocalChecked());
             rb_hash_aset(rb_hash, rb_key, rb_value);
            }
        }
        return rb_hash;
    }

    if (value->IsSymbol()) {
	v8::String::Utf8Value symbol_name(isolate,
	    Local<Symbol>::Cast(value)->Name());

	VALUE str_symbol = rb_utf8_str_new(*symbol_name, symbol_name.length());

	return rb_str_intern(str_symbol);
    }

    MaybeLocal<String> rstr_maybe = value->ToString(context);

    if (rstr_maybe.IsEmpty()) {
	return Qnil;
    } else {
	Local<String> rstr = rstr_maybe.ToLocalChecked();
	return rb_utf8_str_new(*String::Utf8Value(isolate, rstr), rstr->Utf8Length(isolate));
    }
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
	return scope.Escape(String::NewFromUtf8(isolate, RSTRING_PTR(value), NewStringType::kNormal, RSTRING_LENINT(value)).ToLocalChecked());
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
            Maybe<bool> success = array->Set(context, i, convert_ruby_to_v8(isolate, context, rb_ary_entry(value, i)));
	    (void)(success);
	}
	return scope.Escape(array);
    case T_HASH:
	object = Object::New(isolate);
	hash_as_array = rb_funcall(value, rb_intern("to_a"), 0);
	length = RARRAY_LEN(hash_as_array);
	for(i=0; i<length; i++) {
	    pair = rb_ary_entry(hash_as_array, i);
            Maybe<bool> success = object->Set(context, convert_ruby_to_v8(isolate, context, rb_ary_entry(pair, 0)),
                  convert_ruby_to_v8(isolate, context, rb_ary_entry(pair, 1)));
	    (void)(success);
	}
	return scope.Escape(object);
    case T_SYMBOL:
	value = rb_funcall(value, rb_intern("to_s"), 0);
	return scope.Escape(String::NewFromUtf8(isolate, RSTRING_PTR(value), NewStringType::kNormal, RSTRING_LENINT(value)).ToLocalChecked());
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
      return scope.Escape(String::NewFromUtf8Literal(isolate, "Undefined Conversion"));
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
static bool run_extra_code(Isolate *isolate, Local<v8::Context> context,
                    const char *utf8_source, const char *name) {
    Context::Scope context_scope(context);
    TryCatch try_catch(isolate);
    Local<String> source_string;
    if (!String::NewFromUtf8(isolate, utf8_source).ToLocal(&source_string)) {
        return false;
    }
    Local<String> resource_name =
            String::NewFromUtf8(isolate, name).ToLocalChecked();
    ScriptOrigin origin(resource_name);
    ScriptCompiler::Source source(source_string, origin);
    Local<Script> script;
    if (!ScriptCompiler::Compile(context, &source).ToLocal(&script))
        return false;
    if (script->Run(context).IsEmpty()) return false;
    return true;
}

static StartupData
create_snapshot_data_blob(const char *embedded_source = nullptr) {
    Isolate *isolate = Isolate::Allocate();

    // Optionally run a script to embed, and serialize to create a snapshot blob.
    SnapshotCreator snapshot_creator(isolate);
        {
            HandleScope scope(isolate);
        Local<v8::Context> context = v8::Context::New(isolate);
            if (embedded_source != nullptr &&
                !run_extra_code(isolate, context, embedded_source, "<embedded>")) {
           return {};
            }
            snapshot_creator.SetDefaultContext(context);
        }
    return snapshot_creator.CreateBlob(
            SnapshotCreator::FunctionCodeHandling::kClear);
    }

static
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

static VALUE rb_snapshot_size(VALUE self) {
    SnapshotInfo* snapshot_info;
    TypedData_Get_Struct(self, SnapshotInfo, &snapshot_type, snapshot_info);

    return INT2NUM(snapshot_info->raw_size);
}

static VALUE rb_snapshot_load(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    TypedData_Get_Struct(self, SnapshotInfo, &snapshot_type, snapshot_info);

    Check_Type(str, T_STRING);

    init_v8();

    StartupData startup_data = create_snapshot_data_blob(RSTRING_PTR(str));

    if (startup_data.data == NULL && startup_data.raw_size == 0) {
        rb_raise(rb_eSnapshotError, "Could not create snapshot, most likely the source is incorrect");
    }

    snapshot_info->data = startup_data.data;
    snapshot_info->raw_size = startup_data.raw_size;

    return Qnil;
}

static VALUE rb_snapshot_dump(VALUE self) {
    SnapshotInfo* snapshot_info;
    TypedData_Get_Struct(self, SnapshotInfo, &snapshot_type, snapshot_info);

    return rb_str_new(snapshot_info->data, snapshot_info->raw_size);
}

static VALUE rb_snapshot_warmup_unsafe(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    TypedData_Get_Struct(self, SnapshotInfo, &snapshot_type, snapshot_info);

    Check_Type(str, T_STRING);

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
    TypedData_Get_Struct(self, IsolateInfo, &isolate_type, isolate_info);

    init_v8();

    SnapshotInfo* snapshot_info = nullptr;
    if (!NIL_P(snapshot)) {
        TypedData_Get_Struct(snapshot, SnapshotInfo, &snapshot_type, snapshot_info);
    }

    isolate_info->init(snapshot_info);
    isolate_info->hold();

    return Qnil;
}

static VALUE rb_isolate_idle_notification(VALUE self, VALUE idle_time_in_ms) {
    IsolateInfo* isolate_info;
    TypedData_Get_Struct(self, IsolateInfo, &isolate_type, isolate_info);

    if (current_platform == NULL) return Qfalse;

    double duration = NUM2DBL(idle_time_in_ms) / 1000.0;
    double now = current_platform->MonotonicallyIncreasingTime();
    return isolate_info->isolate->IdleNotificationDeadline(now + duration) ? Qtrue : Qfalse;
}

static VALUE rb_isolate_low_memory_notification(VALUE self) {
    IsolateInfo* isolate_info;
    TypedData_Get_Struct(self, IsolateInfo, &isolate_type, isolate_info);

    if (current_platform == NULL) return Qfalse;

    isolate_info->isolate->LowMemoryNotification();
    return Qnil;
}

static VALUE rb_isolate_pump_message_loop(VALUE self) {
    IsolateInfo* isolate_info;
    TypedData_Get_Struct(self, IsolateInfo, &isolate_type, isolate_info);

    if (current_platform == NULL) return Qfalse;

    if (platform::PumpMessageLoop(current_platform.get(), isolate_info->isolate)){
	return Qtrue;
    } else {
	return Qfalse;
    }
}

static VALUE rb_context_init_unsafe(VALUE self, VALUE isolate, VALUE snap) {
    ContextInfo* context_info;
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);

    init_v8();

    IsolateInfo* isolate_info;

    if (NIL_P(isolate) || !rb_obj_is_kind_of(isolate, rb_cIsolate)) {
        isolate_info = new IsolateInfo();

        SnapshotInfo *snapshot_info = nullptr;
        if (!NIL_P(snap) && rb_obj_is_kind_of(snap, rb_cSnapshot)) {
            TypedData_Get_Struct(snap, SnapshotInfo, &snapshot_type, snapshot_info);
        }
        isolate_info->init(snapshot_info);
    } else { // given isolate or snapshot
        TypedData_Get_Struct(isolate, IsolateInfo, &isolate_type, isolate_info);
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
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);

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
            rb_raise(rb_eParseError, "%" PRIsVALUE, message);
        } else {
            rb_raise(rb_eParseError, "Unknown JavaScript Error during parse");
        }
    }

    if (!result.executed) {
        VALUE ruby_exception = rb_iv_get(self, "@current_exception");
        if (ruby_exception == Qnil) {
            bool mem_softlimit_reached = IsolateData::Get(isolate, IsolateData::MEM_SOFTLIMIT_REACHED);
            bool marshal_stack_maxdepth_reached = IsolateData::Get(isolate, IsolateData::MARSHAL_STACKDEPTH_REACHED);
            // If we were terminated or have the memory softlimit flag set
            if (marshal_stack_maxdepth_reached) {
                ruby_exception = rb_eScriptRuntimeError;
                message = rb_utf8_str_new_literal("Marshal object depth too deep. Script terminated.");
            } else if (result.terminated || mem_softlimit_reached) {
                ruby_exception = mem_softlimit_reached ? rb_eV8OutOfMemoryError : rb_eScriptTerminatedError;
            } else {
                ruby_exception = rb_eScriptRuntimeError;
            }

            // exception report about what happened
            if (TYPE(backtrace) == T_STRING) {
                rb_raise(ruby_exception, "%" PRIsVALUE, backtrace);
            } else if(TYPE(message) == T_STRING) {
                rb_raise(ruby_exception, "%" PRIsVALUE, message);
            } else {
                rb_raise(ruby_exception, "Unknown JavaScript Error during execution");
            }
        } else if (rb_obj_is_kind_of(ruby_exception, rb_eException)) {
            rb_exc_raise(ruby_exception);
        } else {
            VALUE rb_str = rb_funcall(ruby_exception, rb_intern("to_s"), 0);
            rb_raise(CLASS_OF(ruby_exception), "%" PRIsVALUE, rb_str);
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
            VALUE json_string = rb_utf8_str_new(*String::Utf8Value(isolate, rstr), rstr->Utf8Length(isolate));
            ret = rb_funcall(rb_mJSON, rb_intern("parse"), 1, json_string);
        } else {
            StackCounter::Reset(isolate);
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

    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;

    Check_Type(str, T_STRING);

    if (!NIL_P(filename)) {
        Check_Type(filename, T_STRING);
    }

    {
        Locker lock(isolate);
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Local<String> eval = String::NewFromUtf8(isolate, RSTRING_PTR(str),
                    NewStringType::kNormal, RSTRING_LENINT(str)).ToLocalChecked();

        Local<String> local_filename;

        if (filename != Qnil) {
            local_filename = String::NewFromUtf8(isolate, RSTRING_PTR(filename),
                NewStringType::kNormal, RSTRING_LENINT(filename)).ToLocalChecked();
            eval_params.filename = &local_filename;
        } else {
            eval_params.filename = NULL;
        }

        eval_params.context_info = context_info;
        eval_params.eval = &eval;
        eval_params.result = &eval_result;
        eval_params.timeout = 0;
        eval_params.max_memory = 0;
        eval_params.marshal_stackdepth = 0;
        VALUE timeout = rb_iv_get(self, "@timeout");
        if (timeout != Qnil) {
            eval_params.timeout = (useconds_t)NUM2LONG(timeout);
        }

        VALUE mem_softlimit = rb_iv_get(self, "@max_memory");
        if (mem_softlimit != Qnil) {
            eval_params.max_memory = (size_t)NUM2ULONG(mem_softlimit);
        }

        VALUE stack_depth = rb_iv_get(self, "@marshal_stack_depth");
        if (stack_depth != Qnil) {
            eval_params.marshal_stackdepth = (size_t)NUM2ULONG(stack_depth);
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

static void*
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

        self = (VALUE)(external->Value());
        callback = rb_iv_get(self, "@callback");

        parent = rb_iv_get(self, "@parent");
        if (NIL_P(parent) || !rb_obj_is_kind_of(parent, rb_cContext)) {
            return NULL;
        }

        TypedData_Get_Struct(parent, ContextInfo, &context_type, context_info);

        if (length > 0) {
            ruby_args = rb_ary_tmp_new(length);
        }

        for (int i = 0; i < length; i++) {
            Local<Value> value = ((*args)[i]).As<Value>();
            StackCounter::Reset(args->GetIsolate());
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

    if (IsolateData::Get(args->GetIsolate(), IsolateData::DO_TERMINATE)) {
        args->GetIsolate()->ThrowException(
                    String::NewFromUtf8Literal(args->GetIsolate(),
                                               "Terminated execution during transition from Ruby to JS"));
        args->GetIsolate()->TerminateExecution();
        if (length > 0) {
            rb_ary_clear(ruby_args);
        }
        return NULL;
    }

    VALUE callback_data_value = (VALUE)&callback_data;

    // TODO: use rb_vrescue2 in Ruby 2.7 and above
    result = rb_rescue2(MR_METHOD_FUNC(protected_callback), callback_data_value,
            MR_METHOD_FUNC(rescue_callback), callback_data_value, rb_eException, (VALUE)0);

    if(callback_data.failed) {
        rb_iv_set(parent, "@current_exception", result);
        args->GetIsolate()->ThrowException(String::NewFromUtf8Literal(args->GetIsolate(), "Ruby exception"));
    }
    else {
        HandleScope scope(args->GetIsolate());
        Handle<Value> v8_result = convert_ruby_to_v8(args->GetIsolate(), context_info->context->Get(args->GetIsolate()), result);
        args->GetReturnValue().Set(v8_result);
    }

    if (length > 0) {
        rb_ary_clear(ruby_args);
    }

    if (IsolateData::Get(args->GetIsolate(), IsolateData::DO_TERMINATE)) {
      args->GetIsolate()->TerminateExecution();
    }

    return NULL;
}

static void ruby_callback(const FunctionCallbackInfo<Value>& args) {
    bool has_gvl = IsolateData::Get(args.GetIsolate(), IsolateData::IN_GVL);

    if(has_gvl) {
        gvl_ruby_callback((void*)&args);
    } else {
        IsolateData::Set(args.GetIsolate(), IsolateData::IN_GVL, true);
        rb_thread_call_with_gvl(gvl_ruby_callback, (void*)(&args));
        IsolateData::Set(args.GetIsolate(), IsolateData::IN_GVL, false);
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

    TypedData_Get_Struct(parent, ContextInfo, &context_type, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;

    {
        Locker lock(isolate);
        Isolate::Scope isolate_scope(isolate);
        HandleScope handle_scope(isolate);

        Local<Context> context = context_info->context->Get(isolate);
        Context::Scope context_scope(context);

        Local<String> v8_str =
            String::NewFromUtf8(isolate, RSTRING_PTR(name),
                                NewStringType::kNormal, RSTRING_LENINT(name))
                .ToLocalChecked();

        // Note that self (rb_cExternalFunction) is a pure Ruby T_OBJECT,
        // not a T_DATA type like most other classes in this file
        Local<Value> external = External::New(isolate, (void *)self);

        if (parent_object == Qnil) {
            Maybe<bool> success = context->Global()->Set(
                        context,
                        v8_str,
                        FunctionTemplate::New(isolate, ruby_callback, external)
                            ->GetFunction(context)
                            .ToLocalChecked());
	    (void)success;

        } else {
            Local<String> eval =
                String::NewFromUtf8(isolate, RSTRING_PTR(parent_object_eval),
                                    NewStringType::kNormal,
                                    RSTRING_LENINT(parent_object_eval))
                    .ToLocalChecked();

            MaybeLocal<Script> parsed_script = Script::Compile(context, eval);
            if (parsed_script.IsEmpty()) {
            parse_error = true;
            } else {
                MaybeLocal<Value> maybe_value =
                    parsed_script.ToLocalChecked()->Run(context);
                attach_error = true;

                if (!maybe_value.IsEmpty()) {
                    Local<Value> value = maybe_value.ToLocalChecked();
                    if (value->IsObject()) {
                        Maybe<bool> success = value.As<Object>()->Set(
                                    context,
                                    v8_str,
                                    FunctionTemplate::New(isolate, ruby_callback, external)
                                        ->GetFunction(context)
                                        .ToLocalChecked());
			(void)success;
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
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);

    if (!context_info->isolate_info) {
        rb_raise(rb_eScriptRuntimeError, "Context has no Isolate available anymore");
    }

    return context_info->isolate_info->mutex;
}

IsolateInfo::~IsolateInfo() {
    if (isolate) {
        if (this->interrupted) {
            fprintf(stderr, "WARNING: V8 isolate was interrupted by Ruby, "
                            "it can not be disposed and memory will not be "
                            "reclaimed till the Ruby process exits.\n");
        } else {
            if (this->pid != getpid() && !single_threaded) {
                fprintf(stderr, "WARNING: V8 isolate was forked, "
                                "it can not be disposed and "
                                "memory will not be reclaimed "
                                "till the Ruby process exits.\n"
				"It is VERY likely your process will hang.\n"
				"If you wish to use v8 in forked environment "
				"please ensure the platform is initialized with:\n"
				"MiniRacer::Platform.set_flags! :single_threaded\n"
				);
            } else {
                isolate->Dispose();
            }
        }
        isolate = nullptr;
    }

    if (startup_data) {
        delete[] startup_data->data;
        delete startup_data;
    }

    delete allocator;
}

static void free_context_raw(void *arg) {
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
}

static void *free_context_thr(void* arg) {
    if (ruby_exiting.load() == 0) {
        free_context_raw(arg);
        xfree(arg);
    }
    return NULL;
}

// destroys everything except freeing the ContextInfo struct (see deallocate())
static void free_context(ContextInfo* context_info) {
    IsolateInfo* isolate_info = context_info->isolate_info;

    if (isolate_info && isolate_info->refs() > 1) {
        pthread_t free_context_thread;
        ContextInfo* context_info_copy = ALLOC(ContextInfo);

        context_info_copy->isolate_info = context_info->isolate_info;
        context_info_copy->context = context_info->context;
        if (pthread_create(&free_context_thread, thread_attr_p,
                           free_context_thr, (void*)context_info_copy)) {
            fprintf(stderr, "WARNING failed to release memory in MiniRacer, thread to release could not be created, process will leak memory\n");
            xfree(context_info_copy);
        }
    } else {
        free_context_raw(context_info);
    }

    context_info->context = NULL;
    context_info->isolate_info = NULL;
}

static void deallocate_isolate(void* data) {

    IsolateInfo* isolate_info = (IsolateInfo*) data;

    isolate_info->release();
}

static void mark_isolate(void* data) {
    IsolateInfo* isolate_info = (IsolateInfo*) data;
    isolate_info->mark();
}

static size_t isolate_memsize(const void *ptr) {
     const IsolateInfo *isolate_info = (const IsolateInfo *)ptr;
     return sizeof(*isolate_info);
}

static void deallocate(void* data) {
    ContextInfo* context_info = (ContextInfo*)data;

    free_context(context_info);

    xfree(data);
}

static size_t context_memsize(const void *ptr)
{
    return sizeof(ContextInfo);
}

static void mark_context(void* data) {
    ContextInfo* context_info = (ContextInfo*)data;
    if (context_info->isolate_info) {
        context_info->isolate_info->mark();
    }
}

static void deallocate_snapshot(void * data) {
    SnapshotInfo* snapshot_info = (SnapshotInfo*)data;
    delete[] snapshot_info->data;
    xfree(snapshot_info);
}

static size_t snapshot_memsize(const void *data) {
    SnapshotInfo* snapshot_info = (SnapshotInfo*)data;
    return sizeof(*snapshot_info) + snapshot_info->raw_size;
}

static VALUE allocate(VALUE klass) {
    ContextInfo* context_info;
    return TypedData_Make_Struct(klass, ContextInfo, &context_type, context_info);
}

static VALUE allocate_snapshot(VALUE klass) {
    SnapshotInfo* snapshot_info;

    return TypedData_Make_Struct(klass, SnapshotInfo, &snapshot_type, snapshot_info);
}

static VALUE allocate_isolate(VALUE klass) {
    IsolateInfo* isolate_info = new IsolateInfo();

    return TypedData_Wrap_Struct(klass, &isolate_type, isolate_info);
}

static VALUE
rb_heap_stats(VALUE self) {

    ContextInfo* context_info;
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);
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
  int err;

  FileOutputStream(int fd) : fd(fd) { err = 0; }

  virtual int GetChunkSize() {
    return 65536;
  }

  virtual void EndOfStream() {}

  virtual WriteResult WriteAsciiChunk(char* data, int size) {
    size_t len = static_cast<size_t>(size);

    while (len) {
        ssize_t w = write(fd, data, len);

        if (w > 0) {
            data += w;
            len -= w;
        } else if (w < 0) {
            err = errno;
            return kAbort;
        } else { /* w == 0, could be out-of-space */
            err = -1;
            return kAbort;
        }
    }
    return kContinue;
  }

 private:
  int fd;
};


static VALUE
rb_heap_snapshot(VALUE self, VALUE file) {

    rb_io_t *fptr;

    fptr = RFILE(file)->fptr;

    if (!fptr) return Qfalse;

    // prepare for unbuffered write(2) below:
    rb_funcall(file, rb_intern("flush"), 0);

    ContextInfo* context_info;
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);
    Isolate* isolate;
    isolate = context_info->isolate_info ? context_info->isolate_info->isolate : NULL;

    if (!isolate) return Qfalse;

    Locker lock(isolate);
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);

    HeapProfiler* heap_profiler = isolate->GetHeapProfiler();

    const HeapSnapshot* const snap = heap_profiler->TakeHeapSnapshot();

    FileOutputStream stream(fptr->fd);
    snap->Serialize(&stream, HeapSnapshot::kJSON);

    const_cast<HeapSnapshot*>(snap)->Delete();

    /* TODO: perhaps rb_sys_fail here */
    if (stream.err) return Qfalse;

    return Qtrue;
}

static VALUE
rb_context_stop(VALUE self) {

    ContextInfo* context_info;
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);

    Isolate* isolate = context_info->isolate_info->isolate;

    // flag for termination
    IsolateData::Set(isolate, IsolateData::DO_TERMINATE, true);

    isolate->TerminateExecution();
    rb_funcall(self, rb_intern("stop_attached"), 0);

    return Qnil;
}

static VALUE
rb_context_dispose(VALUE self) {

    ContextInfo* context_info;
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);

    free_context(context_info);

    return Qnil;
}

static void*
nogvl_context_call(void *args) {

    FunctionCall *call = (FunctionCall *) args;
    if (!call) {
        return NULL;
    }
    IsolateInfo *isolate_info = call->context_info->isolate_info;
    Isolate* isolate = isolate_info->isolate;

    IsolateData::Set(isolate, IsolateData::IN_GVL, false);
    IsolateData::Set(isolate, IsolateData::DO_TERMINATE, false);

    if (call->max_memory > 0) {
        IsolateData::Set(isolate, IsolateData::MEM_SOFTLIMIT_MAX, call->max_memory);
        IsolateData::Set(isolate, IsolateData::MEM_SOFTLIMIT_REACHED, false);
        if (!isolate_info->added_gc_cb) {
            isolate->AddGCEpilogueCallback(gc_callback);
            isolate_info->added_gc_cb = true;
        }
    }
 
    if (call->marshal_stackdepth > 0) {
        StackCounter::SetMax(isolate, call->marshal_stackdepth);
    }

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

    IsolateData::Set(isolate, IsolateData::IN_GVL, true);

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

    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;

    if (argc < 1) {
        rb_raise(rb_eArgError, "need at least one argument %d", argc);
    }

    VALUE function_name = argv[0];
    Check_Type(function_name, T_STRING);

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

    call.max_memory = 0;
    VALUE mem_softlimit = rb_iv_get(self, "@max_memory");
    if (mem_softlimit != Qnil) {
        unsigned long sl_int = NUM2ULONG(mem_softlimit);
        call.max_memory = (size_t)sl_int;
    }
 
    call.marshal_stackdepth = 0;
    VALUE marshal_stackdepth = rb_iv_get(self, "@marshal_stack_depth");
    if (marshal_stackdepth != Qnil) {
        unsigned long sl_int = NUM2ULONG(marshal_stackdepth);
        call.marshal_stackdepth = (size_t)sl_int;
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
        MaybeLocal<String> fname = String::NewFromUtf8(isolate, call.function_name);
        MaybeLocal<v8::Value> val;
        if (!fname.IsEmpty()) {
            val = context->Global()->Get(context, fname.ToLocalChecked());
        }

        if (val.IsEmpty() || !val.ToLocalChecked()->IsFunction()) {
            missingFunction = true;
        } else {
            Local<v8::Function> fun = Local<v8::Function>::Cast(val.ToLocalChecked());
            VALUE tmp;
            call.fun = fun;
            call.argv = (v8::Local<Value> *)RB_ALLOCV_N(void *, tmp, call.argc);
            for(int i=0; i < call.argc; i++) {
                call.argv[i] = convert_ruby_to_v8(isolate, context, call_argv[i]);
            }
            rb_thread_call_without_gvl(nogvl_context_call, &call, unblock_function, &call);
            RB_ALLOCV_END(tmp);
        }
    }

    if (missingFunction) {
    rb_raise(rb_eScriptRuntimeError, "Unknown JavaScript method invoked");
    }

    return convert_result_to_ruby(self, call.result);
}

static VALUE rb_context_create_isolate_value(VALUE self) {
    ContextInfo* context_info;
    TypedData_Get_Struct(self, ContextInfo, &context_type, context_info);
    IsolateInfo *isolate_info = context_info->isolate_info;

    if (!isolate_info) {
        return Qnil;
    }

    isolate_info->hold();
    return TypedData_Wrap_Struct(rb_cIsolate, &isolate_type, isolate_info);
}

static void set_ruby_exiting(VALUE value) {
    (void)value;

    ruby_exiting.store(1);
}

extern "C" {

    __attribute__((visibility("default"))) void Init_mini_racer_extension ( void )
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

        rb_define_method(rb_cSnapshot, "size", (VALUE(*)(...))&rb_snapshot_size, 0);
        rb_define_method(rb_cSnapshot, "dump", (VALUE(*)(...))&rb_snapshot_dump, 0);
        rb_define_method(rb_cSnapshot, "warmup_unsafe!", (VALUE(*)(...))&rb_snapshot_warmup_unsafe, 1);
        rb_define_private_method(rb_cSnapshot, "load", (VALUE(*)(...))&rb_snapshot_load, 1);

        rb_define_method(rb_cIsolate, "idle_notification", (VALUE(*)(...))&rb_isolate_idle_notification, 1);
        rb_define_method(rb_cIsolate, "low_memory_notification", (VALUE(*)(...))&rb_isolate_low_memory_notification, 0);
        rb_define_method(rb_cIsolate, "pump_message_loop", (VALUE(*)(...))&rb_isolate_pump_message_loop, 0);
        rb_define_private_method(rb_cIsolate, "init_with_snapshot",(VALUE(*)(...))&rb_isolate_init_with_snapshot, 1);

        rb_define_singleton_method(rb_cPlatform, "set_flag_as_str!", (VALUE(*)(...))&rb_platform_set_flag_as_str, 1);

        rb_set_end_proc(set_ruby_exiting, Qnil);

        static pthread_attr_t attr;
        if (pthread_attr_init(&attr) == 0) {
            if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0) {
                thread_attr_p = &attr;
            }
        }
    }
}
