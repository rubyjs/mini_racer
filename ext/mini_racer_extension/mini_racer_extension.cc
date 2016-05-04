#include <stdio.h>
#include <ruby.h>
#include <v8.h>
#include <libplatform/libplatform.h>
#include <ruby/encoding.h>

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


Platform* current_platform = NULL;
ArrayBufferAllocator* allocator = NULL;
Isolate* isolate = NULL;

static void init_v8() {
    if (current_platform == NULL) {
	V8::InitializeICU();
	current_platform = platform::CreateDefaultPlatform();
	V8::InitializePlatform(current_platform);
	V8::Initialize();
	allocator = new ArrayBufferAllocator();

	Isolate::CreateParams create_params;
	create_params.array_buffer_allocator = allocator;
	isolate = Isolate::New(create_params);

    }
}


void shutdown_v8() {

}

static VALUE rb_context_eval(VALUE self, VALUE str) {
    init_v8();

    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    Persistent<Context>* persistent_context;

    Data_Get_Struct(self, Persistent<Context>, persistent_context);
    Local<v8::Context> context = Local<Context>::New(isolate, *persistent_context);

    Context::Scope context_scope(context);

    Local<String> eval = String::NewFromUtf8(isolate, RSTRING_PTR(str),
					      NewStringType::kNormal, (int)RSTRING_LEN(str)).ToLocalChecked();

    Local<Script> script = Script::Compile(context, eval).ToLocalChecked();


    MaybeLocal<Value> initial_result = script->Run(context);

    if (initial_result.IsEmpty()) {
	// exception report about what happened
	rb_raise(rb_eStandardError, "JavaScript Error");
    }

    Local<Value> result = initial_result.ToLocalChecked();

    if (result->IsNull() || result->IsUndefined()){
	return Qnil;
    }

    if (result->IsInt32()) {
	return INT2FIX(result->Int32Value());
    }

    if (result->IsNumber()) {
	return rb_float_new(result->NumberValue());
    }

    Local<String> rstr = result->ToString();
    return rb_enc_str_new(*v8::String::Utf8Value(rstr), rstr->Utf8Length(), rb_enc_find("utf-8"));
}

void deallocate(void * context) {
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);

    Persistent<Context>* cast_context = context;

    cast_context->Reset();
    delete cast_context;
}

VALUE allocate(VALUE klass) {
    init_v8();

    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);
    Local<Context> context = Context::New(isolate);

    Persistent<Context>* persistent_context = new Persistent<Context>();
    persistent_context->Reset(isolate, context);

    return Data_Wrap_Struct(klass, NULL, deallocate, (void*)persistent_context);
}

static VALUE
rb_context_init(VALUE self) {

    return self;
}

extern "C" {

    void Init_mini_racer_extension ( void )
    {
	VALUE rb_mMiniRacer = rb_define_module("MiniRacer");
	VALUE rb_cContext = rb_define_class_under(rb_mMiniRacer, "Context", rb_cObject);
	rb_define_method(rb_cContext, "eval", rb_context_eval, 1);
	rb_define_method(rb_cContext, "initialize", rb_context_init, 0);
	rb_define_alloc_func(rb_cContext, allocate);
    }

}
