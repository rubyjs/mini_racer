require 'test_helper'
require 'timeout'

class MiniRacerFunctionTest < Minitest::Test

    def test_fun
        context = MiniRacer::Context.new
        context.eval("function f(x) { return 'I need ' + x + ' foos' }")
        assert_equal context.eval('f(10)'), "I need 10 foos"

        assert_raises(ArgumentError) do
            context.function_call
        end

        count = 4
        res = context.function_call('f', count)
        assert_equal "I need #{count} foos", res
    end

    def test_non_existing_function
        context = MiniRacer::Context.new
        context.eval("function f(x) { return 'I need ' + x + ' foos' }")

        # f is defined, let's call g
        assert_raises(MiniRacer::RuntimeError) do
            context.function_call('g')
        end
    end

    def test_throwing_function
        context = MiniRacer::Context.new
        context.eval("function f(x) { throw new Error() }")

        # f should throw
        assert_raises(MiniRacer::RuntimeError) do
            context.function_call('f', 1)
        end
    end

    def test_args_types
        context = MiniRacer::Context.new
        context.eval("function f(x, y) { return 'I need ' + x + ' ' + y }")

        res = context.function_call('f', 3, "bars")
        assert_equal "I need 3 bars", res

        res = context.function_call('f', {a: 1}, "bars")
        assert_equal "I need [object Object] bars", res

        res = context.function_call('f', [1,2,3], "bars")
        assert_equal "I need 1,2,3 bars", res
    end

    def test_complex_return
        context = MiniRacer::Context.new
        context.eval("function f(x, y) { return { vx: x, vy: y, array: [x, y] } }")

        h = { "vx" => 3, "vy" => "bars", "array" => [3, "bars"] }
        res = context.function_call('f', 3, "bars")
        assert_equal h, res
    end

    def test_do_not_hang_with_concurrent_calls

        context = MiniRacer::Context.new
        context.eval("function f(x) { return 'I need ' + x + ' foos' }")

        thread_count = 2

        threads = []
        thread_count.times do
            threads << Thread.new do
                10.times do |i|
                    context.function_call("f", i)
                end
            end
        end

        joined_thread_count = 0
        for t in threads do
            joined_thread_count += 1
            t.join
        end

        # Dummy test, completing should be enough to show we don't hang
        self.assert_equal thread_count, joined_thread_count
    end
end

