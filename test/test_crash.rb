$LOAD_PATH.unshift File.expand_path('../../lib', __FILE__)
require 'mini_racer'

context = MiniRacer::Context.new(timeout: 10)
context.attach("echo", proc{|msg| GC.start; p msg; msg})
GC.disable
100.times { 'foo' } # alloc a handful of objects
GC.enable

context.eval("while(true) echo('foo');")
