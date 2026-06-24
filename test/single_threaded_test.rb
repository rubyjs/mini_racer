# frozen_string_literal: true

require "test_helper"
require "open3"
require "rbconfig"
require "tempfile"

class MiniRacerSingleThreadedTest < Minitest::Test
  def assert_single_threaded_script(script)
    skip "single-threaded V8 platform tests are only for CRuby" unless RUBY_ENGINE == "ruby"

    file = Tempfile.new(["mini_racer_single_threaded", ".rb"])
    file.write(<<~RUBY)
      $LOAD_PATH.unshift #{File.expand_path("../lib", __dir__).inspect}
      require "mini_racer"

      MiniRacer::Platform.set_flags!(:single_threaded)

      #{script}
    RUBY
    file.close

    stdout, stderr, status = Open3.capture3(RbConfig.ruby, file.path)
    assert status.success?, <<~MSG
      single-threaded script failed with status #{status.exitstatus}
      stdout:
      #{stdout}
      stderr:
      #{stderr}
    MSG
  ensure
    file&.unlink
  end

  def test_basic_eval_and_call
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      raise "bad eval" unless context.eval("1 + 1") == 2
      context.eval("function add(a, b) { return a + b }")
      raise "bad call" unless context.call("add", 20, 22) == 42
    RUBY
  end

  def test_ruby_callback_from_javascript
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.attach("ruby_add", proc { |a, b| a + b })
      raise "bad callback result" unless context.eval("ruby_add(20, 22)") == 42
    RUBY
  end

  def test_nested_javascript_ruby_javascript_call
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.eval("function js_add(a, b) { return a + b }")
      context.attach("ruby_calls_js", proc { context.call("js_add", 20, 22) })
      raise "bad nested callback result" unless context.eval("ruby_calls_js()") == 42
    RUBY
  end

  def test_recursive_javascript_ruby_callback_ping_pong
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.attach("ruby_recurse", proc { |n|
        n <= 0 ? "done" : context.call("js_recurse", n - 1)
      })
      context.eval(<<~JS)
        function js_recurse(n) {
          if (n <= 0) return "done";
          return ruby_recurse(n);
        }
      JS
      raise "bad recursive callback result" unless context.call("js_recurse", 10) == "done"
    RUBY
  end

  def test_ruby_callback_exception_propagates
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.attach("boom", proc { raise "ruby boom" })

      begin
        context.eval("boom()")
        raise "expected callback exception"
      rescue RuntimeError => e
        raise "wrong exception: #{e.class}: #{e.message}" unless e.message.include?("ruby boom")
      end
    RUBY
  end

  def test_dispose_after_runner_started
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      raise "bad eval" unless context.eval("1 + 1") == 2
      context.dispose

      begin
        context.eval("1 + 1")
        raise "expected disposed error"
      rescue MiniRacer::ContextDisposedError
      end

      context = nil
      GC.start
    RUBY
  end

  def test_multiple_contexts_and_dispose_one
    assert_single_threaded_script <<~'RUBY'
      a = MiniRacer::Context.new
      b = MiniRacer::Context.new

      a.eval("var x = 1")
      b.eval("var x = 2")

      raise "bad context a" unless a.eval("x") == 1
      raise "bad context b" unless b.eval("x") == 2

      a.dispose
      raise "context b broke after disposing a" unless b.eval("x + 40") == 42
    RUBY
  end

  def test_busy_eval_thread_does_not_block_process_exit
    assert_single_threaded_script <<~'RUBY'
      Thread.report_on_exception = false
      Thread.new do
        sleep 3
        warn "process timed out"
        exit! 99
      end

      context = MiniRacer::Context.new
      Thread.new { context.eval("while (true) {}") }
      sleep 0.1
      # Fall off the end. Ruby will interrupt the eval thread during VM shutdown.
    RUBY
  end

  def test_thread_kill_interrupts_busy_eval
    assert_single_threaded_script <<~'RUBY'
      Thread.report_on_exception = false
      context = MiniRacer::Context.new
      thread = Thread.new { context.eval("while (true) {}") }
      sleep 0.1
      thread.kill
      raise "thread did not stop" unless thread.join(3)
      raise "context should still be usable" unless context.eval("1 + 1") == 2
    RUBY
  end

  def test_thread_raise_interrupts_busy_eval
    assert_single_threaded_script <<~'RUBY'
      Thread.report_on_exception = false
      context = MiniRacer::Context.new
      thread = Thread.new do
        begin
          context.eval("while (true) {}")
          raise "expected interrupt"
        rescue RuntimeError => e
          raise unless e.message == "interrupt"
        end
      end
      sleep 0.1
      thread.raise RuntimeError, "interrupt"
      raise "thread did not stop" unless thread.join(3)
      raise "context should still be usable" unless context.eval("1 + 1") == 2
    RUBY
  end

  def test_thread_kill_waiting_thread_does_not_terminate_active_eval
    assert_single_threaded_script <<~'RUBY'
      Thread.report_on_exception = false
      context = MiniRacer::Context.new
      terminated = false
      active = Thread.new do
        begin
          context.eval("while (true) {}")
        rescue MiniRacer::ScriptTerminatedError
          terminated = true
        end
      end
      sleep 0.1

      waiter = Thread.new { context.eval("1 + 1") rescue nil }
      sleep 0.1
      waiter.kill
      sleep 0.2
      raise "killing waiter terminated active eval" if terminated

      context.stop
      raise "active thread did not stop" unless active.join(3)
      raise "waiting thread did not stop" unless waiter.join(3)
    RUBY
  end

  def test_dispose_interrupts_busy_eval_from_another_thread
    assert_single_threaded_script <<~'RUBY'
      Thread.report_on_exception = false
      context = MiniRacer::Context.new
      thread = Thread.new do
        begin
          context.eval("while (true) {}")
        rescue MiniRacer::ScriptTerminatedError, MiniRacer::ContextDisposedError
        end
      end
      sleep 0.1
      context.dispose
      raise "thread did not stop" unless thread.join(3)
    RUBY
  end

  def test_dispose_from_callback_raises_instead_of_hanging
    assert_single_threaded_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.attach("dispose_self", proc do
        begin
          context.dispose
          "disposed"
        rescue MiniRacer::RuntimeError => e
          raise unless e.message.include?("busy") || e.message.include?("resource")
          "busy"
        end
      end)

      raise "bad callback result" unless context.eval("dispose_self()") == "busy"
      raise "context should still be usable" unless context.eval("1 + 1") == 2
    RUBY
  end

  def test_dispose_while_callback_is_running
    assert_single_threaded_script <<~'RUBY'
      Thread.report_on_exception = false
      started_r, started_w = IO.pipe
      release_r, release_w = IO.pipe
      context = MiniRacer::Context.new
      context.attach("block", proc do
        started_w.write("x")
        started_w.flush
        release_r.read(1)
        42
      end)

      eval_thread = Thread.new do
        begin
          context.eval("block()")
        rescue MiniRacer::RuntimeError, MiniRacer::ContextDisposedError
        end
      end
      started_r.read(1)

      dispose_thread = Thread.new { context.dispose }
      sleep 0.1
      release_w.write("x")
      release_w.flush

      raise "eval thread did not finish" unless eval_thread.join(3)
      raise "dispose thread did not finish" unless dispose_thread.join(3)
    RUBY
  end

  def test_fork_after_runner_started_and_idle
    assert_single_threaded_script <<~'RUBY'
      exit 0 unless Process.respond_to?(:fork)

      context = MiniRacer::Context.new
      context.eval("var answer = 41")
      context.eval("answer += 1") # starts a runner and exercises the post-dispatch path

      pid = fork do
        Thread.new do
          sleep 3
          warn "child timed out"
          exit! 99
        end

        exit!(context.eval("answer") == 42 ? 0 : 1)
      end
      _, status = Process.wait2(pid)
      raise "child failed with status #{status.inspect}" unless status.success?
    RUBY
  end

  def test_fork_child_normal_exit_after_using_inherited_context
    assert_single_threaded_script <<~'RUBY'
      exit 0 unless Process.respond_to?(:fork)

      context = MiniRacer::Context.new
      context.eval("var answer = 41")
      context.eval("answer += 1")

      pid = fork do
        Thread.new do
          sleep 3
          warn "child timed out"
          exit! 99
        end

        raise "bad child eval" unless context.eval("answer") == 42
        context.dispose
        # Intentionally fall off the end instead of exit!: this exercises
        # normal child-process finalizers for an inherited context.
      end
      _, status = Process.wait2(pid)
      raise "child failed with status #{status.inspect}" unless status.success?
    RUBY
  end

  def test_fork_child_gc_after_non_idle_inherited_context
    assert_single_threaded_script <<~'RUBY'
      exit 0 unless Process.respond_to?(:fork)

      started_r, started_w = IO.pipe
      release_r, release_w = IO.pipe
      context = MiniRacer::Context.new
      context.attach("block", proc do
        started_w.write("x")
        started_w.flush
        release_r.read(1)
        42
      end)

      worker = Thread.new do
        raise "bad callback result" unless context.eval("block()") == 42
      end
      started_r.read(1)

      pid = fork do
        Thread.new do
          sleep 3
          warn "child timed out"
          exit! 99
        end

        context = nil
        GC.start
        GC.compact if GC.respond_to?(:compact)
        exit! 0
      end
      _, status = Process.wait2(pid)
      raise "child failed with status #{status.inspect}" unless status.success?

      release_w.write("x")
      release_w.flush
      raise "worker did not finish" unless worker.join(3)
    RUBY
  end

  def test_fork_after_low_memory_notification
    assert_single_threaded_script <<~'RUBY'
      exit 0 unless Process.respond_to?(:fork)

      context = MiniRacer::Context.new
      context.eval("var answer = 41")
      context.eval("answer += 1")
      context.low_memory_notification
      Process.warmup if Process.respond_to?(:warmup)

      pid = fork do
        Thread.new do
          sleep 3
          warn "child timed out"
          exit! 99
        end

        exit!(context.eval("answer") == 42 ? 0 : 1)
      end
      _, status = Process.wait2(pid)
      raise "child failed with status #{status.inspect}" unless status.success?
    RUBY
  end
end
