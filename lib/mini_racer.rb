require "mini_racer/version"
require "pathname"

module MiniRacer
  class Binary
    attr_reader :data

    def initialize(data)
      unless data.is_a?(String)
        raise TypeError, "wrong argument type #{data.class} (expected String)"
      end
      @data = data
    end
  end
end

if RUBY_ENGINE == "truffleruby"
  require "mini_racer/truffleruby"
else
  if ENV["LD_PRELOAD"].to_s.include?("malloc")
    require "mini_racer_extension"
  else
    require "mini_racer_loader"
    ext_filename = "mini_racer_extension.#{RbConfig::CONFIG["DLEXT"]}"
    ext_path =
      Gem.loaded_specs["mini_racer"].require_paths.map do |p|
        (p = Pathname.new(p)).absolute? ? p : Pathname.new(__dir__).parent + p
      end
    ext_found = ext_path.map { |p| p + ext_filename }.find { |p| p.file? }

    unless ext_found
      raise LoadError,
            "Could not find #{ext_filename} in #{ext_path.map(&:to_s)}"
    end
    MiniRacer::Loader.load(ext_found.to_s)
  end
end

require "json"
require "io/wait"

module MiniRacer
  class Error < ::StandardError
  end

  class ContextDisposedError < Error
  end
  class PlatformAlreadyInitialized < Error
  end
  class PauseTimeoutError < Error
  end

  class EvalError < Error
  end
  class ParseError < EvalError
  end
  class ScriptTerminatedError < EvalError
  end
  class V8OutOfMemoryError < EvalError
  end

  class RuntimeError < EvalError
    def initialize(message)
      message, *@frames = message.split("\n")
      @frames.map! { "JavaScript #{_1.strip}" }
      super(message)
    end

    def backtrace
      frames = super
      @frames + frames unless frames.nil?
    end
  end

  module ForkHooks
    def _fork
      paused = false
      MiniRacer.pause(timeout: MiniRacer.fork_hook_timeout)
      paused = true

      super
    ensure
      exception = $!
      if paused
        begin
          MiniRacer.resume
        rescue StandardError
          # Keep the original fork/pause failure.
          raise unless exception
        end
      end
    end
  end
  private_constant :ForkHooks

  @fork_hook_timeout = 5.0
  @fork_hooks_installed = false
  MAX_FORK_HOOK_TIMEOUT = 10 * 365 * 24 * 60 * 60
  private_constant :MAX_FORK_HOOK_TIMEOUT

  class << self
    attr_reader :fork_hook_timeout

    def install_fork_hooks!(timeout: 5.0)
      unless respond_to?(:pause) && respond_to?(:resume)
        raise NotImplementedError,
              "MiniRacer.pause/resume fork coordination is not available on this platform"
      end
      unless Process.respond_to?(:_fork, true)
        raise NotImplementedError,
              "Process._fork is not available on this platform"
      end
      unless timeout.nil?
        unless timeout.is_a?(Numeric)
          raise ArgumentError,
                "timeout must be nil or a finite number between 0 and 10 years"
        end
        timeout = timeout.to_f
        unless timeout.finite? && timeout >= 0 &&
                 timeout <= MAX_FORK_HOOK_TIMEOUT
          raise ArgumentError,
                "timeout must be nil or a finite number between 0 and 10 years"
        end
      end

      @fork_hook_timeout = timeout
      unless @fork_hooks_installed
        Process.singleton_class.prepend(ForkHooks)
        @fork_hooks_installed = true
      end
      true
    end
  end

  class ScriptError < EvalError
    def initialize(message)
      message, *@frames = message.split("\n")
      @frames.map! { "JavaScript #{_1.strip}" }
      super(message)
    end

    def backtrace
      frames = super || []
      @frames + frames
    end
  end

  class SnapshotError < Error
    def initialize(message)
      message, *@frames = message.split("\n")
      @frames.map! { "JavaScript #{_1.strip}" }
      super(message)
    end

    def backtrace
      frames = super
      @frames + frames unless frames.nil?
    end
  end

  class Context
    def load(filename)
      eval(File.read(filename))
    end

    def write_heap_snapshot(file_or_io)
      f = nil
      implicit = false

      if String === file_or_io
        f = File.open(file_or_io, "w")
        implicit = true
      else
        f = file_or_io
      end

      raise ArgumentError, "file_or_io" unless File === f

      f.write(heap_snapshot())
    ensure
      f.close if implicit
    end
  end
end
