# MiniRacer

[![Build Status](https://travis-ci.org/discourse/mini_racer.svg?branch=master)](https://travis-ci.org/discourse/mini_racer)

Minimal, modern embedded V8 for Ruby.

MiniRacer provides a minimal two way bridge between the V8 JavaScript engine and Ruby.

It was created as an alternative to the excellent [therubyracer](https://github.com/cowboyd/therubyracer). Unlike therubyracer, mini_racer only implements a minimal bridge. This reduces the surface area making upgrading v8 much simpler and exhaustive testing simpler.

MiniRacer has an adapter for [execjs](https://github.com/rails/execjs) so it can be used directly with Rails projects to minify assets, run babel or compile CoffeeScript.

### A note about Ruby version Support

MiniRacer only supports non-EOL versions of Ruby. See [Ruby](https://www.ruby-lang.org/en/downloads) to see the list of non-EOL Rubies.

If you require support for older versions of Ruby install an older version of the gem.

## Features

### Simple eval for JavaScript

You can simply eval one or many JavaScript snippets in a shared context

```ruby
context = MiniRacer::Context.new
context.eval 'var adder = (a,b)=>a+b;'
puts context.eval 'adder(20,22)'
# => 42
```

### Attach global Ruby functions to your JavaScript context

You can attach one or many ruby proc that can be accessed via JavaScript

```ruby
context = MiniRacer::Context.new
context.attach("math.adder", proc{|a,b| a+b})
puts context.eval 'math.adder(20,22)'
# => 42
```

```ruby
context = MiniRacer::Context.new
context.attach("array_and_hash", proc{{a: 1, b: [1, {a: 1}]}})
puts context.eval 'array_and_hash()'
# => {"a" => 1, "b" => [1, {"a" => 1}]}
```

### GIL free JavaScript execution

The Ruby Global interpreter lock is released when scripts are executing

```ruby
context = MiniRacer::Context.new
Thread.new do
  sleep 1
  context.stop
end
context.eval 'while(true){}'
# => exception is raised
```

This allows you to execute multiple scripts in parallel.

### Timeout support

Contexts can specify a default timeout for scripts

```ruby
# times out after 1 second (1000 ms)
context = MiniRacer::Context.new(timeout: 1000)
context.eval 'while(true){}'
# => exception is raised
```

### Memory softlimit support

Contexts can specify a memory softlimit for scripts

```ruby
# terminates script if heap usage exceeds 200mb after V8 garbage collection has run
context = MiniRacer::Context.new(max_memory: 200000000)
context.eval 'var a = new Array(10000); while(true) {a = a.concat(new Array(10000)); print("loop " + a.length);}'
# => V8OutOfMemoryError is raised
```

### Rich debugging with "filename" support

```ruby

context = MiniRacer::Context.new
context.eval('var foo = function() {bar();}', filename: 'a/foo.js')
context.eval('bar()', filename: 'a/bar.js')

# MiniRacer::RuntimeError is raised containing the filenames you specified for evals in backtrace

```


### Threadsafe

Context usage is threadsafe

```ruby

context = MiniRacer::Context.new
context.eval('counter=0; plus=()=>counter++;')

(1..10).map do
  Thread.new {
    context.eval("plus()")
  }
end.each(&:join)

puts context.eval("counter")
# => 10

```

### Snapshots

Contexts can be created with pre-loaded snapshots:

```ruby

snapshot = MiniRacer::Snapshot.new('function hello() { return "world!"; }')

context = MiniRacer::Context.new(snapshot: snapshot)

context.eval("hello()")
# => "world!"

```

Snapshots can come in handy for example if you want your contexts to be pre-loaded for effiency. It uses [V8 snapshots](http://v8project.blogspot.com/2015/09/custom-startup-snapshots.html) under the hood; see [this link](http://v8project.blogspot.com/2015/09/custom-startup-snapshots.html) for caveats using these, in particular:

```
There is an important limitation to snapshots: they can only capture V8’s
heap. Any interaction from V8 with the outside is off-limits when creating the
snapshot. Such interactions include:

 * defining and calling API callbacks (i.e. functions created via v8::FunctionTemplate)
 * creating typed arrays, since the backing store may be allocated outside of V8

And of course, values derived from sources such as `Math.random` or `Date.now`
are fixed once the snapshot has been captured. They are no longer really random
nor reflect the current time.
```

Also note that snapshots can be warmed up, using the `warmup!` method, which allows you to call functions which are otherwise lazily compiled to get them to compile right away; any side effect of your warm up code being then dismissed. [More details on warming up here](https://github.com/electron/electron/issues/169#issuecomment-76783481), and a small example:

```ruby

snapshot = MiniRacer::Snapshot.new('var counter = 0; function hello() { counter++; return "world! "; }')

snapshot.warmup!('hello()')

context = MiniRacer::Context.new(snapshot: snapshot)

context.eval('hello()')
# => "world! 1"
context.eval('counter')
# => 1

```

### Shared isolates

By default, MiniRacer's contexts each have their own isolate (V8 runtime). For efficiency, it is possible to re-use an isolate across contexts:

```ruby

isolate = MiniRacer::Isolate.new

context1 = MiniRacer::Context.new(isolate: isolate)
context2 = MiniRacer::Context.new(isolate: isolate)

context1.isolate == context2.isolate
# => true
```

The main benefit of this is avoiding creating/destroying isolates when not needed (for example if you use a lot of contexts).

The caveat with this is that a given isolate can only execute one context at a time, so don't share isolates across contexts that you want to run concurrently.

Also, note that if you want to use shared isolates together with snapshots, you need to first create an isolate with that snapshot, and then create contexts from that isolate:

```ruby
snapshot = MiniRacer::Snapshot.new('function hello() { return "world!"; }')

isolate = MiniRacer::Isolate.new(snapshot)

context = MiniRacer::Context.new(isolate: isolate)

context.eval("hello()")
# => "world!"
```

Re-using the same isolate over and over again means V8's garbage collector will have to run to clean it up every now and then; it's possible to trigger a _blocking_ V8 GC run inside your isolate by running the `idle_notification` method on it, which takes a single argument: the amount of time (in milliseconds) that V8 should use at most for garbage collecting:

```ruby
isolate = MiniRacer::Isolate.new

context = MiniRacer::Context.new(isolate: isolate)

# do stuff with that context...

# give up to 100ms for V8 garbage collection
isolate.idle_notification(100)

```

This can come in handy to force V8 GC runs for example in between requests if you use MiniRacer on a web application.

Note that this method maps directly to [`v8::Isolate::IdleNotification`](http://bespin.cz/~ondras/html/classv8_1_1Isolate.html#aea16cbb2e351de9a3ae7be2b7cb48297), and that in particular its return value is the same (true if there is no further garbage to collect, false otherwise) and the same caveats apply, in particular that `there is no guarantee that the [call will return] within the time limit.`

### V8 Runtime flags

It is possible to set V8 Runtime flags:

```ruby
MiniRacer::Platform.set_flags! :noconcurrent_recompilation, max_inlining_levels: 10
```

This can come in handy if you want to use MiniRacer with Unicorn, which doesn't seem to alwatys appreciate V8's liberal use of threading:
```ruby
MiniRacer::Platform.set_flags! :noconcurrent_recompilation, :noconcurrent_sweeping
```

Or else to unlock experimental features in V8, for example tail recursion optimization:
```ruby
MiniRacer::Platform.set_flags! :harmony

js = <<-JS
'use strict';
var f = function f(n){
  if (n <= 0) {
    return  'foo';
  }
  return f(n - 1);
}

f(1e6);
JS

context = MiniRacer::Context.new

context.eval js
# => "foo"
```
The same code without the harmony runtime flag results in a `MiniRacer::RuntimeError: RangeError: Maximum call stack size exceeded` exception.
Please refer to http://node.green/ as a reference on other harmony features.

A list of all V8 runtime flags can be found using `node --v8-options`, or else by perusing [the V8 source code for flags (make sure to use the right version of V8)](https://github.com/v8/v8/blob/master/src/flag-definitions.h).

Note that runtime flags must be set before any other operation (e.g. creating a context, a snapshot or an isolate), otherwise an exception will be thrown.

Flags:

- :expose_gc : Will expose `gc()` which you can run in JavaScript to issue a gc
- :max_old_space_size : defaults to 1400 (megs) on 64 bit, you can restric memory usage by limiting this.
- **NOTE TO READER** our documentation could be awesome we could be properly documenting all the flags, they are hugely useful, if you feel like documenting a few more, PLEASE DO, PRs are welcome.

## Controlling memory

When hosting v8 you may want to keep track of memory usage, use #heap_stats to get memory usage:

```ruby
context = MiniRacer::Context.new(timeout: 5)
context.eval("let a='testing';")
p context.heap_stats
# {:total_physical_size=>1280640,
#  :total_heap_size_executable=>4194304,
#  :total_heap_size=>3100672,
#  :used_heap_size=>1205376,
#  :heap_size_limit=>1501560832}
```

If you wish to dispose of a context before waiting on the GC use

```ruby
context = MiniRacer::Context.new(timeout: 5)
context.eval("let a='testing';")
context.dispose
context.eval("a = 2")
# MiniRacer::ContextDisposedError

# nothing works on the context from now on, its a shell waiting to be disposed
```

### Function call

This calls the function passed as first argument:

```ruby
context = MiniRacer::Context.new
context.eval('function hello(name) { return "Hello, " + name + "!" }')
context.call('hello', 'George')
# "Hello, George!"
```

Performance is slightly better than running `eval('hello("George")')` since:

 - compilation of eval'd string is avoided
 - function arguments don't need to be converted to JSON

## Performance

The `bench` folder contains benchmark.

### Benchmark minification of Discourse application.js (both minified and unminified)

MiniRacer outperforms node when minifying assets via execjs.

- MiniRacer version 0.1.9
- node version 6.10
- therubyracer version 0.12.2

```

$ bundle exec ruby bench.rb mini_racer
Benching with mini_racer
mini_racer minify discourse_app.js 9292.72063ms
mini_racer minify discourse_app_minified.js 11799.850171ms
mini_racer minify discourse_app.js twice (2 threads) 10269.570797ms

sam@ubuntu exec_js_uglify % bundle exec ruby bench.rb node
Benching with node
node minify discourse_app.js 13302.715484ms
node minify discourse_app_minified.js 18100.761243ms
node minify discourse_app.js twice (2 threads) 14383.600207000001ms

sam@ubuntu exec_js_uglify % bundle exec ruby bench.rb therubyracer
Benching with therubyracer
therubyracer minify discourse_app.js 171683.01867700001ms
therubyracer minify discourse_app_minified.js 143138.88492ms
therubyracer minify discourse_app.js twice (2 threads) NEVER FINISH

Killed: 9
```

The huge performance disparity (MiniRacer is 10x faster) is due to MiniRacer running latest version of V8. In July 2016 there is a queued upgrade to therubyracer which should bring some of the perf inline.

Note how the global interpreter lock release leads to 2 threads doing the same work taking the same wall time as 1 thread.

As a rule MiniRacer strives to always support and depend on the latest stable version of libv8.

## Source Maps

MiniRacer can fully support source maps but must be configured correctly to do so. [Check out this example](./examples/source-map-support/) for a working implementation.

## Installation

Add this line to your application's Gemfile:

```ruby
gem 'mini_racer'
```

And then execute:

    $ bundle

Or install it yourself as:

    $ gem install mini_racer


**Note** using v8.h and compiling MiniRacer requires a C++11 standard compiler, more specifically clang 3.5 (or later) or gcc 4.8 (or later).


## Travis-ci

To install `mini-racer` you will need a version of gcc that supports C++11 (gcc 4.8) this is included by default in ubuntu trusty based images.

Travis today ships by default with a precise based image. Precise Pangolin (12.04 LTS) was first released in August 2012. Even though you can install GCC 4.8 on precise the simpler approach is to opt for the trusty based image.

Add this to your .travis.yml file:

```
- sudo: required
- dist: trusty
```

## Similar Projects

### therubyracer

- https://github.com/cowboyd/therubyracer
- Most comprehensive bridge available
- Provides the ability to "eval" JavaScript
- Provides the ability to invoke Ruby code from JavaScript
- Hold references to JavaScript objects and methods in your Ruby code
- Hold references to Ruby objects and methods in JavaScript code
- Uses libv8, so installation is fast
- Supports timeouts for JavaScript execution
- Does not release global interpreter lock, so performance is constrained to a single thread
- Currently (May 2016) only supports v8 version 3.16.14 (Released approx November 2013), plans to upgrade by July 2016
- Supports execjs


### v8eval

- https://github.com/sony/v8eval
- Provides the ability to "eval" JavaScript using the latest V8 engine
- Does not depend on the [libv8](https://github.com/cowboyd/libv8) gem, installation can take 10-20 mins as V8 needs to be downloaded and compiled.
- Does not release global interpreter lock when executing JavaScript
- Does not allow you to invoke Ruby code from JavaScript
- Multi runtime support due to SWIG based bindings
- Supports a JavaScript debugger
- Does not support timeouts for JavaScript execution
- No support for execjs (can not be used with Rails uglifier and coffeescript gems)


### therubyrhino

- https://github.com/cowboyd/therubyrhino
- API compatible with therubyracer
- Uses Mozilla's Rhino engine https://github.com/mozilla/rhino
- Requires JRuby
- Support for timeouts for JavaScript execution
- Concurrent cause .... JRuby
- Supports execjs


## Contributing

Bug reports and pull requests are welcome on GitHub at https://github.com/discourse/mini_racer. This project is intended to be a safe, welcoming space for collaboration, and contributors are expected to adhere to the [Contributor Covenant](http://contributor-covenant.org) code of conduct.


## License

The gem is available as open source under the terms of the [MIT License](http://opensource.org/licenses/MIT).

