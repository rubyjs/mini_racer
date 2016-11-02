#include <stdio.h>
#include <ruby.h>
#include <ruby/thread.h>
#include <v8.h>
#include <libplatform/libplatform.h>
#include <ruby/encoding.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>

using namespace v8;

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) { free(data); }
};

typedef struct {
    const char* data;
    int raw_size;
} SnapshotInfo;

typedef struct {
    Isolate* isolate;
    ArrayBufferAllocator* allocator;
    StartupData* startup_data;
    bool interrupted;
    pid_t pid;

    // how many references to this isolate exist
    // we can't rely on Ruby's GC for this, because when destroying
    // objects, Ruby will destroy ruby objects first, then call the
    // extenstion's deallocators. In this case, that means it would
    // call `deallocate_isolate` _before_ `deallocate`, causing a segfault
    int refs_count;
} IsolateInfo;

typedef struct {
    IsolateInfo* isolate_info;
    Persistent<Context>* context;
} ContextInfo;

typedef struct {
    bool parsed;
    bool executed;
    bool terminated;
    Persistent<Value>* value;
    Persistent<Value>* message;
    Persistent<Value>* backtrace;
} EvalResult;

typedef struct {
    ContextInfo* context_info;
    Local<String>* eval;
    useconds_t timeout;
    EvalResult* result;
} EvalParams;

static VALUE rb_eScriptTerminatedError;
static VALUE rb_eParseError;
static VALUE rb_eScriptRuntimeError;
static VALUE rb_cJavaScriptFunction;
static VALUE rb_eSnapshotError;
static VALUE rb_ePlatformAlreadyInitializedError;

static VALUE rb_cFailedV8Conversion;
static VALUE rb_cDateTime = Qnil;

static Platform* current_platform = NULL;
static std::mutex platform_lock;

static VALUE rb_platform_set_flag_as_str(VALUE _klass, VALUE flag_as_str) {
    bool platform_already_initialized = false;

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

    // in gvl flag
    isolate->SetData(0, (void*)false);
    // terminate ASAP
    isolate->SetData(1, (void*)false);

    MaybeLocal<Script> parsed_script = Script::Compile(context, *eval_params->eval);
    result->parsed = !parsed_script.IsEmpty();
    result->executed = false;
    result->terminated = false;
    result->value = NULL;

    if (!result->parsed) {
	result->message = new Persistent<Value>();
	result->message->Reset(isolate, trycatch.Exception());
    } else {

	MaybeLocal<Value> maybe_value = parsed_script.ToLocalChecked()->Run(context);

	result->executed = !maybe_value.IsEmpty();

	if (result->executed) {
	    Persistent<Value>* persistent = new Persistent<Value>();
	    persistent->Reset(isolate, maybe_value.ToLocalChecked());
	    result->value = persistent;
	}
    }

    if (!result->executed || !result->parsed) {
	if (trycatch.HasCaught()) {
	    if (!trycatch.Exception()->IsNull()) {
		result->message = new Persistent<Value>();
		Local<Message> message = trycatch.Message();
		char buf[1000];
		int len;
		len = snprintf(buf, sizeof(buf), "%s at %s:%i:%i", *String::Utf8Value(message->Get()),
			           *String::Utf8Value(message->GetScriptResourceName()->ToString()),
				    message->GetLineNumber(),
				    message->GetStartColumn());

		Local<String> v8_message = String::NewFromUtf8(isolate, buf, NewStringType::kNormal, (int)len).ToLocalChecked();
		result->message->Reset(isolate, v8_message);
	    } else if(trycatch.HasTerminated()) {


		result->terminated = true;
		result->message = new Persistent<Value>();
		Local<String> tmp = String::NewFromUtf8(isolate, "JavaScript was terminated (either by timeout or explicitly)");
		result->message->Reset(isolate, tmp);
	    }
	    if (!trycatch.StackTrace().IsEmpty()) {
		result->backtrace = new Persistent<Value>();
		result->backtrace->Reset(isolate, trycatch.StackTrace()->ToString());
	    }
	}
    }

    isolate->SetData(0, (void*)true);


    return NULL;
}

static VALUE convert_v8_to_ruby(Isolate* isolate, Handle<Value> &value) {

    HandleScope scope(isolate);

    if (value->IsNull() || value->IsUndefined()){
	return Qnil;
    }

    if (value->IsInt32()) {
     return INT2FIX(value->Int32Value());
    }

    if (value->IsNumber()) {
      return rb_float_new(value->NumberValue());
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
	  VALUE rb_elem = convert_v8_to_ruby(isolate, element);
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

	TryCatch trycatch(isolate);

	VALUE rb_hash = rb_hash_new();
	Local<Context> context = Context::New(isolate);
	Local<Object> object = value->ToObject();
	MaybeLocal<Array> maybe_props = object->GetOwnPropertyNames(context);
	if (!maybe_props.IsEmpty()) {
	    Local<Array> props = maybe_props.ToLocalChecked();
	    for(uint32_t i=0; i < props->Length(); i++) {
	     Local<Value> key = props->Get(i);
	     VALUE rb_key = convert_v8_to_ruby(isolate, key);
	     Local<Value> value = object->Get(key);
	     // this may have failed due to Get raising

	     if (trycatch.HasCaught()) {
		 // TODO isolate code that translates execption to ruby
		 // exception so we can properly return it
		 return rb_funcall(rb_cFailedV8Conversion, rb_intern("new"), 1, rb_str_new2(""));
	     }

	     VALUE rb_value = convert_v8_to_ruby(isolate, value);
	     rb_hash_aset(rb_hash, rb_key, rb_value);
	    }
	}
	return rb_hash;
    }

    Local<String> rstr = value->ToString();
    return rb_enc_str_new(*String::Utf8Value(rstr), rstr->Utf8Length(), rb_enc_find("utf-8"));
}

static Handle<Value> convert_ruby_to_v8(Isolate* isolate, VALUE value) {
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
	    array->Set(i, convert_ruby_to_v8(isolate, rb_ary_entry(value, i)));
	}
	return scope.Escape(array);
    case T_HASH:
	object = Object::New(isolate);
	hash_as_array = rb_funcall(value, rb_intern("to_a"), 0);
	length = RARRAY_LEN(hash_as_array);
	for(i=0; i<length; i++) {
	    pair = rb_ary_entry(hash_as_array, i);
	    object->Set(convert_ruby_to_v8(isolate, rb_ary_entry(pair, 0)),
			convert_ruby_to_v8(isolate, rb_ary_entry(pair, 1)));
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
            return scope.Escape(Date::New(isolate, NUM2DBL(value) * 1000));
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

static VALUE rb_snapshot_size(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    return INT2NUM(snapshot_info->raw_size);
}

static VALUE rb_snapshot_load(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    init_v8();

    StartupData startup_data = V8::CreateSnapshotDataBlob(RSTRING_PTR(str));

    if (startup_data.data == NULL && startup_data.raw_size == 0) {
        rb_raise(rb_eSnapshotError, "Could not create snapshot, most likely the source is incorrect");
    }

    snapshot_info->data = startup_data.data;
    snapshot_info->raw_size = startup_data.raw_size;

    return Qnil;
}

static VALUE rb_snapshot_warmup(VALUE self, VALUE str) {
    SnapshotInfo* snapshot_info;
    Data_Get_Struct(self, SnapshotInfo, snapshot_info);

    init_v8();

    StartupData cold_startup_data = {snapshot_info->data, snapshot_info->raw_size};
    StartupData warm_startup_data = V8::WarmUpSnapshotDataBlob(cold_startup_data, RSTRING_PTR(str));

    if (warm_startup_data.data == NULL && warm_startup_data.raw_size == 0) {
        rb_raise(rb_eSnapshotError, "Could not warm up snapshot, most likely the source is incorrect");
    } else {
        delete[] snapshot_info->data;

        snapshot_info->data = warm_startup_data.data;
        snapshot_info->raw_size = warm_startup_data.raw_size;
    }

    return self;
}

static VALUE rb_isolate_init_with_snapshot(VALUE self, VALUE snapshot) {
    IsolateInfo* isolate_info;
    Data_Get_Struct(self, IsolateInfo, isolate_info);

    init_v8();

    isolate_info->allocator = new ArrayBufferAllocator();
    isolate_info->interrupted = false;
    isolate_info->refs_count = 1;

    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = isolate_info->allocator;

    StartupData* startup_data = NULL;
    if (!NIL_P(snapshot)) {
        SnapshotInfo* snapshot_info;
        Data_Get_Struct(snapshot, SnapshotInfo, snapshot_info);

        int raw_size = snapshot_info->raw_size;
        char* data = new char[raw_size];
        memcpy(data, snapshot_info->data, sizeof(char) * raw_size);

        startup_data = new StartupData;
        startup_data->data = data;
        startup_data->raw_size = raw_size;

        create_params.snapshot_blob = startup_data;
    }

    isolate_info->startup_data = startup_data;
    isolate_info->isolate = Isolate::New(create_params);

    return Qnil;
}

static VALUE rb_isolate_idle_notification(VALUE self, VALUE idle_time_in_ms) {
    IsolateInfo* isolate_info;
    Data_Get_Struct(self, IsolateInfo, isolate_info);

    return isolate_info->isolate->IdleNotification(NUM2INT(idle_time_in_ms)) ? Qtrue : Qfalse;
}

static VALUE rb_context_init_with_isolate(VALUE self, VALUE isolate) {
    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    init_v8();

    IsolateInfo* isolate_info;
    Data_Get_Struct(isolate, IsolateInfo, isolate_info);

    context_info->isolate_info = isolate_info;
    isolate_info->refs_count++;

    {
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

static VALUE rb_context_eval_unsafe(VALUE self, VALUE str) {

    EvalParams eval_params;
    EvalResult eval_result;
    ContextInfo* context_info;
    VALUE result;

    VALUE message = Qnil;
    VALUE backtrace = Qnil;

    Data_Get_Struct(self, ContextInfo, context_info);
    Isolate* isolate = context_info->isolate_info->isolate;



    {
	Locker lock(isolate);
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);

	Local<String> eval = String::NewFromUtf8(isolate, RSTRING_PTR(str),
						  NewStringType::kNormal, (int)RSTRING_LEN(str)).ToLocalChecked();

	eval_params.context_info = context_info;
	eval_params.eval = &eval;
	eval_params.result = &eval_result;
	eval_params.timeout = 0;
	VALUE timeout = rb_iv_get(self, "@timeout");
	if (timeout != Qnil) {
	    eval_params.timeout = (useconds_t)NUM2LONG(timeout);
	}

	eval_result.message = NULL;
	eval_result.backtrace = NULL;

	rb_thread_call_without_gvl(nogvl_context_eval, &eval_params, unblock_eval, &eval_params);

	if (eval_result.message != NULL) {
	    Local<Value> tmp = Local<Value>::New(isolate, *eval_result.message);
	    message = convert_v8_to_ruby(isolate, tmp);
	    eval_result.message->Reset();
	    delete eval_result.message;
	}

	if (eval_result.backtrace != NULL) {
	    Local<Value> tmp = Local<Value>::New(isolate, *eval_result.backtrace);
	    backtrace = convert_v8_to_ruby(isolate, tmp);
	    eval_result.backtrace->Reset();
	    delete eval_result.backtrace;
	}
    }

    // NOTE: this is very important, we can not do an rb_raise from within
    // a v8 scope, if we do the scope is never cleaned up properly and we leak
    if (!eval_result.parsed) {
	if(TYPE(message) == T_STRING) {
	    rb_raise(rb_eParseError, "%s", RSTRING_PTR(message));
	} else {
	    rb_raise(rb_eParseError, "Unknown JavaScript Error during parse");
	}
    }

    if (!eval_result.executed) {
	VALUE ruby_exception = rb_iv_get(self, "@current_exception");
	if (ruby_exception == Qnil) {
	    ruby_exception = eval_result.terminated ? rb_eScriptTerminatedError : rb_eScriptRuntimeError;
	    // exception report about what happened
	    if(TYPE(backtrace) == T_STRING) {
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

    // New scope for return value, must release GVL which
    {
	Locker lock(isolate);
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);

	Local<Value> tmp = Local<Value>::New(isolate, *eval_result.value);
	result = convert_v8_to_ruby(isolate, tmp);

	eval_result.value->Reset();
	delete eval_result.value;
    }

    if (rb_funcall(result, rb_intern("class"), 0) == rb_cFailedV8Conversion) {
	// TODO try to recover stack trace from the conversion error
	rb_raise(rb_eScriptRuntimeError, "Error converting JS object to Ruby object");
    }


    return result;
}

typedef struct {
    VALUE callback;
    int length;
    VALUE* args;
    bool failed;
} protected_callback_data;

static
VALUE protected_callback(VALUE rdata) {
    protected_callback_data* data = (protected_callback_data*)rdata;
    VALUE result;

    if (data->length > 0) {
	result = rb_funcall2(data->callback, rb_intern("call"), data->length, data->args);
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
    VALUE* ruby_args = NULL;
    int length = args->Length();
    VALUE callback;
    VALUE result;
    VALUE self;

    {
	HandleScope scope(args->GetIsolate());
	Handle<External> external = Handle<External>::Cast(args->Data());

	VALUE* self_pointer = (VALUE*)(external->Value());
	self = *self_pointer;
	callback = rb_iv_get(self, "@callback");

	if (length > 0) {
	    ruby_args = ALLOC_N(VALUE, length);
	}


	for (int i = 0; i < length; i++) {
	    Local<Value> value = ((*args)[i]).As<Value>();
	    ruby_args[i] = convert_v8_to_ruby(args->GetIsolate(), value);
	}
    }

    // may raise exception stay clear of handle scope
    protected_callback_data callback_data;
    callback_data.length = length;
    callback_data.callback = callback;
    callback_data.args = ruby_args;
    callback_data.failed = false;

    if ((bool)args->GetIsolate()->GetData(1) == true) {
	args->GetIsolate()->ThrowException(String::NewFromUtf8(args->GetIsolate(), "Terminated execution during tansition from Ruby to JS"));
	V8::TerminateExecution(args->GetIsolate());
	return NULL;
    }

    result = rb_rescue2((VALUE(*)(...))&protected_callback, (VALUE)(&callback_data),
			(VALUE(*)(...))&rescue_callback, (VALUE)(&callback_data), rb_eException, (VALUE)0);

    if(callback_data.failed) {
	VALUE parent = rb_iv_get(self, "@parent");
	rb_iv_set(parent, "@current_exception", result);
	args->GetIsolate()->ThrowException(String::NewFromUtf8(args->GetIsolate(), "Ruby exception"));
    }
    else {
	HandleScope scope(args->GetIsolate());
	Handle<Value> v8_result = convert_ruby_to_v8(args->GetIsolate(), result);
	args->GetReturnValue().Set(v8_result);
    }

    if (length > 0) {
	xfree(ruby_args);
    }

    if ((bool)args->GetIsolate()->GetData(1) == true) {
      Isolate* isolate = args->GetIsolate();
      V8::TerminateExecution(isolate);
    }

    return NULL;
}

static void ruby_callback(const FunctionCallbackInfo<Value>& args) {

    bool has_gvl = (bool)args.GetIsolate()->GetData(0);

    if(has_gvl) {
	gvl_ruby_callback((void*)&args);
    } else {
	rb_thread_call_with_gvl(gvl_ruby_callback, (void*)(&args));
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
	rb_raise(rb_eParseError, "Invalid object %s", RSTRING_PTR(parent_object));
    }

    if (attach_error) {
	rb_raise(rb_eParseError, "Was expecting %s to be an object", RSTRING_PTR(parent_object));
    }

    return Qnil;
}

void maybe_free_isolate_info(IsolateInfo* isolate_info) {
    // an isolate can only be freed if no Isolate or Context (ruby) object
    // still need it
    if (isolate_info == NULL || isolate_info->refs_count > 0) {
        return;
    }

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
    xfree(isolate_info);
}

void deallocate_isolate(void* data) {
    IsolateInfo* isolate_info = (IsolateInfo*) data;

    isolate_info->refs_count--;

    maybe_free_isolate_info(isolate_info);
}

void deallocate(void* data) {
    ContextInfo* context_info = (ContextInfo*)data;
    IsolateInfo* isolate_info = context_info->isolate_info;

    if (context_info->context && isolate_info && isolate_info->isolate) {
        Locker lock(isolate_info->isolate);
        v8::Isolate::Scope isolate_scope(isolate_info->isolate);
        context_info->context->Reset();
        delete context_info->context;
    }

    if (isolate_info) {
        isolate_info->refs_count--;
        maybe_free_isolate_info(isolate_info);
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

    return Data_Wrap_Struct(klass, NULL, deallocate, (void*)context_info);
}

VALUE allocate_snapshot(VALUE klass) {
    SnapshotInfo* snapshot_info = ALLOC(SnapshotInfo);
    snapshot_info->data = NULL;
    snapshot_info->raw_size = 0;

    return Data_Wrap_Struct(klass, NULL, deallocate_snapshot, (void*)snapshot_info);
}

VALUE allocate_isolate(VALUE klass) {
    IsolateInfo* isolate_info = ALLOC(IsolateInfo);

    isolate_info->isolate = NULL;
    isolate_info->allocator = NULL;
    isolate_info->startup_data = NULL;
    isolate_info->interrupted = false;
    isolate_info->refs_count = 0;
    isolate_info->pid = getpid();

    return Data_Wrap_Struct(klass, NULL, deallocate_isolate, (void*)isolate_info);
}

static VALUE
rb_context_stop(VALUE self) {

    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);

    Isolate* isolate = context_info->isolate_info->isolate;

    // flag for termination
    isolate->SetData(1, (void*)true);

    V8::TerminateExecution(isolate);
    rb_funcall(self, rb_intern("stop_attached"), 0);

    return Qnil;
}

extern "C" {

    void Init_mini_racer_extension ( void )
    {
	VALUE rb_mMiniRacer = rb_define_module("MiniRacer");
	VALUE rb_cContext = rb_define_class_under(rb_mMiniRacer, "Context", rb_cObject);
	VALUE rb_cSnapshot = rb_define_class_under(rb_mMiniRacer, "Snapshot", rb_cObject);
	VALUE rb_cIsolate = rb_define_class_under(rb_mMiniRacer, "Isolate", rb_cObject);
	VALUE rb_cPlatform = rb_define_class_under(rb_mMiniRacer, "Platform", rb_cObject);

	VALUE rb_eEvalError = rb_define_class_under(rb_mMiniRacer, "EvalError", rb_eStandardError);
	rb_eScriptTerminatedError = rb_define_class_under(rb_mMiniRacer, "ScriptTerminatedError", rb_eEvalError);
	rb_eParseError = rb_define_class_under(rb_mMiniRacer, "ParseError", rb_eEvalError);
	rb_eScriptRuntimeError = rb_define_class_under(rb_mMiniRacer, "RuntimeError", rb_eEvalError);
	rb_cJavaScriptFunction = rb_define_class_under(rb_mMiniRacer, "JavaScriptFunction", rb_cObject);
	rb_eSnapshotError = rb_define_class_under(rb_mMiniRacer, "SnapshotError", rb_eStandardError);
	rb_ePlatformAlreadyInitializedError = rb_define_class_under(rb_mMiniRacer, "PlatformAlreadyInitialized", rb_eStandardError);
	rb_cFailedV8Conversion = rb_define_class_under(rb_mMiniRacer, "FailedV8Conversion", rb_cObject);

	VALUE rb_cExternalFunction = rb_define_class_under(rb_cContext, "ExternalFunction", rb_cObject);
	rb_define_method(rb_cContext, "stop", (VALUE(*)(...))&rb_context_stop, 0);
	rb_define_alloc_func(rb_cContext, allocate);
	rb_define_alloc_func(rb_cSnapshot, allocate_snapshot);
	rb_define_alloc_func(rb_cIsolate, allocate_isolate);

	rb_define_private_method(rb_cContext, "eval_unsafe",(VALUE(*)(...))&rb_context_eval_unsafe, 1);
	rb_define_private_method(rb_cContext, "init_with_isolate",(VALUE(*)(...))&rb_context_init_with_isolate, 1);
	rb_define_private_method(rb_cExternalFunction, "notify_v8", (VALUE(*)(...))&rb_external_function_notify_v8, 0);
	rb_define_alloc_func(rb_cExternalFunction, allocate_external_function);

	rb_define_method(rb_cSnapshot, "size", (VALUE(*)(...))&rb_snapshot_size, 0);
	rb_define_method(rb_cSnapshot, "warmup!", (VALUE(*)(...))&rb_snapshot_warmup, 1);
	rb_define_private_method(rb_cSnapshot, "load", (VALUE(*)(...))&rb_snapshot_load, 1);

	rb_define_method(rb_cIsolate, "idle_notification", (VALUE(*)(...))&rb_isolate_idle_notification, 1);
	rb_define_private_method(rb_cIsolate, "init_with_snapshot",(VALUE(*)(...))&rb_isolate_init_with_snapshot, 1);

	rb_define_singleton_method(rb_cPlatform, "set_flag_as_str!", (VALUE(*)(...))&rb_platform_set_flag_as_str, 1);
    }

}
