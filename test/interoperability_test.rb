require 'test_helper'

module Sqreen
  class InteroperabilityTest < Minitest::Test
    if ENV['LOAD_MINI_RACER'] == '1'
      require 'mini_racer'

      def test_mini_racer_interop
        ctx = ::MiniRacer::Context.new
        ctx.eval 'var adder = (a,b)=>a+b;'
        assert_equal 42, ctx.eval('adder(20,22)')

        simple_sq_mini_racer_op
      end

    elsif ENV['LOAD_THERUBYRACER'] == '1'
      require 'therubyracer'

      def test_therubyracer_interop
        cxt = V8::Context.new
        assert_equal 42, cxt.eval('7 * 6')

        simple_sq_mini_racer_op
      end
    end

    private

    def simple_sq_mini_racer_op
      sq_ctx = ::Sqreen::MiniRacer::Context.new
      sq_ctx.eval 'var adder = (a,b)=>a+b;'
      assert_equal 42, sq_ctx.eval('adder(20,22)')
    end
  end

end
