require "mini_racer/version"
require "mini_racer_extension"

module MiniRacer
  class JavaScriptError < StandardError; end

  # eval is defined in the C class
  class Context
    def initialize(options = nil)
      if options
        @timeout = options[:timeout]
      end
    end
  end

end
