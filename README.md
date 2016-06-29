# MiniRacer

[![Build Status](https://travis-ci.org/discourse/mini_racer.svg?branch=master)](https://travis-ci.org/discourse/mini_racer)

Minimal, modern embedded V8 for Ruby.

MiniRacer provides a minimal two way bridge between the V8 JavaScript engine and Ruby.

It was created as an alternative to the excellent [therubyracer](https://github.com/cowboyd/therubyracer). Unlike therubyracer, mini_racer only implements a minimal bridge. This reduces the surface area making upgrading v8 much simpler and exhaustive testing simpler.

MiniRacer has an adapter for [execjs](https://github.com/sstephenson/execjs) so it can be used directly with Rails projects to minify assets, run babel or compile CoffeeScript.

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

## Performance

The `bench` folder contains benchmark.

### Benchmark minification of Discourse application.js (both minified and unminified)

- MiniRacer version 0.1
- therubyracer version 0.12.2

```
$ ruby bench_uglify.rb
Benching with MiniRacer
MiniRacer minify discourse_app.js 13813.36ms
MiniRacer minify discourse_app_minified.js 18271.19ms
MiniRacer minify discourse_app.js twice (2 threads) 13587.21ms
```

```
Benching with therubyracer
MiniRacer minify discourse_app.js 151467.164ms
MiniRacer minify discourse_app_minified.js 158172.097ms
MiniRacer minify discourse_app.js twice (2 threads) - DOES NOT FINISH

Killed: 9
```

The huge performance disparity (MiniRacer is 10x faster) is due to MiniRacer running latest version of V8. In July 2016 there is a queued upgrade to therubyracer which should bring some of the perf inline.

Note how the global interpreter lock release leads to 2 threads doing the same work taking the same wall time as 1 thread.

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

###therubyracer

- https://github.com/cowboyd/therubyracer
- Most comprehensive bridge available
- Provides the ability to "eval" JavaScript
- Provides the ability to invoke Ruby code from JavaScript
- Hold refrences to JavaScript objects and methods in your Ruby code
- Hold refrences to Ruby objects and methods in JavaScript code
- Uses libv8, so installation is fast
- Supports timeouts for JavaScript execution
- Does not release global interpreter lock, so performance is constrained to a single thread
- Currently (May 2016) only supports v8 version 3.16.14 (Released approx November 2013), plans to upgrade by July 2016
- Supports execjs


###v8eval

- https://github.com/sony/v8eval
- Provides the ability to "eval" JavaScript using the latest V8 engine
- Does not depend on the [libv8](https://github.com/cowboyd/libv8) gem, installation can take 10-20 mins as V8 needs to be downloaded and compiled.
- Does not release global interpreter lock when executing JavaScript
- Does not allow you to invoke Ruby code from JavaScript
- Multi runtime support due to SWIG based bindings
- Supports a JavaScript debugger
- Does not support timeouts for JavaScript execution
- No support for execjs (can not be used with Rails uglifier and coffeescript gems)


###therubyrhino

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

