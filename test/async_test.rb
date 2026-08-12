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
    assert_includes err.message, "nested call_await/eval_await"
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
    assert_includes err.message, "nested call_await/eval_await"
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

  def test_nested_sync_call_does_not_consume_stop
    context = MiniRacer::Context.new
    nested_terminated = false
    context.eval("function inner() { return 1 }")
    context.attach(
      "stopAndCall",
      proc do
        context.stop
        begin
          context.call("inner")
        rescue MiniRacer::ScriptTerminatedError
          nested_terminated = true
        end
        nil
      end
    )

    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval("stopAndCall(); 42")
    end
    assert nested_terminated
    assert_equal 2, context.eval("1 + 1")
  end

  def test_nested_sync_eval_does_not_consume_timeout
    context = MiniRacer::Context.new(timeout: 100)
    nested_terminated = false
    context.attach(
      "runSlowEval",
      proc do
        begin
          context.eval(<<~JS)
            (() => {
              const start = Date.now();
              while (Date.now() - start < 400) {}
            })();
          JS
        rescue MiniRacer::ScriptTerminatedError
          nested_terminated = true
        end
        nil
      end
    )

    source = <<~JS
      runSlowEval();
      (() => {
        const start = Date.now();
        while (Date.now() - start < 500) {}
      })();
    JS

    assert_raises(MiniRacer::ScriptTerminatedError) do
      Timeout.timeout(2) { context.eval(source) }
    end
    assert nested_terminated
    assert_equal 2, context.eval("1 + 1")
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

  def test_await_from_pumped_callback_fails_without_hanging
    context = MiniRacer::Context.new
    nested_error = nil
    context.attach(
      "nestedAwait",
      proc do
        context.eval_await("new Promise(() => {})")
      rescue MiniRacer::RuntimeError => error
        nested_error = error
      end
    )
    context.eval(<<~JS)
      const i32 = new Int32Array(new SharedArrayBuffer(4));
      Atomics.waitAsync(i32, 0, 0, 20).value.then(() => nestedAwait());
    JS

    Timeout.timeout(2) do
      until nested_error
        context.pump_message_loop
        sleep 0.01
      end
    end
    assert_includes nested_error.message, "nested call_await/eval_await"
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

  def test_sync_stop_does_not_leave_a_wakeup_task
    context = MiniRacer::Context.new
    context.attach("stopNow", proc { context.stop })

    assert_raises(MiniRacer::ScriptTerminatedError) do
      context.eval("stopNow(); 42")
    end
    assert_equal false, context.pump_message_loop
    assert_equal 2, context.eval("1 + 1")
  end

  def test_eval_await_preserves_max_memory_error
    context = MiniRacer::Context.new(max_memory: 100_000)
    forced_gc = false
    context.attach(
      "forceGc",
      proc do
        forced_gc = true
        context.low_memory_notification
      end
    )

    assert_raises(MiniRacer::V8OutOfMemoryError) do
      Timeout.timeout(2) do
        # Trigger GC from a microtask while eval_await owns the pending promise.
        context.eval_await(<<~JS)
          Promise.resolve().then(() => forceGc());
          new Promise(() => {});
        JS
      end
    end
    assert forced_gc
    assert_equal 2, context.eval("1 + 1")
  end

  def test_late_watchdog_does_not_terminate_next_eval
    value_size = 20_000_000
    snapshot = MiniRacer::Snapshot.new("var value = 'x'.repeat(#{value_size})")
    context = MiniRacer::Context.new(timeout: 5, snapshot: snapshot)

    # Serializing this result outlives the watchdog after eval's final
    # termination check. Its late timeout belongs to this eval, not the next.
    assert_equal value_size, context.eval("value").bytesize
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
