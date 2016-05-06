require 'test_helper'

class MiniRacerTest < Minitest::Test
  def test_that_it_has_a_version_number
    refute_nil ::MiniRacer::VERSION
  end

  def test_it_can_eval_int
    context = MiniRacer::Context.new
    assert_equal 2, context.eval('1+1')
  end

  def test_it_can_eval_string
    context = MiniRacer::Context.new
    assert_equal "1+1", context.eval('"1+1"')
  end

  def test_it_returns_useful_errors
    context = MiniRacer::Context.new
    assert_raises do
      context.eval('var x=function(){boom;}; x()')
    end
  end

  def test_it_can_stop
    context = MiniRacer::Context.new
    assert_raises do
      Thread.new do
        sleep 0.001
        context.stop
      end
      context.eval('while(true){}')
    end
  end

  def test_it_can_automatically_time_out_context
    # 2 millisecs is a very short timeout but we don't want test running forever
    context = MiniRacer::Context.new(timeout: 2)
    assert_raises do
      context.eval('while(true){}')
    end
  end

  def test_it_handles_malformed_js
    context = MiniRacer::Context.new
    assert_raises do
      context.eval('I am not JavaScript {')
    end
  end

  def test_floats
    context = MiniRacer::Context.new
    assert_equal 1.2, context.eval('1.2')
  end

  def test_it_remembers_stuff_in_context
    context = MiniRacer::Context.new
    context.eval('var x = function(){return 22;}')
    assert_equal 22, context.eval('x()')
  end

end
