engines = {
   mini_racer: proc { ExecJS::MiniRacerRuntime.new },
   therubyracer: proc { ExecJS::RubyRacerRuntime.new },
   rhino: proc { ExecJS::RubyRhinoRuntime.new },
   duktape: proc { ExecJS::DuktapeRuntime.new},
   node: proc { ExecJS::Runtimes::Node }
}

engine = ARGV[0]
unless engine && (execjs_engine = engines[engine.to_sym])
  STDERR.puts "Unknown engine try #{engines.keys.join(',')}"
  exit 1
end

unless engine == "node"
  require engine
end

puts "Benching with #{engine}"
require 'uglifier'

ExecJS.runtime = execjs_engine.call

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

