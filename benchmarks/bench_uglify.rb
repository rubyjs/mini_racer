engine = ARGV[0] == "therubyracer" ? "therubyracer" : "MiniRacer"

if ARGV[0] == "rubyracer"
  require 'therubyracer'
  puts "Benching with therubyracer"
else
  require 'mini_racer'
  puts "Benching with MiniRacer"
end

require 'uglifier'

if ARGV[0] == "rubyracer"
  ExecJS.runtime = ExecJS::RubyRacerRuntime.new
else
  ExecJS.runtime = ExecJS::MiniRacerRuntime.new
end

start = Time.new
Uglifier.compile(File.read("discourse_app.js"))
puts "#{engine} minify discourse_app.js #{(Time.new - start)*1000}ms"

start = Time.new
Uglifier.compile(File.read("discourse_app_minified.js"))
puts "#{engine} minify discourse_app_minified.js #{(Time.new - start)*1000}ms"

start = Time.new
(0..1).map do
  Thread.new do
    Uglifier.compile(File.read("discourse_app.js"))
  end
end.each(&:join)

puts "#{engine} minify discourse_app.js twice (2 threads) #{(Time.new - start)*1000}ms"

