require 'test_helper'

class MiniRacerTest < Minitest::Test

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
    assert_equal nil, context.eval('null')
    assert_equal nil, context.eval('undefined')
  end

  def test_array
    context = MiniRacer::Context.new
    assert_equal [1,"two"], context.eval('[1,"two"]')
  end

  def test_object
    context = MiniRacer::Context.new
    # remember JavaScript is quirky {"1" : 1} magically turns to {1: 1} cause magic
    assert_equal({1 => 2, "two" => "two"}, context.eval('a={"1" : 2, "two" : "two"}'))
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
        sleep 0.001
        context.stop
      end
      context.eval('while(true){}')
    rescue => e
      exp = e
    end

    assert_equal MiniRacer::ScriptTerminatedError, exp.class
    assert_match(/terminated/, exp.message)

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
    assert_equal MiniRacer::JavaScriptFunction, context.eval("a = function(){}").class
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
    context.attach("adder", proc{|a,b| a+b})
    assert_equal 3, context.eval('adder(1,2)')
  end

  def test_es6_arrow_functions
    context = MiniRacer::Context.new
    assert_equal 42, context.eval('adder=(x,y)=>x+y; adder(21,21);')
  end

  def test_concurrent_access
    context = MiniRacer::Context.new
    context.eval('counter=0; plus=()=>counter++;')

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
    context.attach("minion.speak", proc{"banana"})
    assert_equal "banana", context.eval("minion.speak()")
  end

  def test_attached_on_nested_object
    context = MiniRacer::Context.new
    context.attach("minion.kevin.speak", proc{"banana"})
    assert_equal "banana", context.eval("minion.kevin.speak()")
  end

  def test_return_arrays
    context = MiniRacer::Context.new
    context.attach("nose.type", proc{["banana",["nose"]]})
    assert_equal ["banana", ["nose"]], context.eval("nose.type()")
  end

  def test_return_hash
    context = MiniRacer::Context.new
    context.attach("test", proc{{banana: :nose, "inner" => {42 => 42}}})
    assert_equal({"banana" => "nose", "inner" => {42 => 42}}, context.eval("test()"))
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
    Object.send(:remove_const, :DateTime)
    
    # no exceptions should happen here, and non-datetime classes should marshall correctly still.
    context = MiniRacer::Context.new
    test_time = Time.new
    context.attach("test", proc{test_time})
    
    assert_equal((test_time.to_f*1000).to_i, context.eval("var result = test(); result.getTime();").to_i)
    
    result = context.eval("test()")
    assert_equal(test_time.class, result.class)
    assert_equal(test_time.tv_sec, result.tv_sec)
    assert_equal((test_time.tv_usec/1000.0).floor, (result.tv_usec/1000.0).floor)
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

  module Echo
    def self.say(thing)
      thing
    end
  end

  def test_can_attach_method
    context = MiniRacer::Context.new
    context.attach("Echo.say", Echo.method(:say))
    assert_equal "hello", context.eval("Echo.say('hello')")
  end

  def test_attach_error
    context = MiniRacer::Context.new
    context.eval("minion = 2")
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

end
