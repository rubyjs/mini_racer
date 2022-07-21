require 'test_helper'
require 'timeout'

class MiniRacerFunctionTest < Minitest::Test
  def test_fun
    context = MiniRacer::Context.new
    context.eval("function f(x) { return 'I need ' + x + ' foos' }")
    assert_equal context.eval('f(10)'), 'I need 10 foos'

    assert_raises(ArgumentError) do
      context.call
    end

    count = 4
    res = context.call('f', count)
    assert_equal "I need #{count} foos", res
  end

  def test_non_existing_function
    context = MiniRacer::Context.new
    context.eval("function f(x) { return 'I need ' + x + ' galettes' }")

    # f is defined, let's call g
    assert_raises(MiniRacer::RuntimeError) do
      context.call('g')
    end
  end

  def test_throwing_function
    context = MiniRacer::Context.new
    context.eval('function f(x) { throw new Error("foo bar") }')

    # f is defined, let's call g
    err = assert_raises(MiniRacer::RuntimeError) do
      context.call('f', 1)
    end
    assert_equal err.message, 'Error: foo bar'
    assert_match(/1:23/, err.backtrace[0]) unless RUBY_ENGINE == "truffleruby"
    assert_match(/1:/, err.backtrace[0]) if RUBY_ENGINE == "truffleruby"
  end

  def test_args_types
    context = MiniRacer::Context.new
    context.eval("function f(x, y) { return 'I need ' + x + ' ' + y }")

    res = context.call('f', 3, 'bars')
    assert_equal 'I need 3 bars', res

    res = context.call('f', { a: 1 }, 'bars')
    assert_equal 'I need [object Object] bars', res

    res = context.call('f', [1, 2, 3], 'bars')
    assert_equal 'I need 1,2,3 bars', res
  end

  def test_complex_return
    context = MiniRacer::Context.new
    context.eval('function f(x, y) { return { vx: x, vy: y, array: [x, y] } }')

    h = { 'vx' => 3, 'vy' => 'bars', 'array' => [3, 'bars'] }
    res = context.call('f', 3, 'bars')
    assert_equal h, res
  end

  def test_do_not_hang_with_concurrent_calls
    context = MiniRacer::Context.new
    context.eval("function f(x) { return 'I need ' + x + ' foos' }")

    thread_count = 2

    threads = []
    thread_count.times do
      threads << Thread.new do
        10.times do |i|
          context.call('f', i)
        end
      end
    end

    joined_thread_count = 0
    for t in threads do
      joined_thread_count += 1
      t.join
    end

    # Dummy test, completing should be enough to show we don't hang
    assert_equal thread_count, joined_thread_count
  end
end
