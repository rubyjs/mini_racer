# use bundle exec to run this script
#
require 'securerandom'


    context = nil

    Thread.new do
      require "mini_racer"
      context = MiniRacer::Context.new
    end.join

    Thread.new do
      context.low_memory_notification

      start_heap = context.heap_stats[:used_heap_size]

      context.eval("'#{"x" * 1_000_000_0}'")


      end_heap = context.heap_stats[:used_heap_size]

      p end_heap - start_heap
    end.join

    Thread.new do
      10.times do
        context.low_memory_notification
      end
      end_heap = context.heap_stats[:used_heap_size]
      p end_heap
    end.join
    exit


ctx = nil

big_eval = +""

(0..100).map do |j|
  big_regex = (1..10000).map { |i| SecureRandom.hex }.join("|")
  big_eval << "X[#{j}] = /#{big_regex}/;\n"
end

big_eval = <<~JS
  const X = [];
  #{big_eval}

  function test(i, str) {
    return X[i].test(str);
  }

  null;
JS

Thread
  .new do
    require "mini_racer"
    ctx = MiniRacer::Context.new
    ctx.eval("var a = 1+1")
    ctx.eval(big_eval)
  end
  .join

3.times { GC.start }


Thread
  .new do
    10.times do
      10.times { |i| p ctx.eval "test(#{i}, '#{SecureRandom.hex}')" }
    end
  end
  .join

GC.start
