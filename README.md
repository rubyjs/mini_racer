# MiniRacer

Minimal, modern embedded V8 for Ruby.

MiniRacer provides a minimal two way bridge between the V8 JavaScript engine and Ruby.

It was created as an alternative to the excellent [therubyracer](https://github.com/cowboyd/therubyracer). Unlike therubyracer, mini_racer only implements a minimal bridge. This reduces the surface area making upgrading v8 much simpler and exahustive testing simpler.

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
context.attach("adder", proc{|a,b| a+b})
puts context.eval 'adder(20,22)'
# => 42
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

## Performance

The `bench` folder contains benchmark.

### Benchmark minification of Discourse application.js (both minified and unminified)

- MiniRacer version 0.1 (unreleased)
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

Bug reports and pull requests are welcome on GitHub at https://github.com/[USERNAME]/mini_racer. This project is intended to be a safe, welcoming space for collaboration, and contributors are expected to adhere to the [Contributor Covenant](http://contributor-covenant.org) code of conduct.


## License

The gem is available as open source under the terms of the [MIT License](http://opensource.org/licenses/MIT).

