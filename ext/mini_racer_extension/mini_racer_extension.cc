#include <stdio.h>
#include <ruby.h>
#include <ruby/thread.h>
#include <v8.h>
#include <libplatform/libplatform.h>
#include <ruby/encoding.h>
#include <pthread.h>

using namespace v8;

typedef struct {
    Isolate* isolate;
    Persistent<Context>* context;
    Persistent<ObjectTemplate>* globals;
} ContextInfo;

typedef struct {
    bool parsed;
    bool executed;
    Persistent<Value>* value;
} EvalResult;

typedef struct {
    ContextInfo* context_info;
    Local<String>* eval;
    useconds_t timeout;
    EvalResult* result;
} EvalParams;

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    return data == NULL ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) { free(data); }
};


Platform* current_platform = NULL;
ArrayBufferAllocator* allocator = NULL;

static void init_v8() {
    if (current_platform == NULL) {
	V8::InitializeICU();
	current_platform = platform::CreateDefaultPlatform();
	V8::InitializePlatform(current_platform);
	V8::Initialize();
    }
}


void shutdown_v8() {

}

void* breaker(void *d) {
  EvalParams* data = (EvalParams*)d;
  usleep(data->timeout*1000);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
  V8::TerminateExecution(data->context_info->isolate);
  return NULL;
}

void*
nogvl_context_eval(void* arg) {
    EvalParams* eval_params = (EvalParams*)arg;
    EvalResult* result = eval_params->result;

    Isolate::Scope isolate_scope(eval_params->context_info->isolate);
    HandleScope handle_scope(eval_params->context_info->isolate);

    Local<v8::Context> context = Local<Context>::New(eval_params->context_info->isolate,
						     *eval_params->context_info->context);
    Context::Scope context_scope(context);

    MaybeLocal<Script> parsed_script = Script::Compile(context, *eval_params->eval);
    result->parsed = !parsed_script.IsEmpty();
    result->executed = false;
    result->value = NULL;

    if (result->parsed) {

	pthread_t breaker_thread;

	if (eval_params->timeout > 0) {
	   pthread_create(&breaker_thread, NULL, breaker, (void*)eval_params);
	}

	MaybeLocal<Value> maybe_value = parsed_script.ToLocalChecked()->Run(context);

	if (eval_params->timeout > 0) {
	    pthread_cancel(breaker_thread);
	    pthread_join(breaker_thread, NULL);
	}

	result->executed = !maybe_value.IsEmpty();

	if (result->executed) {
	    Persistent<Value>* persistent = new Persistent<Value>();
	    persistent->Reset(eval_params->context_info->isolate, maybe_value.ToLocalChecked());
	    result->value = persistent;
	}
    }

    return NULL;
}

static VALUE convert_v8_to_ruby(Local<Value> &value) {

    if (value->IsNull() || value->IsUndefined()){
	return Qnil;
    }

    if (value->IsInt32()) {
     return INT2FIX(value->Int32Value());
    }

    if (value->IsNumber()) {
      return rb_float_new(value->NumberValue());
    }

    Local<String> rstr = value->ToString();
    return rb_enc_str_new(*v8::String::Utf8Value(rstr), rstr->Utf8Length(), rb_enc_find("utf-8"));
}

static VALUE rb_context_eval(VALUE self, VALUE str) {
    EvalParams eval_params;
    EvalResult eval_result;
    ContextInfo* context_info;
    VALUE result;

    Data_Get_Struct(self, ContextInfo, context_info);

    Locker lock(context_info->isolate);
    Isolate::Scope isolate_scope(context_info->isolate);
    HandleScope handle_scope(context_info->isolate);

    Local<String> eval = String::NewFromUtf8(context_info->isolate, RSTRING_PTR(str),
					      NewStringType::kNormal, (int)RSTRING_LEN(str)).ToLocalChecked();

    eval_params.context_info = context_info;
    eval_params.eval = &eval;
    eval_params.result = &eval_result;
    eval_params.timeout = 0;
    VALUE timeout = rb_iv_get(self, "@timeout");
    if (timeout != Qnil) {
	eval_params.timeout = (useconds_t)NUM2LONG(timeout);
    }

    rb_thread_call_without_gvl(nogvl_context_eval, &eval_params, RUBY_UBF_IO, 0);

    if (!eval_result.parsed) {
	// exception report about what happened
	rb_raise(rb_eStandardError, "Error Parsing JS");
    }

    if (!eval_result.executed) {
	// exception report about what happened
	rb_raise(rb_eStandardError, "JavaScript Error");
    }

    Local<Value> tmp = Local<Value>::New(context_info->isolate, *eval_result.value);
    result = convert_v8_to_ruby(tmp);

    eval_result.value->Reset();
    delete eval_result.value;

    return result;
}

static void RubyCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    HandleScope scope(args.GetIsolate());
    printf("I WAS CALLED\n");
}

static VALUE rb_context_notify(VALUE self, VALUE str) {

    ContextInfo* context_info;

    Data_Get_Struct(self, ContextInfo, context_info);

    Locker lock(context_info->isolate);
    Isolate::Scope isolate_scope(context_info->isolate);
    HandleScope handle_scope(context_info->isolate);

    Local<String> v8_str = String::NewFromUtf8(context_info->isolate, RSTRING_PTR(str),
					      NewStringType::kNormal, (int)RSTRING_LEN(str)).ToLocalChecked();


    context_info->globals->Set(v8_str, FunctionTemplate::New(context_info->isolate, RubyCallback));

    return Qnil;
}

void deallocate(void * data) {
    ContextInfo* context_info = (ContextInfo*)data;
    {
	Locker lock(context_info->isolate);
	Isolate::Scope isolate_scope(context_info->isolate);
	HandleScope handle_scope(context_info->isolate);
	context_info->context->Reset();
    }

    {
	delete context_info->context;
	// FIXME
	//context_info->isolate->Dispose();
    }

    xfree(context_info);

}


VALUE allocate(VALUE klass) {
    init_v8();

    ContextInfo* context_info = ALLOC(ContextInfo);

    allocator = new ArrayBufferAllocator();
    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = allocator;
    context_info->isolate = Isolate::New(create_params);

    Locker lock(context_info->isolate);
    Isolate::Scope isolate_scope(context_info->isolate);
    HandleScope handle_scope(context_info->isolate);

    Local<ObjectTemplate> globals = ObjectTemplate::New(context_info->isolate);
    Persistent<ObjectTemplate> persistent_globals;
    Persistent<Context> persistent_context;

    Local<Context> context = Context::New(context_info->isolate, NULL, globals);

    persistent_globals.Reset(context_info->isolate, globals);
    persistent_context.Reset(context_info->isolate, context);

    context_info->globals = &persistent_globals;
    context_info->context = &persistent_context;

    return Data_Wrap_Struct(klass, NULL, deallocate, (void*)context_info);
}

static VALUE
rb_context_stop(VALUE self) {
    ContextInfo* context_info;
    Data_Get_Struct(self, ContextInfo, context_info);
    V8::TerminateExecution(context_info->isolate);
    return Qnil;
}

extern "C" {

    void Init_mini_racer_extension ( void )
    {
	VALUE rb_mMiniRacer = rb_define_module("MiniRacer");
	VALUE rb_cContext = rb_define_class_under(rb_mMiniRacer, "Context", rb_cObject);
	rb_define_method(rb_cContext, "eval",(VALUE(*)(...))&rb_context_eval, 1);
	rb_define_method(rb_cContext, "stop", (VALUE(*)(...))&rb_context_stop, 0);
	rb_define_alloc_func(rb_cContext, allocate);

	rb_define_private_method(rb_cContext, "notify", (VALUE(*)(...))&rb_context_notify, 1);
    }

}
