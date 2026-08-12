require "test_helper"
require "timeout"

class MiniRacerAsyncTest < Minitest::Test
  def setup
    if RUBY_ENGINE == "truffleruby"
      skip "TruffleRuby does not implement call_await/eval_await"
    end
  end

  def test_call_await_returns_settled_value
    context = MiniRacer::Context.new
    context.eval("async function f(x) { return x * 2 }")
    assert_equal 42, context.call_await("f", 21)
  end

  def test_call_await_passes_arguments_exactly
    context = MiniRacer::Context.new
    context.eval("async function count() { return arguments.length }")
    assert_equal 3, context.call_await("count", 1, 2, 3)
    assert_equal 0, context.call_await("count")
  end

  def test_call_await_microtask_chain
    context = MiniRacer::Context.new
    context.eval(<<~JS)
      async function chain() {
        let n = 0;
        for (let i = 0; i < 100; i++) {
          await Promise.resolve();
          n++;
        }
        return n;
      }
    JS
    assert_equal 100, context.call_await("chain")
  end

  def test_call_await_non_promise_passthrough
    context = MiniRacer::Context.new
    context.eval("function sync(x) { return x + 1 }")
    assert_equal 42, context.call_await("sync", 41)
  end

  def test_call_await_rejection_raises_runtime_error
    context = MiniRacer::Context.new
    context.eval("async function boom() { throw new Error('kaboom') }")
    err = assert_raises(MiniRacer::RuntimeError) { context.call_await("boom") }
    assert_includes err.message, "kaboom"
  end

  def test_call_await_rejection_with_non_error_value
    context = MiniRacer::Context.new
    context.eval("function nope() { return Promise.reject('just a string') }")
    assert_raises(MiniRacer::RuntimeError) { context.call_await("nope") }
  end

  def test_call_await_non_existing_function
    context = MiniRacer::Context.new
    assert_raises(MiniRacer::RuntimeError) { context.call_await("missing") }
  end

  def test_eval_await_top_level_promise
    context = MiniRacer::Context.new
    result =
      context.eval_await(
        "(async () => { await Promise.resolve(); return 6 * 7 })()"
      )
    assert_equal 42, result
  end

  def test_eval_await_non_promise
    context = MiniRacer::Context.new
    assert_equal 2, context.eval_await("1 + 1")
  end

  def test_eval_await_filename
    context = MiniRacer::Context.new
    err =
      assert_raises(MiniRacer::RuntimeError) do
        context.eval_await("Promise.reject(new Error('x'))", filename: "foo.js")
      end
    assert_match(/foo\.js/, err.backtrace[0])
  end

  def test_call_await_ruby_callback_in_awaited_chain
    context = MiniRacer::Context.new
    context.attach("rubyAdd", proc { |a, b| a + b })
    context.eval(<<~JS)
      async function viaRuby() {
        await Promise.resolve();
        return rubyAdd(20, 22);
      }
    JS
    assert_equal 42, context.call_await("viaRuby")
  end

  def test_call_await_ruby_callback_exception_propagates
    context = MiniRacer::Context.new
    context.attach("rubyBoom", proc { raise "ruby boom" })
    context.eval(<<~JS)
      async function boomRuby() {
        await Promise.resolve();
        return rubyBoom();
      }
    JS
    err = assert_raises(RuntimeError) { context.call_await("boomRuby") }
    assert_includes err.message, "ruby boom"
  end

  def test_nested_call_await_fails_instead_of_deadlocking
    context = MiniRacer::Context.new
    context.attach("rubyCallsAsync", proc { context.call_await("inner") })
    context.eval(<<~JS)
      async function inner() {
        await Promise.resolve();
        return 42;
      }

      async function outer() {
        await Promise.resolve();
        return rubyCallsAsync();
      }
    JS

    err =
      assert_raises(MiniRacer::RuntimeError) do
        Timeout.timeout(2) { context.call_await("outer") }
      end
    assert_includes err.message, "nested async call"
    assert_equal 2, context.eval("1 + 1")
  end

  def test_nested_eval_await_fails_instead_of_deadlocking
    context = MiniRacer::Context.new
    context.attach(
      "rubyEvalsAsync",
      proc do
        context.eval_await(
          "(async () => { await Promise.resolve(); return 42 })()"
        )
      end
    )
    context.eval(<<~JS)
      async function outer() {
        await Promise.resolve();
        return rubyEvalsAsync();
      }
    JS

    err =
      assert_raises(MiniRacer::RuntimeError) do
        Timeout.timeout(2) { context.call_await("outer") }
      end
    assert_includes err.message, "nested async call"
    assert_equal 2, context.eval("1 + 1")
  end

  def test_eval_await_delayed_task
    context = MiniRacer::Context.new
    result = context.eval_await(<<~JS)
        (async () => {
          const i32 = new Int32Array(new SharedArrayBuffer(4));
          return (await Atomics.waitAsync(i32, 0, 0, 20).value);
        })()
      JS
    assert_equal "timed-out", result
  end

  def test_timeout_survives_nested_sync_call
    context = MiniRacer::Context.new(timeout: 200)
    context.attach("rubyCallsSync", proc { context.call("inner") })
    context.eval(<<~JS)
      function inner() {
        return 42;
      }

      async function outer() {
        await Promise.resolve();
        rubyCallsSync();
        return new Promise(() => {});
      }
    JS

    assert_raises(MiniRacer::ScriptTerminatedError) do
      Timeout.timeout(2) { context.call_await("outer") }
    end
    assert_equal 2, context.eval("1 + 1")
  end

  def test_never_settling_promise_hits_timeout
    context = MiniRacer::Context.new(timeout: 200)
    start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval_await("new Promise(() => {})")
    end
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start
    assert_operator elapsed, :<, 5
    assert_equal 2, context.eval("1 + 1")
  end

  def test_pump_message_loop_does_not_consume_stop
    context = MiniRacer::Context.new
    context.attach(
      "stopAndPump",
      proc do
        context.stop
        context.pump_message_loop
      end
    )

    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval("stopAndPump(); 42")
    end
    assert_equal 2, context.eval("1 + 1")
  end

  def test_never_settling_promise_interrupted_by_stop
    context = MiniRacer::Context.new
    stopper =
      Thread.new do
        sleep 0.1
        context.stop
      end
    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval_await("new Promise(() => {})")
    end
    stopper.join
    assert_equal 2, context.eval("1 + 1")
  end

  def test_never_settling_promise_interrupted_by_thread_kill
    context = MiniRacer::Context.new
    thread =
      Thread.new do
        context.eval_await("new Promise(() => {})")
      rescue MiniRacer::ScriptTerminatedError
        nil
      end
    sleep 0.1
    thread.kill
    assert thread.join(3), "awaiting thread did not stop"
    assert_equal 2, context.eval("1 + 1")
  end

  def test_call_sync_does_not_await
    context = MiniRacer::Context.new
    context.eval("async function f() { return 42 }")
    assert_equal({}, context.call("f"))
  end

  def test_dispose_after_call_await
    context = MiniRacer::Context.new
    context.eval("async function f() { return 1 }")
    assert_equal 1, context.call_await("f")
    context.dispose
    assert_raises(MiniRacer::ContextDisposedError) { context.call_await("f") }
  end
end
