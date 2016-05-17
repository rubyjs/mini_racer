require "mini_racer/version"
require "mini_racer_extension"
require "thread"

module MiniRacer

  class EvalError < StandardError; end

  class ScriptTerminatedError < EvalError; end
  class ParseError < EvalError; end

  class RuntimeError < EvalError
    def initialize(message)
      message, js_backtrace = message.split("\n", 2)
      if js_backtrace && !js_backtrace.empty?
        @js_backtrace = js_backtrace.split("\n")
        @js_backtrace.map!{|f| "JavaScript #{f.strip}"}
      else
        @js_backtrace = nil
      end

      super(message)
    end

    def backtrace
      val = super
      return unless val
      if @js_backtrace
        @js_backtrace + val
      else
        val
      end
    end

  end

  # helper class returned when we have a JavaScript function
  class JavaScriptFunction
    def to_s
      "JavaScript Function"
    end
  end

  # eval is defined in the C class
  class Context

    class ExternalFunction
      def initialize(name, callback, parent)
        @name = name
        @callback = callback
        @parent = parent
        notify_v8
      end
    end

    def initialize(options = nil)
      @functions = {}
      @lock = Mutex.new
      @timeout = nil
      @current_exception = nil

      if options
        @timeout = options[:timeout]
      end
    end

    def load(filename)
      # TODO do this native cause no need to allocate VALUE here
      eval(File.read(filename))
    end

    def eval(str)
      @lock.synchronize do
        @current_exception = nil
        eval_unsafe(str)
      end
    end

    def attach(name, callback)
      @lock.synchronize do
        external = ExternalFunction.new(name, callback, self)
        @functions[name.to_s] = external
      end
    end

  end

end
