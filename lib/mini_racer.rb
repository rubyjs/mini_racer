require "mini_racer/version"
require "mini_racer_extension"

module MiniRacer
  class JavaScriptError < StandardError; end

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
      if options
        @timeout = options[:timeout]
      end
    end

    def attach(name, callback)
      external = ExternalFunction.new(name, callback, self)
      @functions[name.to_s] = external
    end

  end

end
