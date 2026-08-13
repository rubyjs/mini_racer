# frozen_string_literal: true

require "open3"
require "rbconfig"
require "tempfile"
require "test_helper"

class MiniRacerForkSafetyTest < Minitest::Test
  def assert_fork_script(script)
    skip "fork safety tests are only for CRuby" unless RUBY_ENGINE == "ruby"
    skip "fork is not available" unless Process.respond_to?(:fork)

    file = Tempfile.new(%w[mini_racer_fork_safety .rb])
    file.write(<<~RUBY)
      $LOAD_PATH.unshift #{File.expand_path("../lib", __dir__).inspect}
      require "mini_racer"

      Thread.new do
        sleep 15
        warn "fork safety script timed out"
        exit! 98
      end

      def child_watchdog
        Thread.new do
          sleep 3
          warn "child timed out"
          exit! 99
        end
      end

      #{script}
    RUBY
    file.close

    stdout, stderr, status = Open3.capture3(RbConfig.ruby, file.path)
    assert status.success?, <<~MSG
      fork safety script failed with status #{status.exitstatus}
      stdout:
      #{stdout}
      stderr:
      #{stderr}
    MSG
    [stdout, stderr]
  ensure
    file&.unlink
  end

  def test_fork_error_is_a_mini_racer_error
    assert_operator MiniRacer::ForkError, :<, MiniRacer::Error
  end

  def test_inherited_default_context_fails_promptly
    assert_fork_script <<~'RUBY'
      context = MiniRacer::Context.new
      raise "bad parent eval" unless context.eval("var answer = 42; answer") == 42

      pid = fork do
        child_watchdog
        begin
          context.eval("answer")
          warn "inherited eval unexpectedly succeeded"
          exit! 1
        rescue MiniRacer::ForkError => e
          unless e.message.include?("inherited default-platform context") &&
                 e.message.include?(":single_threaded")
            warn "bad fork error: #{e.message}"
            exit! 2
          end
          exit! 0
        end
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
      raise "parent context broke" unless context.eval("answer") == 42
    RUBY
  end

  def test_new_default_context_fails_after_parent_initialization
    assert_fork_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.eval("1 + 1")

      pid = fork do
        child_watchdog
        begin
          MiniRacer::Context.new
          warn "child context unexpectedly initialized"
          exit! 1
        rescue MiniRacer::ForkError => e
          unless e.message.include?("default platform was initialized before fork") &&
                 e.message.include?(":single_threaded")
            warn "bad platform fork error: #{e.message}"
            exit! 2
          end
          exit! 0
        end
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
      raise "parent context broke" unless context.eval("20 + 22") == 42
    RUBY
  end

  def test_snapshot_initialization_also_guards_the_default_platform
    assert_fork_script <<~'RUBY'
      MiniRacer::Snapshot.new("var snap = 42")

      pid = fork do
        child_watchdog
        begin
          MiniRacer::Snapshot.new
          warn "child snapshot unexpectedly initialized"
          exit! 1
        rescue MiniRacer::ForkError => e
          exit!(e.message.include?("default platform was initialized before fork") ? 0 : 2)
        end
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
    RUBY
  end

  def test_inherited_default_context_finalizer_does_not_hang
    assert_fork_script <<~'RUBY'
      require "weakref"

      context = MiniRacer::Context.new
      context.eval("1 + 1")
      weak_context = WeakRef.new(context)

      pid = fork do
        child_watchdog
        context = nil
        5.times do
          GC.start(full_mark: true, immediate_sweep: true)
          GC.compact if GC.respond_to?(:compact)
          break unless weak_context.weakref_alive?
        end
        raise "inherited context was not finalized" if weak_context.weakref_alive?
        # Intentionally fall off the end: normal child shutdown must also avoid
        # touching the inherited default platform or its missing threads.
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
      raise "parent context broke" unless context.eval("20 + 22") == 42
    RUBY
  end

  def test_inherited_default_context_dispose_and_stop_are_safe
    assert_fork_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.eval("var answer = 42")

      dispose_pid = fork do
        child_watchdog
        context.dispose
        begin
          context.eval("answer")
          exit! 1
        rescue MiniRacer::ContextDisposedError
          exit! 0
        end
      end
      _, dispose_status = Process.wait2(dispose_pid)
      raise "dispose child failed with #{dispose_status.inspect}" unless dispose_status.success?

      stop_pid = fork do
        child_watchdog
        begin
          context.stop
          exit! 1
        rescue MiniRacer::ForkError => e
          exit!(e.message.include?("inherited default-platform context") ? 0 : 2)
        end
      end
      _, stop_status = Process.wait2(stop_pid)
      raise "stop child failed with #{stop_status.inspect}" unless stop_status.success?
      raise "parent context broke" unless context.eval("answer") == 42
    RUBY
  end

  def test_default_platform_can_initialize_for_the_first_time_in_child
    assert_fork_script <<~'RUBY'
      pid = fork do
        child_watchdog
        context = MiniRacer::Context.new
        exit!(context.eval("6 * 7") == 42 ? 0 : 1)
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
    RUBY
  end

  def test_raw_fork_racing_first_default_initialization_never_hangs
    assert_fork_script <<~'RUBY'
      ready_r, ready_w = IO.pipe
      initializer = Thread.new do
        ready_w.write("x")
        ready_w.flush
        context = MiniRacer::Context.new
        raise "bad initializer result" unless context.eval("6 * 7") == 42
      end
      ready_r.read(1)

      pid = fork do
        child_watchdog
        begin
          context = MiniRacer::Context.new
          exit!(context.eval("6 * 7") == 42 ? 0 : 1)
        rescue MiniRacer::ForkError => e
          valid = e.message.include?("initialization was in progress") ||
                  e.message.include?("default platform was initialized before fork")
          exit!(valid ? 0 : 2)
        end
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
      raise "parent initializer did not finish" unless initializer.join(10)
    RUBY
  end

  def test_child_can_avoid_mini_racer_after_parent_default_initialization
    assert_fork_script <<~'RUBY'
      context = MiniRacer::Context.new
      context.eval("var answer = 42")

      pid = fork do
        child_watchdog
        exit! 0
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
      raise "parent context broke" unless context.eval("answer") == 42
    RUBY
  end

  def test_child_cannot_switch_an_inherited_default_platform
    assert_fork_script <<~'RUBY'
      MiniRacer::Context.new

      pid = fork do
        child_watchdog
        begin
          MiniRacer::Platform.set_flags!(:single_threaded)
          exit! 1
        rescue MiniRacer::PlatformAlreadyInitialized
          exit! 0
        end
      end
      _, status = Process.wait2(pid)
      raise "child failed with #{status.inspect}" unless status.success?
    RUBY
  end

  def test_fork_hooks_do_not_make_initialized_default_platform_reusable
    _stdout, stderr =
      assert_fork_script <<~'RUBY'
        MiniRacer.install_fork_hooks!(timeout: 1)
        context = MiniRacer::Context.new
        context.eval("var answer = 42")

        pid = fork do
          child_watchdog
          begin
            MiniRacer::Context.new
            exit! 1
          rescue MiniRacer::ForkError
            exit! 0
          end
        end
        _, status = Process.wait2(pid)
        raise "child failed with #{status.inspect}" unless status.success?
        raise "parent context broke" unless context.eval("answer") == 42
      RUBY

    assert_includes stderr, "Fork hooks only quiesce MiniRacer operations"
  end

  def test_default_platform_fork_hook_rejects_fork_from_callback
    assert_fork_script <<~'RUBY'
      MiniRacer.install_fork_hooks!(timeout: 1)
      context = MiniRacer::Context.new
      context.attach("try_fork", proc do
        begin
          fork { exit! 88 }
          "forked"
        rescue MiniRacer::RuntimeError => e
          raise unless e.message.include?("cannot pause")
          "rejected"
        end
      end)

      raise "fork was not rejected" unless context.eval("try_fork()") == "rejected"
      raise "context should still be usable" unless context.eval("1 + 1") == 2
    RUBY
  end

  def test_default_platform_hook_warning_is_emitted_once
    _stdout, stderr =
      assert_fork_script <<~'RUBY'
        MiniRacer.install_fork_hooks!(timeout: 1)
        MiniRacer.install_fork_hooks!(timeout: 2)
        raise "timeout was not updated" unless MiniRacer.fork_hook_timeout == 2.0
      RUBY

    assert_equal 1, stderr.scan("Fork hooks only quiesce MiniRacer operations").length
    assert_includes stderr, ":single_threaded"
  end

  def test_initialized_default_platform_gets_stronger_hook_warning
    _stdout, stderr =
      assert_fork_script <<~'RUBY'
        MiniRacer::Context.new
        MiniRacer.install_fork_hooks!(timeout: 1)
      RUBY

    assert_includes stderr, "default platform is already initialized"
  end

  def test_single_threaded_configuration_does_not_warn
    _stdout, stderr =
      assert_fork_script <<~'RUBY'
        MiniRacer::Platform.set_flags!(:single_threaded)
        MiniRacer.install_fork_hooks!(timeout: 1)
      RUBY

    refute_includes stderr, "Fork hooks only quiesce MiniRacer operations"
  end
end
