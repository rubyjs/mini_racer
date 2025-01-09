# frozen_string_literal: true

require "securerandom"
require "date"
require "test_helper"

class MiniRacerTest < Minitest::Test
  # see `test_platform_set_flags_works` below
  MiniRacer::Platform.set_flags! :use_strict

  # --stress_snapshot works around a bogus debug assert in V8
  # that terminates the process with the following error:
  #
  #	Fatal error in ../deps/v8/src/heap/read-only-spaces.cc, line 70
  #	Check failed: read_only_blob_checksum_ == snapshot_checksum (<unprintable> vs. 1099685679).
  MiniRacer::Platform.set_flags! :stress_snapshot

  def test_locale_mx
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not have all js timezone by default"
    end
    val =
      MiniRacer::Context.new.eval(
        "new Date('April 28 2021').toLocaleDateString('es-MX');"
      )
    assert_equal "28/4/2021", val
  end

  def test_locale_us
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not have all js timezone by default"
    end
    val =
      MiniRacer::Context.new.eval(
        "new Date('April 28 2021').toLocaleDateString('en-US');"
      )
    assert_equal "4/28/2021", val
  end

  def test_locale_fr
    # TODO: this causes a segfault on Linux

    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not have all js timezone by default"
    end
    val =
      MiniRacer::Context.new.eval(
        "new Date('April 28 2021').toLocaleDateString('fr-FR');"
      )
    assert_equal "28/04/2021", val
  end

  def test_segfault
    skip "running this test is very slow"
    # 5000.times do
    #   GC.start
    #   context = MiniRacer::Context.new(timeout: 5)
    #   context.attach("echo", proc{|msg| msg.to_sym.to_s})
    #   assert_raises(MiniRacer::EvalError) do
    #     context.eval("while(true) echo('foo');")
    #   end
    # end
  end

  def test_that_it_has_a_version_number
    refute_nil ::MiniRacer::VERSION
  end

  def test_types
    context = MiniRacer::Context.new
    assert_equal 2, context.eval("2")
    assert_equal "two", context.eval('"two"')
    assert_equal 2.1, context.eval("2.1")
    assert_equal true, context.eval("true")
    assert_equal false, context.eval("false")
    assert_nil context.eval("null")
    assert_nil context.eval("undefined")
  end

  def test_compile_nil_context
    context = MiniRacer::Context.new
    assert_raises(TypeError) { assert_equal 2, context.eval(nil) }
  end

  def test_array
    context = MiniRacer::Context.new
    assert_equal [1, "two"], context.eval('[1,"two"]')
  end

  def test_object
    context = MiniRacer::Context.new
    # remember JavaScript is quirky {"1" : 1} magically turns to {1: 1} cause magic
    assert_equal(
      { "1" => 2, "two" => "two" },
      context.eval('var a={"1" : 2, "two" : "two"}; a')
    )
  end

  def test_it_returns_runtime_error
    context = MiniRacer::Context.new
    exp = nil

    begin
      context.eval("var foo=function(){boom;}; foo()")
    rescue => e
      exp = e
    end

    assert_equal MiniRacer::RuntimeError, exp.class

    assert_match(/boom/, exp.message)
    assert_match(/foo/, exp.backtrace[0])
    assert_match(/mini_racer/, exp.backtrace[2])

    # context should not be dead
    assert_equal 2, context.eval("1+1")
  end

  def test_it_can_stop
    context = MiniRacer::Context.new
    exp = nil

    begin
      Thread.new do
        sleep 0.01
        context.stop
      end
      context.eval("while(true){}")
    rescue => e
      exp = e
    end

    assert_equal MiniRacer::ScriptTerminatedError, exp.class
    assert_match(/terminated/, exp.message)
  end

  def test_it_can_timeout_during_serialization
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby needs a fix for timing out during translation"
    end
    context = MiniRacer::Context.new(timeout: 500)

    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval "var a = {get a(){ while(true); }}; a"
    end
  end

  def test_it_can_automatically_time_out_context
    # 2 millisecs is a very short timeout but we don't want test running forever
    context = MiniRacer::Context.new(timeout: 2)
    assert_raises { context.eval("while(true){}") }
  end

  def test_returns_javascript_function
    context = MiniRacer::Context.new
    assert_same MiniRacer::JavaScriptFunction,
                context.eval("var a = function(){}; a").class
  end

  def test_it_handles_malformed_js
    context = MiniRacer::Context.new
    assert_raises MiniRacer::ParseError do
      context.eval("I am not JavaScript {")
    end
  end

  def test_it_handles_malformed_js_with_backtrace
    context = MiniRacer::Context.new
    assert_raises MiniRacer::ParseError do
      begin
        context.eval("var i;\ni=2;\nI am not JavaScript {")
      rescue => e
        # I <parse error> am not
        assert_match(/3:2/, e.message)
        raise
      end
    end
  end

  def test_it_remembers_stuff_in_context
    context = MiniRacer::Context.new
    context.eval("var x = function(){return 22;}")
    assert_equal 22, context.eval("x()")
  end

  def test_can_attach_functions
    context = MiniRacer::Context.new
    context.eval "var adder"
    context.attach("adder", proc { |a, b| a + b })
    assert_equal 3, context.eval("adder(1,2)")
  end

  def test_es6_arrow_functions
    context = MiniRacer::Context.new
    assert_equal 42, context.eval("var adder=(x,y)=>x+y; adder(21,21);")
  end

  def test_concurrent_access
    context = MiniRacer::Context.new
    context.eval("var counter=0; var plus=()=>counter++;")

    (1..10).map { Thread.new { context.eval("plus()") } }.each(&:join)

    assert_equal 10, context.eval("counter")
  end

  class FooError < StandardError
    def initialize(message)
      super(message)
    end
  end

  def test_attached_exceptions
    context = MiniRacer::Context.new
    context.attach("adder", proc { raise FooError, "I like foos" })
    assert_raises do
      begin
        raise FooError, "I like foos"
        context.eval("adder()")
      rescue => e
        assert_equal FooError, e.class
        assert_match(/I like foos/, e.message)
        # TODO backtrace splicing so js frames are injected
        raise
      end
    end
  end

  def test_attached_on_object
    context = MiniRacer::Context.new
    context.eval "var minion"
    context.attach("minion.speak", proc { "banana" })
    assert_equal "banana", context.eval("minion.speak()")
  end

  def test_attached_on_nested_object
    context = MiniRacer::Context.new
    context.eval "var minion"
    context.attach("minion.kevin.speak", proc { "banana" })
    assert_equal "banana", context.eval("minion.kevin.speak()")
  end

  def test_return_arrays
    context = MiniRacer::Context.new
    context.eval "var nose"
    context.attach("nose.type", proc { ["banana", ["nose"]] })
    assert_equal ["banana", ["nose"]], context.eval("nose.type()")
  end

  def test_return_hash
    context = MiniRacer::Context.new
    context.attach(
      "test",
      proc { { :banana => :nose, "inner" => { 42 => 42 } } }
    )
    assert_equal(
      { "banana" => "nose", "inner" => { "42" => 42 } },
      context.eval("test()")
    )
  end

  def test_date_nan
    # NoMethodError: undefined method `source_location' for "<internal:core>
    # core/float.rb:114:in `to_i'":Thread::Backtrace::Location
    skip "TruffleRuby bug" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new
    assert_raises(RangeError) { context.eval("new Date(NaN)") } # should not crash process
  end

  def test_return_date
    context = MiniRacer::Context.new
    test_time = Time.new
    test_datetime = test_time.to_datetime
    context.attach("test", proc { test_time })
    context.attach("test_datetime", proc { test_datetime })

    # check that marshalling to JS creates a date object (getTime())
    assert_equal(
      (test_time.to_f * 1000).to_i,
      context.eval("var result = test(); result.getTime();").to_i
    )

    # check that marshalling to RB creates a Time object
    result = context.eval("test()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)

    # check that no precision is lost in the marshalling (js only stores milliseconds)
    assert_equal(
      (test_time.tv_usec / 1000.0).floor,
      (result.tv_usec / 1000.0).floor
    )

    # check that DateTime gets marshalled to js date and back out as rb Time
    result = context.eval("test_datetime()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)
    assert_equal(
      (test_time.tv_usec / 1000.0).floor,
      (result.tv_usec / 1000.0).floor
    )
  end

  def test_datetime_missing
    date_time_backup = Object.send(:remove_const, :DateTime)

    begin
      # no exceptions should happen here, and non-datetime classes should marshall correctly still.
      context = MiniRacer::Context.new
      test_time = Time.new
      context.attach("test", proc { test_time })

      assert_equal(
        (test_time.to_f * 1000).to_i,
        context.eval("var result = test(); result.getTime();").to_i
      )

      result = context.eval("test()")
      assert_equal(test_time.class, result.class)
      assert_equal(test_time.tv_sec, result.tv_sec)
      assert_equal(
        (test_time.tv_usec / 1000.0).floor,
        (result.tv_usec / 1000.0).floor
      )
    ensure
      Object.const_set(:DateTime, date_time_backup)
    end
  end

  def test_return_large_number
    context = MiniRacer::Context.new
    test_num = 1_000_000_000_000_000
    context.attach("test", proc { test_num })

    assert_equal(true, context.eval("test() === 1000000000000000"))
    assert_equal(test_num, context.eval("test()"))
  end

  def test_return_int_max
    context = MiniRacer::Context.new
    test_num = 2**(31) - 1 #last int32 number
    context.attach("test", proc { test_num })

    assert_equal(true, context.eval("test() === 2147483647"))
    assert_equal(test_num, context.eval("test()"))
  end

  def test_return_unknown
    context = MiniRacer::Context.new
    test_unknown = Date.new # hits T_DATA in convert_ruby_to_v8
    context.attach("test", proc { test_unknown })
    assert_equal("Undefined Conversion", context.eval("test()"))

    # clean up and start up a new context
    context = nil
    GC.start

    context = MiniRacer::Context.new
    test_unknown = Date.new # hits T_DATA in convert_ruby_to_v8
    context.attach("test", proc { test_unknown })
    assert_equal("Undefined Conversion", context.eval("test()"))
  end

  def test_max_memory
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement max_memory"
    end
    context = MiniRacer::Context.new(max_memory: 200_000_000)

    assert_raises(MiniRacer::V8OutOfMemoryError) do
      context.eval(
        "let s = 1000; var a = new Array(s); a.fill(0); while(true) {s *= 1.1; let n = new Array(Math.floor(s)); n.fill(0); a = a.concat(n); };"
      )
    end
  end

  def test_max_memory_for_call
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement max_memory"
    end
    context = MiniRacer::Context.new(max_memory: 100_000_000)
    context.eval(<<~JS)
      let s;
      function memory_test() {
        var a = new Array(s);
        a.fill(0);
        while(true) {
          s *= 1.1;
          let n = new Array(Math.floor(s));
          n.fill(0);
          a = a.concat(n);
          if (s > 1000000) {
            return;
          }
        }
      }
      function set_s(val) {
        s = val;
      }
    JS
    context.call("set_s", 1000)
    assert_raises(MiniRacer::V8OutOfMemoryError) { context.call("memory_test") }
    s = context.eval("s")
    assert_operator(s, :>, 100_000)
  end

  def test_max_memory_bounds
    assert_raises(ArgumentError) do
      MiniRacer::Context.new(max_memory: -200_000_000)
    end

    assert_raises(ArgumentError) { MiniRacer::Context.new(max_memory: 2**32) }
  end

  module Echo
    def self.say(thing)
      thing
    end
  end

  def test_can_attach_method
    context = MiniRacer::Context.new
    context.eval "var Echo"
    context.attach("Echo.say", Echo.method(:say))
    assert_equal "hello", context.eval("Echo.say('hello')")
  end

  def test_attach_non_object
    context = MiniRacer::Context.new
    context.eval("var minion = 2")
    context.attach("minion.kevin.speak", proc { "banana" })
    assert_equal "banana", context.call("minion.kevin.speak")
  end

  def test_load
    context = MiniRacer::Context.new
    context.load(File.dirname(__FILE__) + "/file.js")
    assert_equal "world", context.eval("hello")
    assert_raises { context.load(File.dirname(__FILE__) + "/missing.js") }
  end

  def test_contexts_can_be_safely_GCed
    context = MiniRacer::Context.new
    context.eval 'var hello = "world";'

    context = nil
    GC.start
  end

  def test_it_can_use_snapshots
    snapshot =
      MiniRacer::Snapshot.new(
        'function hello() { return "world"; }; var foo = "bar";'
      )

    context = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal "world", context.eval("hello()")
    assert_equal "bar", context.eval("foo")
  end

  def test_snapshot_size
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement snapshots"
    end
    snapshot = MiniRacer::Snapshot.new('var foo = "bar";')

    # for some reason sizes seem to change across runs, so we just
    # check it's a positive integer
    assert(snapshot.size > 0)
  end

  def test_snapshot_dump
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement snapshots"
    end
    snapshot = MiniRacer::Snapshot.new('var foo = "bar";')
    dump = snapshot.dump

    assert_equal(String, dump.class)
    assert_equal(Encoding::ASCII_8BIT, dump.encoding)
    assert_equal(snapshot.size, dump.length)
  end

  def test_invalid_snapshots_throw_an_exception
    begin
      MiniRacer::Snapshot.new("var foo = bar;")
    rescue MiniRacer::SnapshotError => e
      assert(e.backtrace[0].include? "JavaScript")
      got_error = true
    end

    assert(got_error, "should raise")
  end

  def test_an_empty_snapshot_is_valid
    MiniRacer::Snapshot.new("")
    MiniRacer::Snapshot.new
    GC.start
  end

  def test_snapshots_can_be_warmed_up_with_no_side_effects
    # shamelessly inspired by https://github.com/v8/v8/blob/5.3.254/test/cctest/test-serialize.cc#L792-L854
    snapshot_source = <<-JS
      function f() { return Math.sin(1); }
      var a = 5;
    JS

    snapshot = MiniRacer::Snapshot.new(snapshot_source)

    warmup_source = <<-JS
      Math.tan(1);
      var a = f();
      Math.sin = 1;
    JS

    warmed_up_snapshot = snapshot.warmup!(warmup_source)

    context = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal 5, context.eval("a")
    assert_equal "function", context.eval("typeof(Math.sin)")
    assert_same snapshot, warmed_up_snapshot
  end

  def test_invalid_warmup_sources_throw_an_exception
    assert_raises(MiniRacer::SnapshotError) do
      MiniRacer::Snapshot.new("Math.sin = 1;").warmup!("var a = Math.sin(1);")
    end
  end

  def test_invalid_warmup_sources_throw_an_exception_2
    assert_raises(TypeError) do
      MiniRacer::Snapshot.new("function f() { return 1 }").warmup!([])
    end
  end

  def test_warming_up_with_invalid_source_does_not_affect_the_snapshot_internal_state
    snapshot = MiniRacer::Snapshot.new("Math.sin = 1;")

    begin
      snapshot.warmup!("var a = Math.sin(1);")
    rescue StandardError
      # do nothing
    end

    context = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal 1, context.eval("Math.sin")
  end

  def test_snapshots_can_be_GCed_without_affecting_contexts_created_from_them
    snapshot = MiniRacer::Snapshot.new("Math.sin = 1;")
    context = MiniRacer::Context.new(snapshot: snapshot)

    # force the snapshot to be GC'ed
    snapshot = nil
    GC.start

    # the context should still work fine
    assert_equal 1, context.eval("Math.sin")
  end

  def test_isolates_from_snapshot_dont_get_corrupted_if_the_snapshot_gets_warmed_up_or_GCed
    # basically tests that isolates get their own copy of the snapshot and don't
    # get corrupted if the snapshot is subsequently warmed up
    snapshot_source = <<-JS
      function f() { return Math.sin(1); }
      var a = 5;
    JS

    snapshot = MiniRacer::Snapshot.new(snapshot_source)

    warmump_source = <<-JS
      Math.tan(1);
      var a = f();
      Math.sin = 1;
    JS

    snapshot.warmup!(warmump_source)

    context1 = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal 5, context1.eval("a")
    assert_equal "function", context1.eval("typeof(Math.sin)")

    GC.start

    context2 = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal 5, context2.eval("a")
    assert_equal "function", context2.eval("typeof(Math.sin)")
  end

  def test_isolate_can_be_notified_of_idle_time
    context = MiniRacer::Context.new

    # returns true if embedder should stop calling
    assert(context.idle_notification(1000))
  end

  def test_platform_set_flags_raises_an_exception_if_already_initialized
    # makes sure it's initialized
    MiniRacer::Snapshot.new

    assert_raises(MiniRacer::PlatformAlreadyInitialized) do
      MiniRacer::Platform.set_flags! :noconcurrent_recompilation
    end
  end

  def test_platform_set_flags_works
    context = MiniRacer::Context.new

    assert_raises(MiniRacer::RuntimeError) do
      # should fail because of strict mode set for all these tests
      context.eval "x = 28"
    end
  end

  def test_error_on_return_val
    v8 = MiniRacer::Context.new
    assert_raises(MiniRacer::RuntimeError) do
      v8.eval(
        'var o = {}; o.__defineGetter__("bar", function() { return null(); }); o'
      )
    end
  end

  def test_ruby_based_property_in_rval
    v8 = MiniRacer::Context.new
    v8.attach "echo", proc { |x| x }
    assert_equal(
      { "bar" => 42 },
      v8.eval("var o = {get bar() { return echo(42); }}; o")
    )
  end

  def test_function_rval
    context = MiniRacer::Context.new
    context.attach("echo", proc { |msg| msg })
    assert_equal("foo", context.eval("echo('foo')"))
  end

  def test_timeout_in_ruby_land
    skip "TODO(bnoordhuis) need to think on how to interrupt ruby code"
    context = MiniRacer::Context.new(timeout: 50)
    context.attach("sleep", proc { sleep 10 })
    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval('sleep(); "hi";')
    end
  end

  def test_undef_mem
    context = MiniRacer::Context.new(timeout: 5)

    context.attach(
      "marsh",
      proc do |a, b, c|
        if a.is_a?(MiniRacer::FailedV8Conversion) ||
             b.is_a?(MiniRacer::FailedV8Conversion) ||
             c.is_a?(MiniRacer::FailedV8Conversion)
          return a, b, c
        end

        a[rand(10_000).to_s] = "a"
        b[rand(10_000).to_s] = "b"
        c[rand(10_000).to_s] = "c"
        [a, b, c]
      end
    )

    assert_raises do
      # TODO make it raise the correct exception!
      context.eval(
        "var a = [{},{},{}]; while(true) { a = marsh(a[0],a[1],a[2]); }"
      )
    end
  end

  def test_can_dispose_context
    context = MiniRacer::Context.new(timeout: 5)
    context.dispose
    assert_raises(MiniRacer::ContextDisposedError) { context.eval("a") }
  end

  def test_estimated_size
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement heap_stats"
    end
    context = MiniRacer::Context.new(timeout: 500)
    context.eval(<<~JS)
      let a='testing';
      let f=function(foo) { foo + 42 };

      // call `f` a lot to have things JIT'd so that total_heap_size_executable becomes > 0
      for (let i = 0; i < 1000000; i++) { f(10); }
    JS

    stats = context.heap_stats
    # eg: {:total_physical_size=>1280640, :total_heap_size_executable=>4194304, :total_heap_size=>3100672, :used_heap_size=>1205376, :heap_size_limit=>1501560832}
    assert_equal(
      %i[
        external_memory
        heap_size_limit
        malloced_memory
        number_of_detached_contexts
        number_of_native_contexts
        peak_malloced_memory
        total_available_size
        total_global_handles_size
        total_heap_size
        total_heap_size_executable
        total_physical_size
        used_global_handles_size
        used_heap_size
      ].sort,
      stats.keys.sort
    )

    assert_equal 0, stats[:external_memory]
    assert_equal 0, stats[:number_of_detached_contexts]
    stats.delete :external_memory
    stats.delete :number_of_detached_contexts

    assert(
      stats.values.all? { |v| v > 0 },
      "expecting the isolate to have values for all the vals: actual stats #{stats}"
    )
  end

  def test_releasing_memory
    context = MiniRacer::Context.new

    context.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 1_000_000}'")

    context.low_memory_notification

    end_heap = context.heap_stats[:used_heap_size]

    assert(
      (end_heap - start_heap).abs < 1000,
      "expecting most of the 1_000_000 long string to be freed"
    )
  end

  def test_bad_params
    assert_raises { MiniRacer::Context.new(random: :thing) }
  end

  def test_ensure_gc
    context = MiniRacer::Context.new(ensure_gc_after_idle: 1)
    context.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 10_000_000}'")

    sleep 0.01

    end_heap = context.heap_stats[:used_heap_size]

    assert(
      (end_heap - start_heap).abs < 1000,
      "expecting most of the 1_000_000 long string to be freed"
    )
  end

  def test_eval_with_filename
    context = MiniRacer::Context.new()
    context.eval("var foo = function(){baz();}", filename: "b/c/foo1.js")

    got_error = false
    begin
      context.eval("foo()", filename: "baz1.js")
    rescue MiniRacer::RuntimeError => e
      assert_match(/foo1.js/, e.backtrace[0])
      assert_match(/baz1.js/, e.backtrace[1])
      got_error = true
    end

    assert(got_error, "should raise")
  end

  def test_estimated_size_when_disposed
    context = MiniRacer::Context.new(timeout: 50)
    context.eval("let a='testing';")
    context.dispose

    assert_raises(MiniRacer::ContextDisposedError) { context.heap_stats }
  end

  def test_can_dispose
    skip "takes too long"
    #
    # junk_it_up
    # 3.times do
    #   GC.start(full_mark: true, immediate_sweep: true)
    # end
  end

  def junk_it_up
    1000.times do
      context = MiniRacer::Context.new(timeout: 5)
      context.dispose
    end
  end

  def test_attached_recursion
    context = MiniRacer::Context.new(timeout: 200)
    context.attach("a", proc { |a| a })
    context.attach("b", proc { |a| a })

    context.eval("const obj = {get r(){ b() }}; a(obj);")
  end

  def test_heap_dump
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement heap_dump"
    end
    f = Tempfile.new("heap")
    path = f.path
    f.unlink

    context = MiniRacer::Context.new
    context.eval("let x = 1000;")
    context.write_heap_snapshot(path)

    dump = File.read(path)

    assert dump.length > 0

    FileUtils.rm(path)
  end

  def test_pipe_leak
    # in Ruby 2.7 pipes will stay open for longer
    # make sure that we clean up early so pipe file
    # descriptors are not kept around
    context = MiniRacer::Context.new(timeout: 1000)
    10_000.times { |i| context.eval("'hello'") }
  end

  def test_symbol_support
    context = MiniRacer::Context.new()
    assert_equal "foo", context.eval("Symbol('foo')")
    assert_nil context.eval("Symbol()") # should not crash
  end

  def test_infinite_object_js
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement marshal_stack_depth"
    end
    context = MiniRacer::Context.new
    context.attach("a", proc { |a| a })

    js = <<~JS
      var d=0;
      function get(z) {
        z.depth=d++; // this isn't necessary to make it infinite, just to make it more obvious that it is
        Object.defineProperty(z,'foo',{get(){var r={};return get(r);},enumerable:true})
        return z;
      }
      a(get({}));
    JS

    assert_raises(MiniRacer::RuntimeError) { context.eval(js) }
  end

  def test_deep_object_js
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not yet implement marshal_stack_depth"
    end
    context = MiniRacer::Context.new
    context.attach("a", proc { |a| a })

    # stack depth should be enough to marshal the object
    assert_equal [[[]]], context.eval("let arr = [[[]]]; a(arr)")

    # too deep
    assert_raises(MiniRacer::RuntimeError) do
      context.eval("let arr = [[[[[[[[]]]]]]]]; a(arr)")
    end
  end

  def test_wasm_ref
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not support WebAssembly"
    end
    context = MiniRacer::Context.new
    # Error: [object Object] could not be cloned
    assert_raises(MiniRacer::RuntimeError) do
      context.eval(
        "
        var b = [0,97,115,109,1,0,0,0,1,26,5,80,0,95,0,80,0,95,1,127,0,96,0,1,110,96,1,100,2,1,111,96,0,1,100,3,3,4,3,3,2,4,7,26,2,12,99,114,101,97,116,101,83,116,114,117,99,116,0,1,7,114,101,102,70,117,110,99,0,2,9,5,1,3,0,1,0,10,23,3,8,0,32,0,20,2,251,27,11,7,0,65,12,251,0,1,11,4,0,210,0,11,0,44,4,110,97,109,101,1,37,3,0,11,101,120,112,111,114,116,101,100,65,110,121,1,12,99,114,101,97,116,101,83,116,114,117,99,116,2,7,114,101,102,70,117,110,99]
        var o = new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(b))).exports
        o.refFunc()(o.createStruct) // exotic object
      "
      )
    end
  end

  def test_proxy_support
    js = <<~JS
      function MyProxy(reference) {
        return new Proxy(function() {}, {
          get: function(obj, prop) {
            if (prop === Symbol.toPrimitive) return reference[prop];
            return new MyProxy(reference.concat(prop));
          },
          apply: function(target, thisArg, argumentsList) {
            myFunctionLogger(reference);
          }
        });
      };
      (new MyProxy([])).function_call(new MyProxy([])-1)
    JS
    context = MiniRacer::Context.new()
    context.attach("myFunctionLogger", ->(property) {})
    context.eval(js)
  end

  def test_proxy_uncloneable
    context = MiniRacer::Context.new()
    expected = { "x" => 42 }
    assert_equal expected, context.eval(<<~JS)
      const o = {x: 42}
      const p = new Proxy(o, {})
      Object.seal(p)
    JS
  end

  def test_promise
    context = MiniRacer::Context.new()
    context.eval <<~JS
      var x = 0;
      async function test() {
        return 99;
      }

      test().then(v => x = v);
    JS

    v = context.eval("x")
    assert_equal(v, 99)
  end

  def test_webassembly
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not enable WebAssembly by default"
    end
    context = MiniRacer::Context.new()
    context.eval("let instance = null;")
    filename = File.expand_path("../support/add.wasm", __FILE__)
    context.attach("loadwasm", proc { |f| File.read(filename).each_byte.to_a })
    context.attach("print", proc { |f| puts f })

    context.eval <<~JS
      WebAssembly
        .instantiate(new Uint8Array(loadwasm()), {
          wasi_snapshot_preview1: {
            proc_exit: function() { print("exit"); },
            args_get: function() { return 0 },
            args_sizes_get: function() { return 0 }
          }
        })
        .then(i => { instance = i["instance"];})
        .catch(e => print(e.toString()));
    JS

    context.pump_message_loop while !context.eval("instance")

    assert_equal(3, context.eval("instance.exports.add(1,2)"))
  end

  class ReproError < StandardError
    def initialize(response)
      super("response said #{response.code}")
    end
  end

  Response = Struct.new(:code)

  def test_exception_objects
    context = MiniRacer::Context.new
    context.attach("repro", lambda { raise ReproError.new(Response.new(404)) })
    assert_raises(ReproError) { context.eval("repro();") }
  end

  def test_timeout
    context = MiniRacer::Context.new(timeout: 500, max_memory: 20_000_000)
    assert_raises(MiniRacer::ScriptTerminatedError) { context.eval <<~JS }
        var doit = () => {
          while (true) {}
        }
        doit();
        JS
  end

  def test_eval_returns_unfrozen_string
    context = MiniRacer::Context.new
    result = context.eval("'Hello George!'")
    assert_equal("Hello George!", result)
    assert_equal(false, result.frozen?)
  end

  def test_call_returns_unfrozen_string
    context = MiniRacer::Context.new
    context.eval('function hello(name) { return "Hello " + name + "!" }')
    result = context.call("hello", "George")
    assert_equal("Hello George!", result)
    assert_equal(false, result.frozen?)
  end

  def test_callback_string_arguments_are_not_frozen
    context = MiniRacer::Context.new
    context.attach("test", proc { |text| text.frozen? })

    frozen = context.eval("test('Hello George!')")
    assert_equal(false, frozen)
  end

  def test_threading_safety
    Thread.new { MiniRacer::Context.new.eval("100") }.join
    GC.start
  end

  def test_forking
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby forking is not supported"
    else
      `bundle exec ruby test/test_forking.rb`
      assert false, "forking test failed" if $?.exitstatus != 0
    end
  end

  def test_poison
    context = MiniRacer::Context.new
    context.eval <<~JS
      const f = () => { throw "poison" }
      const d = {get: f, set: f}
      Object.defineProperty(Array.prototype, "0", d)
      Object.defineProperty(Array.prototype, "1", d)
    JS
    assert_equal 42, context.eval("42")
  end

  def test_map
    context = MiniRacer::Context.new
    expected = { "x" => 42 }
    assert_equal expected, context.eval("new Map([['x', 42]])")
    expected = ["x", 42]
    assert_equal expected, context.eval("new Map([['x', 42]]).entries()")
  end

  def test_regexp_string_iterator
    context = MiniRacer::Context.new
    exc = false
    begin
      context.eval("'abc'.matchAll(/./g)")
    rescue MiniRacer::RuntimeError => e
      # TODO(bnoordhuis) maybe detect the iterator object and serialize
      # it as an array of strings; problem is there is no V8 API to detect
      # regexp string iterator objects
      assert_match(
        /\[object RegExp String Iterator\] could not be cloned/,
        e.message
      )
      exc = true
    end
    assert exc
  end

  def test_function_cloning
    message = nil
    context = MiniRacer::Context.new
    context.attach("rails.logger.warn", proc { |msg| message = msg })
    context.eval <<~JS
               console = {
                 prefix: "[PrettyText] ",
                 log: function(...args){ rails.logger.info(console.prefix + args.join(" ")); },
               }
             JS

    context.eval("console.log('Hello', 'World!')")
    assert_equal "[PrettyText] Hello World!", message
  end
end
