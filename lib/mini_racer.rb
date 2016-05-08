require "mini_racer/version"
require "mini_racer_extension"

module MiniRacer
  class JavaScriptError < StandardError; end

  # eval is defined in the C class
  class Context
    def initialize(options = nil)

      @functions = {}

      if options
        @timeout = options[:timeout]
      end
    end

    def attach(name, callback)
      @functions[name.to_s] = callback
      notify(name.to_s)
    end

  end

end
