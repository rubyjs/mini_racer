# MiniRacer

[![Test](https://github.com/rubyjs/mini_racer/actions/workflows/ci.yml/badge.svg)](https://github.com/rubyjs/mini_racer/actions/workflows/ci.yml) ![Gem](https://img.shields.io/gem/v/mini_racer)

Minimal, modern embedded V8 for Ruby.

MiniRacer provides a minimal two way bridge between the V8 JavaScript engine and Ruby.

It was created as an alternative to the excellent [therubyracer](https://github.com/cowboyd/therubyracer), which is [no longer maintained](https://github.com/rubyjs/therubyracer/issues/462). Unlike therubyracer, mini_racer only implements a minimal bridge. This reduces the surface area making upgrading v8 much simpler and exhaustive testing simpler.

MiniRacer has an adapter for [execjs](https://github.com/rails/execjs) so it can be used directly with Rails projects to minify assets, run babel or compile CoffeeScript.

## Supported Ruby Versions & Troubleshooting

MiniRacer only supports non-EOL versions of Ruby. See [Ruby Maintenance Branches](https://www.ruby-lang.org/en/downloads/branches/) for the list of non-EOL Rubies. If you require support for older versions of Ruby install an older version of the gem. [TruffleRuby](https://github.com/oracle/truffleruby) is also supported.

MiniRacer **does not support**

* [Ruby built on MinGW](https://github.com/rubyjs/mini_racer/issues/252#issuecomment-1201172236), "pure windows" no Cygwin, no WSL2 (see https://github.com/rubyjs/libv8-node/issues/9)
* [JRuby](https://www.jruby.org)

If you have a problem installing MiniRacer, please consider the following steps:

* make sure you try the latest released version of `mini_racer`
* make sure you have Rubygems >= 3.2.13 and bundler >= 2.2.13 installed: `gem update --system`
* if you are using bundler
  * make sure it is actually using the latest bundler version: [`bundle update --bundler`](https://bundler.io/v2.4/man/bundle-update.1.html)
  * make sure to have `PLATFORMS` set correctly in `Gemfile.lock` via [`bundle lock --add-platform`](https://bundler.io/v2.4/man/bundle-lock.1.html#SUPPORTING-OTHER-PLATFORMS)
* make sure to recompile/reinstall `mini_racer` and `libv8-node` after OS upgrades (for example via `gem uninstall --all mini_racer libv8-node`)
* make sure you are on the latest patch/teeny version of a supported Ruby branch

## Features

### Simple eval for JavaScript

You can simply eval one or many JavaScript snippets in a shared context

```ruby
context = MiniRacer::Context.new
context.eval("var adder = (a,b)=>a+b;")
puts context.eval("adder(20,22)")
# => 42
```

### Attach global Ruby functions to your JavaScript context

You can attach one or many ruby proc that can be accessed via JavaScript

```ruby
context = MiniRacer::Context.new
context.attach("math.adder", proc{|a,b| a+b})
puts context.eval("math.adder(20,22)")
# => 42
```

```ruby
context = MiniRacer::Context.new
context.attach("array_and_hash", proc{{a: 1, b: [1, {a: 1}]}})
puts context.eval("array_and_hash()")
# => {"a" => 1, "b" => [1, {"a" => 1}]}
```

### GIL free JavaScript execution

The Ruby Global interpreter lock is released when scripts are executing:

```ruby
context = MiniRacer::Context.new
Thread.new do
  sleep 1
  context.stop
end
context.eval("while(true){}")
# => exception is raised
```

This allows you to execute multiple scripts in parallel.

### Timeout Support

Contexts can specify a default timeout for scripts

```ruby
context = MiniRacer::Context.new(timeout: 1000)
context.eval("while(true){}")
# => exception is raised after 1 second (1000 ms)
```

### Memory softlimit Support

Contexts can specify a memory softlimit for scripts

```ruby
# terminates script if heap usage exceeds 200mb after V8 garbage collection has run
context = MiniRacer::Context.new(max_memory: 200_000_000)
context.eval("var a = new Array(10000); while(true) {a = a.concat(new Array(10000)) }")
# => V8OutOfMemoryError is raised
```

### Rich Debugging with File Name in Stack Trace Support

You can provide `filename:` to `#eval` which will be used in stack traces produced by V8:

```ruby
context = MiniRacer::Context.new
context.eval("var foo = function() {bar();}", filename: "a/foo.js")
context.eval("bar()", filename: "a/bar.js")

# JavaScript at a/bar.js:1:1: ReferenceError: bar is not defined (MiniRacer::RuntimeError)
# …
```

### Fork Safety

Some Ruby web servers employ forking (for example unicorn or puma in clustered mode). V8 is not fork safe by default and sadly Ruby does not have support for fork notifications per [#5446](https://bugs.ruby-lang.org/issues/5446).

Since 0.6.1 mini_racer does support V8 single threaded platform mode which should remove most forking related issues. To enable run this before using `MiniRacer::Context`, for example in a Rails initializer:

```ruby
MiniRacer::Platform.set_flags!(:single_threaded)
```

If you want to ensure your application does not leak memory after fork either:

1. Ensure no `MiniRacer::Context` objects are created in the master process; or
2. Dispose manually of all `MiniRacer::Context` objects prior to forking

```ruby
# before fork

require "objspace"
ObjectSpace.each_object(MiniRacer::Context){|c| c.dispose}

# fork here
```

### Threadsafe

Context usage is threadsafe

```ruby
context = MiniRacer::Context.new
context.eval("counter=0; plus=()=>counter++;")

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
snapshot = MiniRacer::Snapshot.new("function hello() { return 'world!'; }")

context = MiniRacer::Context.new(snapshot: snapshot)

context.eval("hello()")
# => "world!"
```

Snapshots can come in handy for example if you want your contexts to be pre-loaded for efficiency. It uses [V8 snapshots](http://v8project.blogspot.com/2015/09/custom-startup-snapshots.html) under the hood; see [this link](http://v8project.blogspot.com/2015/09/custom-startup-snapshots.html) for caveats using these, in particular:

> There is an important limitation to snapshots: they can only capture V8’s
> heap. Any interaction from V8 with the outside is off-limits when creating the
> snapshot. Such interactions include:
>
> * defining and calling API callbacks (i.e. functions created via v8::FunctionTemplate)
> * creating typed arrays, since the backing store may be allocated outside of V8
>
> And of course, values derived from sources such as `Math.random` or `Date.now`
> are fixed once the snapshot has been captured. They are no longer really random
> nor reflect the current time.

Also note that snapshots can be warmed up, using the `warmup!` method, which allows you to call functions which are otherwise lazily compiled to get them to compile right away; any side effect of your warm up code being then dismissed. [More details on warming up here](https://github.com/electron/electron/issues/169#issuecomment-76783481), and a small example:

```ruby
snapshot = MiniRacer::Snapshot.new("var counter = 0; function hello() { counter++; return 'world! '; }")

snapshot.warmup!("hello()")

context = MiniRacer::Context.new(snapshot: snapshot)

context.eval("hello()")
# => "world! 1"
context.eval("counter")
# => 1
```

### Garbage collection

You can make the garbage collector more aggressive by defining the context with `MiniRacer::Context.new(ensure_gc_after_idle: 1000)`. Using this will ensure V8 will run a full GC using `context.low_memory_notification` 1 second after the last eval on the context. Low memory notifications ensure long living contexts use minimal amounts of memory.

### V8 Runtime flags

It is possible to set V8 Runtime flags:

```ruby
MiniRacer::Platform.set_flags! :noconcurrent_recompilation, max_inlining_levels: 10
```

This can come in handy if you want to use MiniRacer with Unicorn, which doesn't seem to always appreciate V8's liberal use of threading:

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

A list of all V8 runtime flags can be found using `node --v8-options`, or else by perusing [the V8 source code for flags (make sure to use the right version of V8)](https://github.com/v8/v8/blob/master/src/flags/flag-definitions.h).

Note that runtime flags must be set before any other operation (e.g. creating a context or a snapshot), otherwise an exception will be thrown.

Flags:

* `:expose_gc`: Will expose `gc()` which you can run in JavaScript to issue a GC run.
* `:max_old_space_size`: defaults to 1400 (megs) on 64 bit, you can restrict memory usage by limiting this.

**NOTE TO READER** our documentation could be awesome we could be properly documenting all the flags, they are hugely useful, if you feel like documenting a few more, PLEASE DO, PRs are welcome.

## Controlling memory

When hosting v8 you may want to keep track of memory usage, use `#heap_stats` to get memory usage:

```ruby
context = MiniRacer::Context.new
# use context
p context.heap_stats
# {:total_physical_size=>1280640,
#  :total_heap_size_executable=>4194304,
#  :total_heap_size=>3100672,
#  :used_heap_size=>1205376,
#  :heap_size_limit=>1501560832}
```

If you wish to dispose of a context before waiting on the GC use `#dispose`:

```ruby
context = MiniRacer::Context.new
context.eval("let a='testing';")
context.dispose
context.eval("a = 2")
# MiniRacer::ContextDisposedError

# nothing works on the context from now on, it's a shell waiting to be disposed
```

A MiniRacer context can also be dumped in a heapsnapshot file using `#write_heap_snapshot(file_or_io)`

```ruby
context = MiniRacer::Context.new
# use context
context.write_heap_snapshot("test.heapsnapshot")
```

This file can then be loaded in the "memory" tab of the [Chrome DevTools](https://developer.chrome.com/docs/devtools/memory-problems/heap-snapshots/#view_snapshots).

### Function call

This calls the function passed as first argument:

```ruby
context = MiniRacer::Context.new
context.eval("function hello(name) { return `Hello, ${name}!` }")
context.call("hello", "George")
# "Hello, George!"
```

Performance is slightly better than running `context.eval("hello('George')")` since:

* compilation of eval'd string is avoided
* function arguments don't need to be converted to JSON

## Performance

The `bench` folder contains benchmark.

### Benchmark minification of Discourse application.js (both minified and non-minified)

MiniRacer outperforms node when minifying assets via execjs.

* MiniRacer version 0.1.9
* node version 6.10
* therubyracer version 0.12.2

```terminal
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
gem "mini_racer"
```

And then execute:

```terminal
$ bundle

Or install it yourself as:

```terminal
$ gem install mini_racer
```

**Note** using v8.h and compiling MiniRacer requires a C++20 capable compiler.
gcc >= 12.2 and Xcode >= 13 are, at the time of writing, known to work.

## Similar Projects

### therubyracer

* https://github.com/cowboyd/therubyracer
* Most comprehensive bridge available
* Provides the ability to "eval" JavaScript
* Provides the ability to invoke Ruby code from JavaScript
* Hold references to JavaScript objects and methods in your Ruby code
* Hold references to Ruby objects and methods in JavaScript code
* Uses libv8, so installation is fast
* Supports timeouts for JavaScript execution
* Does not release global interpreter lock, so performance is constrained to a single thread
* Currently (May 2016) only supports v8 version 3.16.14 (Released approx November 2013), plans to upgrade by July 2016
* Supports execjs

### v8eval

* https://github.com/sony/v8eval
* Provides the ability to "eval" JavaScript using the latest V8 engine
* Does not depend on the [libv8](https://github.com/cowboyd/libv8) gem, installation can take 10-20 mins as V8 needs to be downloaded and compiled.
* Does not release global interpreter lock when executing JavaScript
* Does not allow you to invoke Ruby code from JavaScript
* Multi runtime support due to SWIG based bindings
* Supports a JavaScript debugger
* Does not support timeouts for JavaScript execution
* No support for execjs (can not be used with Rails uglifier and coffeescript gems)

### therubyrhino

* https://github.com/cowboyd/therubyrhino
* API compatible with therubyracer
* Uses Mozilla's Rhino engine https://github.com/mozilla/rhino
* Requires JRuby
* Support for timeouts for JavaScript execution
* Concurrent cause .... JRuby
* Supports execjs

## Contributing

Bug reports and pull requests are welcome on GitHub at https://github.com/rubyjs/mini_racer. This project is intended to be a safe, welcoming space for collaboration, and contributors are expected to adhere to the [Contributor Covenant](http://contributor-covenant.org) code of conduct.

## License

The gem is available as open source under the terms of the [MIT License](http://opensource.org/licenses/MIT).
