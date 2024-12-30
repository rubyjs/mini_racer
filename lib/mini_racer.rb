require "mini_racer/version"
require "pathname"

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

require "thread"
require "json"
require "io/wait"

module MiniRacer
  class Error < ::StandardError; end

  class ContextDisposedError < Error; end
  class PlatformAlreadyInitialized < Error; end

  class EvalError < Error; end
  class ParseError < EvalError; end
  class ScriptTerminatedError < EvalError; end
  class V8OutOfMemoryError < EvalError; end

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

      if !(File === f)
        raise ArgumentError, "file_or_io"
      end

      f.write(heap_snapshot())
    ensure
      f.close if implicit
    end
  end
end
