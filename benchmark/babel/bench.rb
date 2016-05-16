unless defined? Bundler
  system 'bundle'
  exec 'bundle exec ruby bench.rb'
end

require 'mini_racer'


ctx2 = MiniRacer::Context.new

start = Time.now
ctx2.eval(File.read('./helper_files/babel.js'))
puts "#{(Time.now - start) * 1000} load babel"

composer = File.read('./helper_files/composer.js.es6').inspect
str = "babel.transform(#{composer}, {ast: false, whitelist: ['es6.constants', 'es6.properties.shorthand', 'es6.arrowFunctions', 'es6.blockScoping', 'es6.destructuring', 'es6.spread', 'es6.parameters', 'es6.templateLiterals', 'es6.regex.unicode', 'es7.decorators', 'es6.classes']})['code']"


10.times do

  start = Time.now
  ctx2.eval str

  puts "mini racer #{(Time.now - start) * 1000} transform"

end

str = "for(var i=0;i<10;i++){#{str}}"

10.times do

  start = Time.now
  ctx2.eval str

  puts "mini racer 10x #{(Time.now - start) * 1000} transform"

end

