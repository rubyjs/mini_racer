# frozen_string_literal: true

require 'securerandom'
require 'date'
require 'test_helper'

class MiniRacerTest < Minitest::Test
  # see `test_platform_set_flags_works` below
  MiniRacer::Platform.set_flags! :use_strict


  def test_locale
    skip "TruffleRuby does not have all js timezone by default" if RUBY_ENGINE == "truffleruby"
    val = MiniRacer::Context.new.eval("new Date('April 28 2021').toLocaleDateString('es-MX');")
    assert_equal '28/4/2021', val

    val = MiniRacer::Context.new.eval("new Date('April 28 2021').toLocaleDateString('en-US');")
    assert_equal '4/28/2021', val
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
    assert_equal 2, context.eval('2')
    assert_equal "two", context.eval('"two"')
    assert_equal 2.1, context.eval('2.1')
    assert_equal true, context.eval('true')
    assert_equal false, context.eval('false')
    assert_nil context.eval('null')
    assert_nil context.eval('undefined')
  end

  def test_compile_nil_context
    context = MiniRacer::Context.new
    assert_raises(TypeError) do
        assert_equal 2, context.eval(nil)
    end
  end

  def test_array
    context = MiniRacer::Context.new
    assert_equal [1,"two"], context.eval('[1,"two"]')
  end

  def test_object
    context = MiniRacer::Context.new
    # remember JavaScript is quirky {"1" : 1} magically turns to {1: 1} cause magic
    assert_equal({"1" => 2, "two" => "two"}, context.eval('var a={"1" : 2, "two" : "two"}; a'))
  end

  def test_it_returns_runtime_error
    context = MiniRacer::Context.new
    exp = nil

    begin
      context.eval('var foo=function(){boom;}; foo()')
    rescue => e
      exp = e
    end

    assert_equal MiniRacer::RuntimeError, exp.class

    assert_match(/boom/, exp.message)
    assert_match(/foo/, exp.backtrace[0])
    assert_match(/mini_racer/, exp.backtrace[2])

    # context should not be dead
    assert_equal 2, context.eval('1+1')
  end

  def test_it_can_stop
    context = MiniRacer::Context.new
    exp = nil

    begin
      Thread.new do
        sleep 0.01
        context.stop
      end
      context.eval('while(true){}')
    rescue => e
      exp = e
    end

    assert_equal MiniRacer::ScriptTerminatedError, exp.class
    assert_match(/terminated/, exp.message)

  end

  def test_it_can_timeout_during_serialization
    skip "TruffleRuby needs a fix for timing out during translation" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(timeout: 500)

    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval 'var a = {get a(){ while(true); }}; a'
    end
  end

  def test_it_can_automatically_time_out_context
    # 2 millisecs is a very short timeout but we don't want test running forever
    context = MiniRacer::Context.new(timeout: 2)
    assert_raises do
      context.eval('while(true){}')
    end
  end

  def test_returns_javascript_function
    context = MiniRacer::Context.new
    assert_same MiniRacer::JavaScriptFunction, context.eval("var a = function(){}; a").class
  end

  def test_it_handles_malformed_js
    context = MiniRacer::Context.new
    assert_raises MiniRacer::ParseError do
      context.eval('I am not JavaScript {')
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
    context.eval('var x = function(){return 22;}')
    assert_equal 22, context.eval('x()')
  end

  def test_can_attach_functions
    context = MiniRacer::Context.new
    context.eval 'var adder'
    context.attach("adder", proc{|a,b| a+b})
    assert_equal 3, context.eval('adder(1,2)')
  end

  def test_es6_arrow_functions
    context = MiniRacer::Context.new
    assert_equal 42, context.eval('var adder=(x,y)=>x+y; adder(21,21);')
  end

  def test_concurrent_access
    context = MiniRacer::Context.new
    context.eval('var counter=0; var plus=()=>counter++;')

    (1..10).map do
      Thread.new {
        context.eval("plus()")
      }
    end.each(&:join)

    assert_equal 10, context.eval("counter")
  end

  class FooError < StandardError
    def initialize(message)
      super(message)
    end
  end

  def test_attached_exceptions
    context = MiniRacer::Context.new
    context.attach("adder", proc{ raise FooError, "I like foos" })
    assert_raises do
      begin
raise FooError, "I like foos"
        context.eval('adder()')
      rescue => e
        assert_equal FooError, e.class
        assert_match( /I like foos/, e.message)
        # TODO backtrace splicing so js frames are injected
        raise
      end
    end
  end

  def test_attached_on_object
    context = MiniRacer::Context.new
    context.eval 'var minion'
    context.attach("minion.speak", proc{"banana"})
    assert_equal "banana", context.eval("minion.speak()")
  end

  def test_attached_on_nested_object
    context = MiniRacer::Context.new
    context.eval 'var minion'
    context.attach("minion.kevin.speak", proc{"banana"})
    assert_equal "banana", context.eval("minion.kevin.speak()")
  end

  def test_return_arrays
    context = MiniRacer::Context.new
    context.eval 'var nose'
    context.attach("nose.type", proc{["banana",["nose"]]})
    assert_equal ["banana", ["nose"]], context.eval("nose.type()")
  end

  def test_return_hash
    context = MiniRacer::Context.new
    context.attach("test", proc{{banana: :nose, "inner" => {42 => 42}}})
    assert_equal({"banana" => "nose", "inner" => {"42" => 42}}, context.eval("test()"))
  end

  def test_return_date
    context = MiniRacer::Context.new
    test_time = Time.new
    test_datetime = test_time.to_datetime
    context.attach("test", proc{test_time})
    context.attach("test_datetime", proc{test_datetime})

    # check that marshalling to JS creates a date object (getTime())
    assert_equal((test_time.to_f*1000).to_i, context.eval("var result = test(); result.getTime();").to_i)

    # check that marshalling to RB creates a Time object
    result = context.eval("test()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)

    # check that no precision is lost in the marshalling (js only stores milliseconds)
    assert_equal((test_time.tv_usec/1000.0).floor, (result.tv_usec/1000.0).floor)

    # check that DateTime gets marshalled to js date and back out as rb Time
    result = context.eval("test_datetime()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)
    assert_equal((test_time.tv_usec/1000.0).floor, (result.tv_usec/1000.0).floor)
  end

  def test_datetime_missing
    date_time_backup = Object.send(:remove_const, :DateTime)

    begin
      # no exceptions should happen here, and non-datetime classes should marshall correctly still.
      context = MiniRacer::Context.new
      test_time = Time.new
      context.attach("test", proc{test_time})

      assert_equal((test_time.to_f*1000).to_i, context.eval("var result = test(); result.getTime();").to_i)

      result = context.eval("test()")
      assert_equal(test_time.class, result.class)
      assert_equal(test_time.tv_sec, result.tv_sec)
      assert_equal((test_time.tv_usec/1000.0).floor, (result.tv_usec/1000.0).floor)
    ensure
      Object.const_set(:DateTime, date_time_backup)
    end
  end

  def test_return_large_number
    context = MiniRacer::Context.new
    test_num = 1_000_000_000_000_000
    context.attach("test", proc{test_num})

    assert_equal(true, context.eval("test() === 1000000000000000"))
    assert_equal(test_num, context.eval("test()"))
  end

  def test_return_int_max
    context = MiniRacer::Context.new
    test_num = 2 ** (31) - 1 #last int32 number
    context.attach("test", proc{test_num})

    assert_equal(true, context.eval("test() === 2147483647"))
    assert_equal(test_num, context.eval("test()"))
  end

  def test_return_unknown
    context = MiniRacer::Context.new
    test_unknown = Date.new # hits T_DATA in convert_ruby_to_v8
    context.attach("test", proc{test_unknown})
    assert_equal("Undefined Conversion", context.eval("test()"))

    # clean up and start up a new context
    context = nil
    GC.start

    context = MiniRacer::Context.new
    test_unknown = Date.new # hits T_DATA in convert_ruby_to_v8
    context.attach("test", proc{test_unknown})
    assert_equal("Undefined Conversion", context.eval("test()"))
  end

  def test_max_memory
    skip "TruffleRuby does not yet implement max_memory" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(max_memory: 200_000_000)

    assert_raises(MiniRacer::V8OutOfMemoryError) { context.eval('let s = 1000; var a = new Array(s); a.fill(0); while(true) {s *= 1.1; let n = new Array(Math.floor(s)); n.fill(0); a = a.concat(n); };') }
  end

  def test_max_memory_for_call
    skip "TruffleRuby does not yet implement max_memory" if RUBY_ENGINE == "truffleruby"
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
    context.call('set_s', 1000)
    assert_raises(MiniRacer::V8OutOfMemoryError) { context.call('memory_test') }
    s = context.eval('s')
    assert_operator(s, :>, 100_000)
  end

  def test_max_memory_bounds
    assert_raises(ArgumentError) do
      MiniRacer::Context.new(max_memory: -200_000_000)
    end

    assert_raises(ArgumentError) do
      MiniRacer::Context.new(max_memory: 2**32)
    end
  end

  module Echo
    def self.say(thing)
      thing
    end
  end

  def test_can_attach_method
    context = MiniRacer::Context.new
    context.eval 'var Echo'
    context.attach("Echo.say", Echo.method(:say))
    assert_equal "hello", context.eval("Echo.say('hello')")
  end

  def test_attach_error
    context = MiniRacer::Context.new
    context.eval("var minion = 2")
    assert_raises do
      begin
        context.attach("minion.kevin.speak", proc{"banana"})
      rescue => e
        assert_equal MiniRacer::ParseError, e.class
        assert_match(/expecting minion.kevin/, e.message)
        raise
      end
    end

  end

  def test_load
    context = MiniRacer::Context.new
    context.load(File.dirname(__FILE__) + "/file.js")
    assert_equal "world", context.eval("hello")
    assert_raises do
      context.load(File.dirname(__FILE__) + "/missing.js")
    end
  end

  def test_contexts_can_be_safely_GCed
    context = MiniRacer::Context.new
    context.eval 'var hello = "world";'

    context = nil
    GC.start
  end

  def test_it_can_use_snapshots
    snapshot = MiniRacer::Snapshot.new('function hello() { return "world"; }; var foo = "bar";')

    context = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal "world", context.eval("hello()")
    assert_equal "bar", context.eval("foo")
  end

  def test_snapshot_size
    skip "TruffleRuby does not yet implement snapshots" if RUBY_ENGINE == "truffleruby"
    snapshot = MiniRacer::Snapshot.new('var foo = "bar";')

    # for some reason sizes seem to change across runs, so we just
    # check it's a positive integer
    assert(snapshot.size > 0)
  end

  def test_snapshot_dump
    skip "TruffleRuby does not yet implement snapshots" if RUBY_ENGINE == "truffleruby"
    snapshot = MiniRacer::Snapshot.new('var foo = "bar";')
    dump = snapshot.dump

    assert_equal(String, dump.class)
    assert_equal(Encoding::ASCII_8BIT, dump.encoding)
    assert_equal(snapshot.size, dump.length)
  end

  def test_invalid_snapshots_throw_an_exception
    begin
      MiniRacer::Snapshot.new('var foo = bar;')
    rescue MiniRacer::SnapshotError => e
      assert(e.backtrace[0].include? 'JavaScript')
      got_error = true
    end

    assert(got_error, "should raise")
  end

  def test_an_empty_snapshot_is_valid
    MiniRacer::Snapshot.new('')
    MiniRacer::Snapshot.new
    GC.start
  end

  def test_snapshots_can_be_warmed_up_with_no_side_effects
    # shamelessly insipired by https://github.com/v8/v8/blob/5.3.254/test/cctest/test-serialize.cc#L792-L854
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
      MiniRacer::Snapshot.new('Math.sin = 1;').warmup!('var a = Math.sin(1);')
    end
  end

  def test_invalid_warmup_sources_throw_an_exception_2
    assert_raises(TypeError) do
      MiniRacer::Snapshot.new('function f() { return 1 }').warmup!([])
    end
  end

  def test_warming_up_with_invalid_source_does_not_affect_the_snapshot_internal_state
    snapshot = MiniRacer::Snapshot.new('Math.sin = 1;')

    begin
      snapshot.warmup!('var a = Math.sin(1);')
    rescue
      # do nothing
    end

    context = MiniRacer::Context.new(snapshot: snapshot)

    assert_equal 1, context.eval('Math.sin')
  end

  def test_snapshots_can_be_GCed_without_affecting_contexts_created_from_them
    snapshot = MiniRacer::Snapshot.new('Math.sin = 1;')
    context = MiniRacer::Context.new(snapshot: snapshot)

    # force the snapshot to be GC'ed
    snapshot = nil
    GC.start

    # the context should still work fine
    assert_equal 1, context.eval('Math.sin')
  end

  def test_it_can_re_use_isolates_for_multiple_contexts
    snapshot = MiniRacer::Snapshot.new('Math.sin = 1;')
    isolate = MiniRacer::Isolate.new(snapshot)

    context1 = MiniRacer::Context.new(isolate: isolate)
    assert_equal 1, context1.eval('Math.sin')

    context1.eval('var a = 5;')

    context2 = MiniRacer::Context.new(isolate: isolate)
    assert_equal 1, context2.eval('Math.sin')
    assert_raises MiniRacer::RuntimeError do
      begin
        context2.eval('a;')
      rescue => e
        assert_equal('ReferenceError: a is not defined', e.message)
        raise
      end
    end

    assert_same isolate, context1.isolate
    assert_same isolate, context2.isolate
  end

  def test_empty_isolate_is_valid_and_can_be_GCed
    MiniRacer::Isolate.new
    GC.start
  end

  def test_isolates_from_snapshot_dont_get_corrupted_if_the_snapshot_gets_warmed_up_or_GCed
    # basically tests that isolates get their own copy of the snapshot and don't
    # get corrupted if the snapshot is subsequently warmed up
    snapshot_source = <<-JS
      function f() { return Math.sin(1); }
      var a = 5;
    JS

    snapshot = MiniRacer::Snapshot.new(snapshot_source)
    isolate = MiniRacer::Isolate.new(snapshot)

    warmump_source = <<-JS
      Math.tan(1);
      var a = f();
      Math.sin = 1;
    JS

    snapshot.warmup!(warmump_source)

    context1 = MiniRacer::Context.new(isolate: isolate)

    assert_equal 5, context1.eval("a")
    assert_equal "function", context1.eval("typeof(Math.sin)")

    snapshot = nil
    GC.start

    context2 = MiniRacer::Context.new(isolate: isolate)

    assert_equal 5, context2.eval("a")
    assert_equal "function", context2.eval("typeof(Math.sin)")
  end

  def test_isolate_can_be_notified_of_idle_time
    isolate = MiniRacer::Isolate.new

    # returns true if embedder should stop calling
    assert(isolate.idle_notification(1000))
  end


  def test_concurrent_access_over_the_same_isolate_1
    isolate = MiniRacer::Isolate.new
    context = MiniRacer::Context.new(isolate: isolate)
    context.eval('var counter=0; var plus=()=>counter++;')

    (1..10).map do
      Thread.new {
        context.eval("plus()")
      }
    end.each(&:join)

    assert_equal 10, context.eval('counter')
  end

  def test_concurrent_access_over_the_same_isolate_2
    isolate = MiniRacer::Isolate.new

    # workaround Rubies prior to commit 475c8701d74ebebe
    # (Make SecureRandom support Ractor, 2020-09-04)
    SecureRandom.hex

    equals_after_sleep = (1..10).map do |i|
      Thread.new {
        random = SecureRandom.hex
        context = MiniRacer::Context.new(isolate: isolate)

        context.eval('var now = new Date().getTime(); while(new Date().getTime() < now + 20) {}')
        context.eval("var a='#{random}'")
        context.eval('var now = new Date().getTime(); while(new Date().getTime() < now + 20) {}')

        # cruby hashes are thread safe as long as you don't mess with the
        # same key in different threads
        context.eval('a') == random
       }
    end.map(&:value)

    assert_equal 10, equals_after_sleep.size
    assert equals_after_sleep.all?
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
      context.eval 'x = 28'
    end
  end

  def test_error_on_return_val
    v8 = MiniRacer::Context.new
    assert_raises(MiniRacer::RuntimeError) do
      v8.eval('var o = {}; o.__defineGetter__("bar", function() { return null(); }); o')
    end
  end

  def test_ruby_based_property_in_rval
    v8 = MiniRacer::Context.new
    v8.attach 'echo', proc{|x| x}
    assert_equal({"bar" => 42}, v8.eval("var o = {get bar() { return echo(42); }}; o"))
  end

  def test_function_rval
    context = MiniRacer::Context.new
    context.attach("echo", proc{|msg| msg})
    assert_equal("foo", context.eval("echo('foo')"))
  end

  def test_timeout_in_ruby_land
    context = MiniRacer::Context.new(timeout: 50)
    context.attach('sleep', proc{ sleep 0.1 })
    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval('sleep(); "hi";')
    end
  end

  def test_undef_mem
    context = MiniRacer::Context.new(timeout: 5)

    context.attach("marsh", proc do |a, b, c|
      return [a,b,c] if a.is_a?(MiniRacer::FailedV8Conversion) || b.is_a?(MiniRacer::FailedV8Conversion) || c.is_a?(MiniRacer::FailedV8Conversion)

      a[rand(10000).to_s] = "a"
      b[rand(10000).to_s] = "b"
      c[rand(10000).to_s] = "c"
      [a,b,c]
    end)

    assert_raises do
      # TODO make it raise the correct exception!
      context.eval("var a = [{},{},{}]; while(true) { a = marsh(a[0],a[1],a[2]); }")
    end

  end

  class TestPlatform < MiniRacer::Platform
    def self.public_flags_to_strings(flags)
      flags_to_strings(flags)
    end
  end

  def test_platform_flags_to_strings
    flags = [
      :flag1,
      [[[:flag2]]],
      {key1: :value1},
      {key2: 42,
       key3: 8.7},
      '--i_already_have_leading_hyphens',
      [:'--me_too',
       'i_dont']
    ]

    expected_string_flags = [
      '--flag1',
      '--flag2',
      '--key1 value1',
      '--key2 42',
      '--key3 8.7',
      '--i_already_have_leading_hyphens',
      '--me_too',
      '--i_dont'
    ]

    assert_equal expected_string_flags, TestPlatform.public_flags_to_strings(flags)
  end

  def test_can_dispose_context
    context = MiniRacer::Context.new(timeout: 5)
    context.dispose
    assert_raises(MiniRacer::ContextDisposedError) do
      context.eval("a")
    end
  end

  def test_estimated_size
    skip "TruffleRuby does not yet implement heap_stats" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(timeout: 5)
    context.eval("let a='testing';")

    stats = context.heap_stats
    # eg: {:total_physical_size=>1280640, :total_heap_size_executable=>4194304, :total_heap_size=>3100672, :used_heap_size=>1205376, :heap_size_limit=>1501560832}
    assert_equal(
      [:total_physical_size, :total_heap_size_executable, :total_heap_size, :used_heap_size, :heap_size_limit].sort,
      stats.keys.sort
    )

    assert(stats.values.all?{|v| v > 0}, "expecting the isolate to have values for all the vals")
  end

  def test_releasing_memory
    context = MiniRacer::Context.new

    context.isolate.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 1_000_000}'")

    context.isolate.low_memory_notification

    end_heap = context.heap_stats[:used_heap_size]

    assert((end_heap - start_heap).abs < 1000, "expecting most of the 1_000_000 long string to be freed")
  end

  def test_bad_params
    assert_raises do
      MiniRacer::Context.new(random: :thing)
    end
  end

  def test_ensure_gc
    context = MiniRacer::Context.new(ensure_gc_after_idle: 1)
    context.isolate.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 10_000_000}'")

    sleep 0.005

    end_heap = context.heap_stats[:used_heap_size]

    assert((end_heap - start_heap).abs < 1000, "expecting most of the 1_000_000 long string to be freed")
  end

  def test_eval_with_filename
    context = MiniRacer::Context.new()
    context.eval("var foo = function(){baz();}", filename: 'b/c/foo1.js')

    got_error = false
    begin
      context.eval("foo()", filename: 'baz1.js')
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

    stats = context.heap_stats
    assert(stats.values.all?{|v| v==0}, "should have 0 values once disposed")
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
    context.attach("a", proc{|a| a})
    context.attach("b", proc{|a| a})

    context.eval('const obj = {get r(){ b() }}; a(obj);')
  end

  def test_no_disposal_of_isolate_when_it_is_referenced
    isolate = MiniRacer::Isolate.new
    context = MiniRacer::Context.new(isolate: isolate)
    context.dispose
    _context2 = MiniRacer::Context.new(isolate: isolate) # Received signal 11 SEGV_MAPERR
  end

  def test_context_starts_with_no_isolate_value
    context = MiniRacer::Context.new
    assert_equal context.instance_variable_get('@isolate'), false
  end

  def test_context_isolate_value_is_kept
    context = MiniRacer::Context.new
    isolate = context.isolate
    assert_same isolate, context.isolate
  end

  def test_isolate_is_nil_after_disposal
    context = MiniRacer::Context.new
    context.dispose
    assert_nil context.isolate

    context = MiniRacer::Context.new
    context.isolate
    context.dispose
    assert_nil context.isolate
  end

  def test_heap_dump
    skip "TruffleRuby does not yet implement heap_dump" if RUBY_ENGINE == "truffleruby"
    f = Tempfile.new("heap")
    path = f.path
    f.unlink

    context = MiniRacer::Context.new
    context.eval('let x = 1000;')
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
    10000.times do |i|
      context.eval("'hello'")
    end
  end

  def test_symbol_support
    context = MiniRacer::Context.new()
    assert_equal :foo, context.eval("Symbol('foo')")
  end

  def test_cyclical_object_js
    skip "TruffleRuby does not yet implement marshal_stack_depth" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(marshal_stack_depth: 5)
    context.attach("a", proc{|a| a})

    assert_raises(MiniRacer::RuntimeError) { context.eval("var o={}; o.o=o; a(o)") }
  end

  def test_cyclical_array_js
    skip "TruffleRuby does not yet implement marshal_stack_depth" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(marshal_stack_depth: 5)
    context.attach("a", proc{|a| a})

    assert_raises(MiniRacer::RuntimeError) { context.eval("let arr = []; arr.push(arr); a(arr)") }
  end

  def test_cyclical_elem_in_array_js
    skip "TruffleRuby does not yet implement marshal_stack_depth" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(marshal_stack_depth: 5)
    context.attach("a", proc{|a| a})

    assert_raises(MiniRacer::RuntimeError) { context.eval("let arr = []; arr[0]=1; arr[1]=arr; a(arr)") }
  end

  def test_infinite_object_js
    skip "TruffleRuby does not yet implement marshal_stack_depth" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(marshal_stack_depth: 5)
    context.attach("a", proc{|a| a})
  
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
    skip "TruffleRuby does not yet implement marshal_stack_depth" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new(marshal_stack_depth: 5)
    context.attach("a", proc{|a| a})

    # stack depth should be enough to marshal the object
    assert_equal [[[]]], context.eval("let arr = [[[]]]; a(arr)")

    # too deep
    assert_raises(MiniRacer::RuntimeError) { context.eval("let arr = [[[[[[[[]]]]]]]]; a(arr)") }
  end

  def test_stackdepth_bounds
    assert_raises(ArgumentError) do
      MiniRacer::Context.new(marshal_stack_depth: -2)
    end

    assert_raises(ArgumentError) do
      MiniRacer::Context.new(marshal_stack_depth: MiniRacer::MARSHAL_STACKDEPTH_MAX_VALUE+1)
    end
  end

  def test_proxy_support
    js = <<~JS
      function MyProxy(reference) {
        return new Proxy(function() {}, {
          get: function(obj, prop) {
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
    context.attach('myFunctionLogger', ->(property) { })
    context.eval(js)
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

    v = context.eval("x");
    assert_equal(v, 99)
  end

  def test_webassembly
    skip "TruffleRuby does not enable WebAssembly by default" if RUBY_ENGINE == "truffleruby"
    context = MiniRacer::Context.new()
    context.eval("let instance = null;")
    filename = File.expand_path("../support/add.wasm", __FILE__)
    context.attach("loadwasm", proc {|f| File.read(filename).each_byte.to_a})
    context.attach("print", proc {|f| puts f})

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

    while !context.eval("instance") do
      context.isolate.pump_message_loop
    end

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
    context.attach('repro', lambda {
      raise ReproError.new(Response.new(404))
    })
    assert_raises(ReproError) do
      context.eval('repro();')
    end
  end

  def test_timeout
    context = MiniRacer::Context.new(timeout: 500, max_memory: 20_000_000)
    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval <<~JS
        var doit = async() => {
        while (true)
          await new Promise(resolve => resolve())
        }
        doit();
        JS
    end
  end
end
