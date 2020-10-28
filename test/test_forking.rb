$LOAD_PATH.unshift File.expand_path('../../lib', __FILE__)
require 'mini_racer'

# MiniRacer::Platform.set_flags! :single_threaded

def trigger_gc
  puts "a"
  ctx = MiniRacer::Context.new
  puts "b"
  ctx.eval("var a = #{('x' * 100000).inspect}")
  puts "c"
  ctx.eval("a = undefined")
  puts "d"
  ctx.isolate.low_memory_notification
  puts "f"
  puts "done triggering"
  #ctx.dispose
end

puts "A"
trigger_gc
puts "B"
MiniRacer::Platform.terminate

pid = fork do
  puts "I AM HERE"
  puts Process.pid
  trigger_gc
  puts "done #{Process.pid}"
end

Process.wait pid
