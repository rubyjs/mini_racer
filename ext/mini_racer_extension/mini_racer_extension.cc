#include <stdio.h>
#include <ruby.h>
#include <ruby/thread.h>
#include <v8.h>
#include <libplatform/libplatform.h>
#include <ruby/encoding.h>
#include <pthread.h>

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
    Isolate* isolate;
    Persistent<Context>* context;
    ArrayBufferAllocator* allocator;
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

Platform* current_platform = NULL;

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


    Local<Context> context = eval_params->context_info->context->Get(eval_params->context_info->isolate);

    // Local<Context> context = Context::New(eval_params->context_info->isolate, NULL,
//	 				  eval_params->context_info->globals->Get(eval_params->context_info->isolate));

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

static VALUE convert_v8_to_ruby(Handle<Value> &value) {

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

static Handle<Value> convert_ruby_to_v8(Isolate* isolate, VALUE value) {
    EscapableHandleScope scope(isolate);
    Local<String> result = String::NewFromUtf8(isolate, "hello");
    return scope.Escape(result);
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

void*
gvl_ruby_callback(void* data) {

    FunctionCallbackInfo<Value>* args = (FunctionCallbackInfo<Value>*)data;

    HandleScope scope(args->GetIsolate());

    Handle<External> external = Handle<External>::Cast(args->Data());


    VALUE* self_pointer = (VALUE*)(external->Value());
    VALUE callback = rb_iv_get(*self_pointer, "@callback");

    int length = args->Length();
    VALUE* ruby_args;
    if (length > 0) {
	ruby_args = ALLOC_N(VALUE, length);
    }


    for (int i = 0; i < length; i++) {
	Local<Value> value = ((*args)[i]).As<Value>();
	ruby_args[i] = convert_v8_to_ruby(value);
    }

    return NULL;

    VALUE result = rb_funcall(callback, rb_intern("call"), length, ruby_args);
    Handle<Value> v8_result = convert_ruby_to_v8(args->GetIsolate(), result);
    args->GetReturnValue().Set(v8_result);

    if (length > 0) {
	xfree(ruby_args);
    }

    return NULL;
}

static void ruby_callback(const FunctionCallbackInfo<Value>& args) {
   rb_thread_call_with_gvl(gvl_ruby_callback, (void*)(&args));
}


static VALUE rb_external_function_notify_v8(VALUE self) {

    ContextInfo* context_info;

    VALUE parent = rb_iv_get(self, "@parent");
    VALUE name = rb_iv_get(self, "@name");

    Data_Get_Struct(parent, ContextInfo, context_info);

    Locker lock(context_info->isolate);
    Isolate::Scope isolate_scope(context_info->isolate);
    HandleScope handle_scope(context_info->isolate);

    Local<String> v8_str = String::NewFromUtf8(context_info->isolate, RSTRING_PTR(name),
					      NewStringType::kNormal, (int)RSTRING_LEN(name)).ToLocalChecked();


    VALUE* self_copy;
    Data_Get_Struct(self, VALUE, self_copy);
    *self_copy = self;

    Local<ObjectTemplate> globals = context_info->globals->Get(context_info->isolate);
    Local<Value> external = External::New(context_info->isolate, self_copy);
    globals->Set(v8_str, FunctionTemplate::New(context_info->isolate, ruby_callback, external));

    return Qnil;
}

void deallocate(void * data) {
    ContextInfo* context_info = (ContextInfo*)data;
    {
	Locker lock(context_info->isolate);
	Isolate::Scope isolate_scope(context_info->isolate);
	HandleScope handle_scope(context_info->isolate);


	Local<Context> context = context_info->context->Get(context_info->isolate);
	Local<String> source = String::NewFromUtf8(context_info->isolate, "for(;;);");
 	MaybeLocal<Script> script = Script::Compile(context, source);
 	//V8::TerminateExecution(context_info->isolate);
 	//script.ToLocalChecked()->Run();
    }

    {
	context_info->context->Reset();
	delete context_info->context;
    }

    {
	//context_info->isolate->Dispose();
    }

    delete context_info->allocator;
    xfree(context_info);
}

void deallocate_external_function(void * data) {
    xfree(data);
}

VALUE allocate_external_function(VALUE klass) {
    VALUE* self = ALLOC(VALUE);
    return Data_Wrap_Struct(klass, NULL, deallocate_external_function, (void*)self);
}


VALUE allocate(VALUE klass) {
    init_v8();

    ContextInfo* context_info = ALLOC(ContextInfo);

    context_info->allocator = new ArrayBufferAllocator();

    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = context_info->allocator;
    context_info->isolate = Isolate::New(create_params);

    Locker lock(context_info->isolate);
    Isolate::Scope isolate_scope(context_info->isolate);
    HandleScope handle_scope(context_info->isolate);

    Local<ObjectTemplate> globals = ObjectTemplate::New(context_info->isolate);
    context_info->globals = new Persistent<ObjectTemplate>();
    context_info->globals->Reset(context_info->isolate, globals);

    Local<Context> context = Context::New(context_info->isolate, NULL, globals);

    context_info->context = new Persistent<Context>();
    context_info->context->Reset(context_info->isolate, context);

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
	VALUE rb_cExternalFunction = rb_define_class_under(rb_cContext, "ExternalFunction", rb_cObject);
	rb_define_method(rb_cContext, "eval",(VALUE(*)(...))&rb_context_eval, 1);
	rb_define_method(rb_cContext, "stop", (VALUE(*)(...))&rb_context_stop, 0);
	rb_define_alloc_func(rb_cContext, allocate);

	rb_define_private_method(rb_cExternalFunction, "notify_v8", (VALUE(*)(...))&rb_external_function_notify_v8, 0);
	rb_define_alloc_func(rb_cExternalFunction, allocate_external_function);
    }

}
