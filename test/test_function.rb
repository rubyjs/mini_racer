require 'test_helper'

class MiniRacerFunctionTest < Minitest::Test

    def test_fun
        context = MiniRacer::Context.new
        context.eval("function f(x) { return 'I need ' + x + ' galettes' }")
        assert_equal context.eval('f(10)'), "I need 10 galettes"
        
        assert_raises(ArgumentError) do
            context.function_call
        end

        count = 4
        res = context.function_call('f', count)
        assert_equal res, "I need #{count} galettes"
    end

    def test_fun2
        context = MiniRacer::Context.new
        context.eval("function f(x, y) { return 'I need ' + x + ' ' + y }")
        
        res = context.function_call('f', 3, "dogs")
        assert_equal res, "I need 3 dogs"
    end
end

