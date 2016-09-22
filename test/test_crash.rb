$LOAD_PATH.unshift File.expand_path('../../lib', __FILE__)
require 'mini_racer'

def test
  context = MiniRacer::Context.new(timeout: 10)
  context.attach("echo", proc{ |msg|
    GC.start
    msg
  })

  GC.disable
  100.times { 'foo' } # alloc a handful of objects
  GC.enable


  context.eval("while(true) echo('foo');") rescue nil

  # give some time to clean up
  puts "we are done"
end

test
GC.start

test

10.times{GC.start}
test

