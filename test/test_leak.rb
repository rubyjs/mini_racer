$LOAD_PATH.unshift File.expand_path('../../lib', __FILE__)
require 'mini_racer'
require 'objspace'

def has_contexts
  ObjectSpace.each_object(MiniRacer::Context).count > 0
end

def clear
  while has_contexts
    GC.start
  end
end

def test
  context = MiniRacer::Context.new
  context.attach("add", proc{|a,b| a+b})
  context.eval('1+1')
  context.eval('"1"')
  context.eval('a=function(){}')
  context.eval('a={a: 1}')
  context.eval('a=[1,2,"3"]')
  context.eval('add(1,2)')
end


def start
  n = 100

  puts "Running #{n} contexts"

  n.times do
    test
    clear
  end

  puts "Ensuring garbage is collected"


  puts "Done"
end

start
