# frozen_string_literal: true

module MiniRacer

  class Context

    class ExternalFunction
      private

      def notify_v8
        name = @name.encode(::Encoding::UTF_8)
        wrapped = lambda do |*args|
          converted = @parent.send(:convert_js_to_ruby, args)
          begin
            result = @callback.call(*converted)
          rescue Polyglot::ForeignException => e
            e = RuntimeError.new(e.message)
            e.set_backtrace(e.backtrace)
            @parent.instance_variable_set(:@current_exception, e)
            raise e
          rescue => e
            @parent.instance_variable_set(:@current_exception, e)
            raise e
          end
          @parent.send(:convert_ruby_to_js, result)
        end

        if @parent_object.nil?
          # set global name to proc
          result = @parent.eval_in_context('this')
          result[name] = wrapped
        else
          parent_object_eval = @parent_object_eval.encode(::Encoding::UTF_8)
          begin
            result = @parent.eval_in_context(parent_object_eval)
          rescue Polyglot::ForeignException, StandardError => e
            raise ParseError, "Was expecting #{@parent_object} to be an object", e.backtrace
          end
          result[name] = wrapped
          # set evaluated object results name to proc
        end
      end
    end

    def heap_stats
      {
        total_physical_size: 0,
        total_heap_size_executable: 0,
        total_heap_size: 0,
        used_heap_size: 0,
        heap_size_limit: 0,
      }
    end

    def stop
      if @entered
        @context.stop
        @stopped = true
        stop_attached
      end
    end

    private

    @context_initialized = false
    @use_strict = false

    def init_unsafe(isolate, snapshot)
      unless defined?(Polyglot::InnerContext)
        raise "TruffleRuby #{RUBY_ENGINE_VERSION} does not have support for inner contexts, use a more recent version"
      end

      unless Polyglot.languages.include? "js"
        warn "You also need to install the 'js' component with 'gu install js' on GraalVM 22.2+", uplevel: 0 if $VERBOSE
      end

      @context = Polyglot::InnerContext.new(on_cancelled: -> { 
        raise ScriptTerminatedError, 'JavaScript was terminated (either by timeout or explicitly)' 
      })
      Context.instance_variable_set(:@context_initialized, true)
      @js_object = @context.eval('js', 'Object')
      @isolate_mutex = Mutex.new
      @stopped = false
      @entered = false
      @has_entered = false
      @current_exception = nil
      if isolate && snapshot
        isolate.instance_variable_set(:@snapshot, snapshot)
      end
      if snapshot
        @snapshot = snapshot
      elsif isolate
        @snapshot = isolate.instance_variable_get(:@snapshot)
      else
        @snapshot = nil
      end
      @is_object_or_array_func, @is_time_func, @js_date_to_time_func, @is_symbol_func, @js_symbol_to_symbol_func, @js_new_date_func, @js_new_array_func = eval_in_context <<-CODE
        [
          (x) => { return (x instanceof Object || x instanceof Array) && !(x instanceof Date) && !(x instanceof Function) },
          (x) => { return x instanceof Date },
          (x) => { return x.getTime(x) },
          (x) => { return typeof x === 'symbol' },
          (x) => { var r = x.description; return r === undefined ? 'undefined' : r },
          (x) => { return new Date(x) },
          (x) => { return new Array(x) },
        ]
      CODE
    end

    def dispose_unsafe
      @context.close
    end

    def eval_unsafe(str, filename)
      @entered = true
      if !@has_entered && @snapshot
        snapshot_src = encode(@snapshot.instance_variable_get(:@source))
        begin
          eval_in_context(snapshot_src, filename)
        rescue Polyglot::ForeignException => e
          raise RuntimeError, e.message, e.backtrace
        end
      end
      @has_entered = true
      raise RuntimeError, "TruffleRuby does not support eval after stop" if @stopped
      raise TypeError, "wrong type argument #{str.class} (should be a string)" unless str.is_a?(String)
      raise TypeError, "wrong type argument #{filename.class} (should be a string)" unless filename.nil? || filename.is_a?(String)

      str = encode(str)
      begin
        translate do
          eval_in_context(str, filename)
        end
      rescue Polyglot::ForeignException => e
        raise RuntimeError, e.message, e.backtrace
      rescue ::RuntimeError => e
        if @current_exception
          e = @current_exception
          @current_exception = nil
          raise e
        else
          raise e, e.message
        end
      end
    ensure
      @entered = false
    end

    def call_unsafe(function_name, *arguments)
      @entered = true
      if !@has_entered && @snapshot
        src = encode(@snapshot.instance_variable_get(:source))
        begin
          eval_in_context(src)
        rescue Polyglot::ForeignException => e
          raise RuntimeError, e.message, e.backtrace
        end
      end
      @has_entered = true
      raise RuntimeError, "TruffleRuby does not support call after stop" if @stopped
      begin
        translate do
          function = eval_in_context(function_name)
          function.call(*convert_ruby_to_js(arguments))
        end
      rescue Polyglot::ForeignException => e
        raise RuntimeError, e.message, e.backtrace
      end
    ensure
      @entered = false
    end

    def create_isolate_value
      # Returning a dummy object since TruffleRuby does not have a 1-1 concept with isolate.
      # However, code and ASTs are shared between contexts.
      Isolate.new
    end

    def isolate_mutex
      @isolate_mutex
    end

    def translate
      convert_js_to_ruby yield
    rescue Object => e
      message = e.message
      if @current_exception
        raise @current_exception
      elsif e.message && e.message.start_with?('SyntaxError:')
        error_class = MiniRacer::ParseError
      elsif e.is_a?(MiniRacer::ScriptTerminatedError)
        error_class = MiniRacer::ScriptTerminatedError
      else
        error_class = MiniRacer::RuntimeError
      end

      if error_class == MiniRacer::RuntimeError
        bls = e.backtrace_locations&.select { |bl| bl&.source_location&.language == 'js' }
        if bls && !bls.empty?
          if '(eval)' != bls[0].path
            message = "#{e.message}\n at #{bls[0]}\n" + bls[1..].map(&:to_s).join("\n")
          else
            message = "#{e.message}\n" + bls.map(&:to_s).join("\n")
          end
        end
        raise error_class, message
      else
        raise error_class, message, e.backtrace
      end
    end

    def convert_js_to_ruby(value)
      case value
      when true, false, Integer, Float
        value
      else
        if value.nil?
          nil
        elsif value.respond_to?(:call)
          MiniRacer::JavaScriptFunction.new
        elsif value.respond_to?(:to_str)
          value.to_str.dup
        elsif value.respond_to?(:to_ary)
          value.to_ary.map do |e|
            if e.respond_to?(:call)
              nil
            else
              convert_js_to_ruby(e)
            end
          end
        elsif time?(value)
          js_date_to_time(value)
        elsif symbol?(value)
          js_symbol_to_symbol(value)
        else
          object = value
          h = {}
          object.instance_variables.each do |member|
            v = object[member]
            unless v.respond_to?(:call)
              h[member.to_s] = convert_js_to_ruby(v)
            end
          end
          h
        end
      end
    end

    def object_or_array?(val)
      @is_object_or_array_func.call(val)
    end

    def time?(value)
      @is_time_func.call(value)
    end

    def js_date_to_time(value)
      millis = @js_date_to_time_func.call(value)
      Time.at(Rational(millis, 1000))
    end

    def symbol?(value)
      @is_symbol_func.call(value)
    end

    def js_symbol_to_symbol(value)
      @js_symbol_to_symbol_func.call(value).to_s.to_sym
    end

    def js_new_date(value)
      @js_new_date_func.call(value)
    end

    def js_new_array(size)
      @js_new_array_func.call(size)
    end

    def convert_ruby_to_js(value)
      case value
      when nil, true, false, Integer, Float
        value
      when Array
        ary = js_new_array(value.size)
        value.each_with_index do |v, i|
          ary[i] = convert_ruby_to_js(v)
        end
        ary
      when Hash
        h = @js_object.new
        value.each_pair do |k, v|
          h[convert_ruby_to_js(k.to_s)] = convert_ruby_to_js(v)
        end
        h
      when String, Symbol
        Truffle::Interop.as_truffle_string value
      when Time
        js_new_date(value.to_f * 1000)
      when DateTime
        js_new_date(value.to_time.to_f * 1000)
      else
        "Undefined Conversion"
      end
    end

    def encode(string)
      raise ArgumentError unless string
      string.encode(::Encoding::UTF_8)
    end

    class_eval <<-'RUBY', "(mini_racer)", 1
        def eval_in_context(code, file = nil); code = ('"use strict";' + code) if Context.instance_variable_get(:@use_strict); @context.eval('js', code, file || '(mini_racer)'); end
    RUBY

  end

  class Isolate
    def init_with_snapshot(snapshot)
      # TruffleRuby does not have a 1-1 concept with isolate.
      # However, isolate can hold a snapshot, and code and ASTs are shared between contexts.
      @snapshot = snapshot
    end

    def low_memory_notification
      GC.start
    end

    def idle_notification(idle_time)
      true
    end
  end

  class Platform
    def self.set_flag_as_str!(flag)
      raise TypeError, "wrong type argument #{flag.class} (should be a string)" unless flag.is_a?(String)
      raise MiniRacer::PlatformAlreadyInitialized, "The platform is already initialized." if Context.instance_variable_get(:@context_initialized)
      Context.instance_variable_set(:@use_strict, true) if "--use_strict" == flag
    end
  end

  class Snapshot
    def load(str)
      raise TypeError, "wrong type argument #{str.class} (should be a string)" unless str.is_a?(String)
      # Intentionally noop since TruffleRuby mocks the snapshot API
    end

    def warmup_unsafe!(src)
      raise TypeError, "wrong type argument #{src.class} (should be a string)" unless src.is_a?(String)
      # Intentionally noop since TruffleRuby mocks the snapshot API
      # by replaying snapshot source before the first eval/call
      self
    end
  end
end
