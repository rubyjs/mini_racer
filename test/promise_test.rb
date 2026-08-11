require "test_helper"
require "timeout"

class MiniRacerPromiseTest < Minitest::Test
  def setup
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not implement MiniRacer::Promise"
    end
  end

  def test_call_returns_promise_for_async_function
    context = MiniRacer::Context.new
    context.eval("async function f(x) { return x * 2 }")
    promise = context.call("f", 21)
    assert_instance_of MiniRacer::Promise, promise
    assert_equal 42, promise.await
  end

  def test_eval_returns_promise
    context = MiniRacer::Context.new
    promise =
      context.eval("(async () => { await Promise.resolve(); return 6 * 7 })()")
    assert_instance_of MiniRacer::Promise, promise
    assert_equal 42, promise.await
  end

  def test_non_promise_results_unchanged
    context = MiniRacer::Context.new
    context.eval("function sync(x) { return x + 1 }")
    assert_equal 42, context.call("sync", 41)
    assert_equal 2, context.eval("1 + 1")
  end

  def test_await_returns_same_value_when_called_again
    context = MiniRacer::Context.new
    promise = context.eval("Promise.resolve(42)")
    assert_equal 42, promise.await
    assert_equal 42, promise.await
  end

  def test_await_out_of_order
    context = MiniRacer::Context.new
    context.eval(<<~JS)
      let resolvers = {};
      function makePending(k) { return new Promise(r => resolvers[k] = r) }
      function resolveIt(k, v) { resolvers[k](v) }
    JS
    a = context.call("makePending", "a")
    b = context.call("makePending", "b")
    context.call("resolveIt", "b", 2)
    assert_equal 2, b.await
    context.call("resolveIt", "a", 1)
    assert_equal 1, a.await
  end

  def test_await_rejection_raises_runtime_error
    context = MiniRacer::Context.new
    promise = context.eval("Promise.reject(new Error('kaboom'))")
    err = assert_raises(MiniRacer::RuntimeError) { promise.await }
    assert_includes err.message, "kaboom"
  end

  def test_await_delayed_task
    context = MiniRacer::Context.new
    promise = context.eval(<<~JS)
      (async () => {
        const i32 = new Int32Array(new SharedArrayBuffer(4));
        return (await Atomics.waitAsync(i32, 0, 0, 20).value);
      })()
    JS
    assert_equal "timed-out", promise.await
  end

  def test_await_runs_ruby_callbacks
    context = MiniRacer::Context.new
    context.attach("rubyAdd", proc { |a, b| a + b })
    context.eval(<<~JS)
      async function viaRuby() {
        await Promise.resolve();
        return rubyAdd(20, 22);
      }
    JS
    assert_equal 42, context.call("viaRuby").await
  end

  def test_await_from_ruby_callback_raises
    context = MiniRacer::Context.new
    context.eval("async function inner() { return 1 }")
    promise = context.call("inner")
    context.attach("reenter", proc { promise.await })
    context.eval("function outer() { return reenter() }")
    err =
      assert_raises(MiniRacer::RuntimeError) do
        Timeout.timeout(2) { context.call("outer") }
      end
    assert_includes err.message, "nested async call"
    assert_equal 2, context.eval("1 + 1")
  end

  def test_await_never_settling_promise_hits_timeout
    context = MiniRacer::Context.new(timeout: 200)
    promise = context.eval("new Promise(() => {})")
    start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    assert_raises(MiniRacer::ScriptTerminatedError) { promise.await }
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start
    assert_operator elapsed, :<, 5
    assert_equal 2, context.eval("1 + 1")
  end

  def test_await_never_settling_promise_interrupted_by_stop
    context = MiniRacer::Context.new
    promise = context.eval("new Promise(() => {})")
    stopper =
      Thread.new do
        sleep 0.1
        context.stop
      end
    assert_raises(MiniRacer::ScriptTerminatedError) { promise.await }
    stopper.join
    assert_equal 2, context.eval("1 + 1")
  end

  def test_await_after_dispose
    context = MiniRacer::Context.new
    promise = context.eval("Promise.resolve(1)")
    context.dispose
    assert_raises(MiniRacer::ContextDisposedError) { promise.await }
  end

  def test_gc_releases_v8_handles
    context = MiniRacer::Context.new
    context.eval("async function f() { return 1 }")
    100.times { context.call("f") }
    grown = context.heap_stats[:used_global_handles_size]
    GC.start
    context.eval("0") # next dispatch drains released handles
    after = context.heap_stats[:used_global_handles_size]
    assert_operator after, :<, grown
  end

  def test_promise_keeps_context_alive
    promise =
      MiniRacer::Context
        .new
        .tap { |c| c.eval("void 0") }
        .eval("Promise.resolve(42)")
    GC.start
    assert_equal 42, promise.await
  end
end
