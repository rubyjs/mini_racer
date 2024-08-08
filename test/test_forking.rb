# use bundle exec to run this script
require "mini_racer"

MiniRacer::Platform.set_flags! :single_threaded

@ctx = MiniRacer::Context.new
@ctx.eval("var a = 1+1")

def trigger_gc
  ctx = MiniRacer::Context.new
  ctx.eval("var a = #{("x" * 100_000).inspect}")
  ctx.eval("a = undefined")
  ctx.isolate.low_memory_notification
end

@ctx.eval("var b=100")

trigger_gc

MiniRacer::Context.new.dispose

if Process.respond_to?(:fork)
  Process.wait fork {
                 puts "after fork"
                 puts @ctx.eval("a")
                 puts @ctx.eval("b")
                 trigger_gc
                 puts @ctx.eval("a")
                 puts @ctx.eval("b")
                 @ctx.dispose
                 puts "done #{Process.pid}"
               }
end
