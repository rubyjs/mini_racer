require "mini_racer/version"
require "mini_racer_extension"
require "thread"
require "json"

module MiniRacer

  class Error < ::StandardError; end

  class ContextDisposedError < Error; end
  class SnapshotError < Error; end
  class PlatformAlreadyInitialized < Error; end

  class EvalError < Error; end
  class ParseError < EvalError; end
  class ScriptTerminatedError < EvalError; end
  class V8OutOfMemoryError < EvalError; end

  class FailedV8Conversion
    attr_reader :info
    def initialize(info)
      @info = info
    end
  end

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

  class Isolate
    def initialize(snapshot = nil)
      unless snapshot.nil? || snapshot.is_a?(Snapshot)
        raise ArgumentError, "snapshot must be a Snapshot object, passed a #{snapshot.inspect}"
      end

      # defined in the C class
      init_with_snapshot(snapshot)
    end
  end

  class Platform
    class << self
      def set_flags!(*args, **kwargs)
        flags_to_strings([args, kwargs]).each do |flag|
          # defined in the C class
          set_flag_as_str!(flag)
        end
      end

    private

      def flags_to_strings(flags)
        flags.flatten.map { |flag| flag_to_string(flag) }.flatten
      end

      # normalize flags to strings, and adds leading dashes if needed
      def flag_to_string(flag)
        if flag.is_a?(Hash)
          flag.map do |key, value|
            "#{flag_to_string(key)} #{value}"
          end
        else
          str = flag.to_s
          str = "--#{str}" unless str.start_with?('--')
          str
        end
      end
    end
  end

  # eval is defined in the C class
  class Context

    class ExternalFunction
      def initialize(name, callback, parent)
        unless String === name
          raise ArgumentError, "parent_object must be a String"
        end
        parent_object, _ , @name = name.rpartition(".")
        @callback = callback
        @parent = parent
        @parent_object_eval = nil
        @parent_object = nil

        unless parent_object.empty?
          @parent_object = parent_object

          @parent_object_eval = ""
          prev = ""
          first = true
          parent_object.split(".").each do |obj|
            prev << obj
            if first
              @parent_object_eval << "if (typeof #{prev} === 'undefined') { #{prev} = {} };\n"
            else
              @parent_object_eval << "#{prev} = #{prev} || {};\n"
            end
            prev << "."
            first = false
          end
          @parent_object_eval << "#{parent_object};"
        end
        notify_v8
      end
    end

    def initialize(options = nil)
      options ||= {}

      check_init_options!(options)

      @functions = {}
      @timeout = nil
      @max_memory = nil
      @current_exception = nil
      @timeout = options[:timeout]
      if options[:max_memory].is_a?(Numeric) && options[:max_memory] > 0
        @max_memory = options[:max_memory]
      end
      # false signals it should be fetched if requested
      @isolate = options[:isolate] || false
      @disposed = false

      @callback_mutex = Mutex.new
      @callback_running = false
      @thread_raise_called = false
      @eval_thread = nil

      # defined in the C class
      init_unsafe(options[:isolate], options[:snapshot])
    end

    def isolate
      return @isolate if @isolate != false
      # defined in the C class
      @isolate = create_isolate_value
    end

    def load(filename)
      # TODO do this native cause no need to allocate VALUE here
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
        raise ArgumentError("file_or_io")
      end

      write_heap_snapshot_unsafe(f)

    ensure
      f.close if implicit
    end

    def eval(str, options=nil)
      raise(ContextDisposedError, 'attempted to call eval on a disposed context!') if @disposed

      filename = options && options[:filename].to_s

      @eval_thread = Thread.current
      isolate_mutex.synchronize do
        @current_exception = nil
        timeout do
          eval_unsafe(str, filename)
        end
      end
    ensure
      @eval_thread = nil
    end

    def call(function_name, *arguments)
      raise(ContextDisposedError, 'attempted to call function on a disposed context!') if @disposed

      @eval_thread = Thread.current
      isolate_mutex.synchronize do
        timeout do
          call_unsafe(function_name, *arguments)
        end
      end
    ensure
      @eval_thread = nil
    end

    def dispose
      return if @disposed
      isolate_mutex.synchronize do
        dispose_unsafe
      end
      @disposed = true
      @isolate = nil # allow it to be garbage collected, if set
    end


    def attach(name, callback)
      raise(ContextDisposedError, 'attempted to call function on a disposed context!') if @disposed

      wrapped = lambda do |*args|
        begin

          r = nil

          begin
            @callback_mutex.synchronize{
              @callback_running = true
            }
            r = callback.call(*args)
          ensure
            @callback_mutex.synchronize{
              @callback_running = false
            }
          end

          # wait up to 2 seconds for this to be interrupted
          # will very rarely be called cause #raise is called
          # in another mutex
          @callback_mutex.synchronize {
            if @thread_raise_called
              sleep 2
            end
          }

          r

        ensure
          @callback_mutex.synchronize {
            @thread_raise_called = false
          }
        end
      end

      isolate_mutex.synchronize do
        external = ExternalFunction.new(name, wrapped, self)
        @functions["#{name}"] = external
      end
    end

  private

    def stop_attached
      @callback_mutex.synchronize{
        if @callback_running
          @eval_thread.raise ScriptTerminatedError, "Terminated during callback"
          @thread_raise_called = true
        end
      }
    end

    def timeout(&blk)
      return blk.call unless @timeout

      mutex = Mutex.new
      done = false

      rp,wp = IO.pipe

      t = Thread.new do
        begin
          result = IO.select([rp],[],[],(@timeout/1000.0))
          if !result
            mutex.synchronize do
              stop unless done
            end
          end
        rescue => e
          STDERR.puts e
          STDERR.puts "FAILED TO TERMINATE DUE TO TIMEOUT"
        end
      end

      rval = blk.call
      mutex.synchronize do
        done = true
      end

      wp.write("done")

      # ensure we do not leak a thread in state
      t.join

      rval

    end

    def check_init_options!(options)
      assert_option_is_nil_or_a('isolate', options[:isolate], Isolate)
      assert_option_is_nil_or_a('snapshot', options[:snapshot], Snapshot)

      if options[:isolate] && options[:snapshot]
        raise ArgumentError, 'can only pass one of isolate and snapshot options'
      end
    end

    def assert_option_is_nil_or_a(option_name, object, klass)
      unless object.nil? || object.is_a?(klass)
        raise ArgumentError, "#{option_name} must be a #{klass} object, passed a #{object.inspect}"
      end
    end
  end

  # `size` and `warmup!` public methods are defined in the C class
  class Snapshot
    def initialize(str = '')
      # ensure it first can load
      begin
        ctx = MiniRacer::Context.new
        ctx.eval(str)
      rescue MiniRacer::RuntimeError => e
        raise MiniRacer::SnapshotError.new, e.message, e.backtrace
      end

      @source = str

      # defined in the C class
      load(str)
    end

    def warmup!(src)
      # we have to do something here
      # we are bloating memory a bit but it is more correct
      # than hitting an exception when attempty to compile invalid source
      begin
        ctx = MiniRacer::Context.new
        ctx.eval(@source)
        ctx.eval(src)
      rescue MiniRacer::RuntimeError => e
        raise MiniRacer::SnapshotError.new, e.message, e.backtrace
      end

      warmup_unsafe!(src)
    end
  end
end
