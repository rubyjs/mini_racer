# use bundle exec to run this script
#
require "securerandom"

context = nil

Thread
  .new do
    require "mini_racer"
    context = MiniRacer::Context.new
  end
  .join

Thread
  .new do
    context.low_memory_notification

    start_heap = context.heap_stats[:used_heap_size]

    context.eval("'#{"x" * 1_000_000_0}'")

    end_heap = context.heap_stats[:used_heap_size]

    p end_heap - start_heap
  end
  .join

Thread
  .new do
    10.times { context.low_memory_notification }
    end_heap = context.heap_stats[:used_heap_size]
    p end_heap
  end
  .join
exit
