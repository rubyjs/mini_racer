require 'mini_racer'
MiniRacer::Platform.set_flags! :perf_basic_prof
c = MiniRacer::Context.new
res = nil
script = File.read('small.json');
c.eval <<JS
var data = #{script};
function foo() { return data; }
JS
3000.times do
  res = c.call('foo')
end
p res.size
