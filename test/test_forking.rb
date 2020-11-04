$LOAD_PATH.unshift File.expand_path('../../lib', __FILE__)
require 'mini_racer'


# MiniRacer::Platform.set_flags! :single_threaded
#

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
  ctx.dispose
  puts "disposed"
end

trigger_gc

# terminate is broken inside libv8 for now.
# MiniRacer::Platform.terminate

pid = fork do
  puts Process.pid
  trigger_gc
  exit
end

puts "waiting on #{pid}"
# this can hang erratically, bug in mini_racer
Process.wait pid
