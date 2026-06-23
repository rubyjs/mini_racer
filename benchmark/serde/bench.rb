#!/usr/bin/env ruby
# frozen_string_literal: true

require "bundler/setup"
require "json"
require "optparse"
require "rbconfig"
require "time"
require "mini_racer"

BenchmarkCase = Struct.new(:name, :iterations, :block, keyword_init: true)

class Suite
  CLOCK = Process::CLOCK_MONOTONIC

  attr_reader :cases

  def initialize(scale:, warmup:, rounds:)
    @scale = scale
    @warmup = warmup
    @rounds = rounds
    @cases = []
  end

  def add(name, iterations, &block)
    scaled_iterations = [(iterations * @scale).round, 1].max
    @cases << BenchmarkCase.new(name: name, iterations: scaled_iterations, block: block)
  end

  def run(selected_cases, output: $stdout, quiet: false)
    results = []

    selected_cases.each do |bench|
      samples = []

      @rounds.times do
        begin
          # Disable GC during each timed sample so allocation changes show up in the
          # measured boundary work instead of as noisy, schedule-dependent GC pauses.
          GC.start
          GC.disable

          @warmup.times { bench.block.call }

          started_at = Process.clock_gettime(CLOCK)
          bench.iterations.times { bench.block.call }
          samples << (Process.clock_gettime(CLOCK) - started_at)
        ensure
          GC.enable
        end
      end

      elapsed = samples.sort[samples.length / 2]
      sample_ms = samples.map { |sample| sample * 1000.0 }
      result = {
        name: bench.name,
        iterations: bench.iterations,
        rounds: @rounds,
        total_ms: elapsed * 1000.0,
        ms_per_iter: elapsed * 1000.0 / bench.iterations,
        samples_ms: sample_ms
      }
      results << result

      unless quiet
        output.puts("%-42s n=%-8d total=%10.3fms per=%10.6fms" % [
          result[:name],
          result[:iterations],
          result[:total_ms],
          result[:ms_per_iter]
        ])
      end
    end

    results
  end
end

options = {
  scale: Float(ENV.fetch("BENCH_SCALE", "1.0")),
  warmup: Integer(ENV.fetch("BENCH_WARMUP", "5")),
  rounds: Integer(ENV.fetch("BENCH_ROUNDS", "1")),
  only: nil,
  list: false,
  json: false,
  output: nil,
  compare: nil,
  single_threaded: ENV["BENCH_SINGLE_THREADED"] == "1"
}

OptionParser.new do |parser|
  parser.banner = "Usage: bundle exec ruby benchmark/serde/bench.rb [options]"

  parser.on("--only REGEX", "Run only benchmark names matching REGEX") do |value|
    options[:only] = Regexp.new(value)
  end

  parser.on("--scale SCALE", Float, "Multiply iteration counts; default: BENCH_SCALE or 1.0") do |value|
    options[:scale] = value
  end

  parser.on("--warmup COUNT", Integer, "Warmup iterations per case; default: BENCH_WARMUP or 5") do |value|
    options[:warmup] = value
  end

  parser.on("--rounds COUNT", Integer, "Timed samples per case; default: BENCH_ROUNDS or 1") do |value|
    options[:rounds] = value
  end

  parser.on("--json", "Print JSON instead of human-readable output") do
    options[:json] = true
  end

  parser.on("--output PATH", "Write JSON results to PATH") do |value|
    options[:output] = value
  end

  parser.on("--compare PATH", "Compare current run against JSON results from PATH") do |value|
    options[:compare] = value
  end

  parser.on("--single-threaded", "Run V8 on MiniRacer's single-threaded platform") do
    options[:single_threaded] = true
  end

  parser.on("--list", "List benchmark names and exit") do
    options[:list] = true
  end
end.parse!

abort "--rounds must be positive" if options[:rounds] < 1
abort "--warmup must be non-negative" if options[:warmup] < 0
abort "--scale must be positive" if options[:scale] <= 0

MiniRacer::Platform.set_flags!(:single_threaded) if options[:single_threaded]

suite = Suite.new(scale: options[:scale], warmup: options[:warmup], rounds: options[:rounds])
ctx = MiniRacer::Context.new(timeout: 120_000)

ctx.eval(<<~JS)
  function noop() {
    return 1;
  }

  function id(value) {
    return value;
  }

  function add(a, b) {
    return a + b;
  }

  function intArray(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push(i);
    return array;
  }

  function floatArray(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push(i + 0.5);
    return array;
  }

  function asciiStringArray(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push("x" + i);
    return array;
  }

  function latin1StringArray(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push("é" + i);
    return array;
  }

  function utf16StringArray(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push("Ā" + i);
    return array;
  }

  function emojiStringArray(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push("😀" + i);
    return array;
  }

  function objectOfInts(n) {
    const object = {};
    for (let i = 0; i < n; i++) object["k" + i] = i;
    return object;
  }

  function arrayOfObjects(n) {
    const array = [];
    for (let i = 0; i < n; i++) array.push({ id: i, name: "x" + i });
    return array;
  }

  function callRubyEchoMany(n, value) {
    let result;
    for (let i = 0; i < n; i++) result = rubyEcho(value);
    return result;
  }

  function callRubyAddMany(n) {
    let result = 0;
    for (let i = 0; i < n; i++) result = rubyAdd(result, 1);
    return result;
  }
JS

ctx.attach("rubyEcho", proc { |value| value })
ctx.attach("rubyAdd", proc { |a, b| a + b })

ruby_ints_10k = Array.new(10_000) { |i| i }
ruby_floats_10k = Array.new(10_000) { |i| i + 0.5 }
ruby_strings_10k = Array.new(10_000) { |i| "x#{i}" }
ruby_utf8_strings_10k = Array.new(10_000) { |i| "Ā#{i}" }
ruby_emoji_strings_10k = Array.new(10_000) { |i| "😀#{i}" }
ruby_hash_1k = 1_000.times.each_with_object({}) { |i, h| h["k#{i}"] = i }
ruby_array_of_hashes_1k = Array.new(1_000) { |i| { "id" => i, "name" => "x#{i}" } }

# JS -> Ruby deserialization. The first three cases mirror the numeric/string
# shape of the synthetic regression repro; the UTF-8 cases exercise V8's
# UTF-16LE serialized strings that Ruby exports to UTF-8.
suite.add("js_to_ruby/return_10k_ints", 100) { ctx.call("intArray", 10_000) }
suite.add("js_to_ruby/return_10k_floats", 100) { ctx.call("floatArray", 10_000) }
suite.add("js_to_ruby/return_10k_ascii_strings", 100) { ctx.call("asciiStringArray", 10_000) }
suite.add("js_to_ruby/return_10k_latin1_strings", 50) { ctx.call("latin1StringArray", 10_000) }
suite.add("js_to_ruby/return_10k_utf8_bmp_strings", 50) { ctx.call("utf16StringArray", 10_000) }
suite.add("js_to_ruby/return_10k_utf8_emoji_strings", 50) { ctx.call("emojiStringArray", 10_000) }
suite.add("js_to_ruby/return_object_1k_ints", 200) { ctx.call("objectOfInts", 1_000) }
suite.add("js_to_ruby/return_array_1k_objects", 100) { ctx.call("arrayOfObjects", 1_000) }

# Ruby -> JS serialization plus reply deserialization through Context#call.
suite.add("ruby_to_js/call_no_args", 100_000) { ctx.call("noop") }
suite.add("ruby_to_js/call_int_arg", 100_000) { ctx.call("id", 123) }
suite.add("ruby_to_js/call_short_string", 100_000) { ctx.call("id", "hello") }
suite.add("ruby_to_js/call_utf8_string", 100_000) { ctx.call("id", "Ā hello") }
suite.add("ruby_to_js/call_emoji_string", 100_000) { ctx.call("id", "😀 hello") }
suite.add("ruby_to_js/roundtrip_10k_ints", 100) { ctx.call("id", ruby_ints_10k) }
suite.add("ruby_to_js/roundtrip_10k_floats", 100) { ctx.call("id", ruby_floats_10k) }
suite.add("ruby_to_js/roundtrip_10k_strings", 50) { ctx.call("id", ruby_strings_10k) }
suite.add("ruby_to_js/roundtrip_10k_utf8_bmp_strings", 50) { ctx.call("id", ruby_utf8_strings_10k) }
suite.add("ruby_to_js/roundtrip_10k_utf8_emoji_strings", 50) { ctx.call("id", ruby_emoji_strings_10k) }
suite.add("ruby_to_js/roundtrip_hash_1k", 100) { ctx.call("id", ruby_hash_1k) }
suite.add("ruby_to_js/roundtrip_array_1k_hashes", 50) { ctx.call("id", ruby_array_of_hashes_1k) }

# JS -> Ruby callback path. Each iteration performs 1,000 callbacks.
suite.add("callback/1000_echo_int", 50) { ctx.call("callRubyEchoMany", 1_000, 123) }
suite.add("callback/1000_echo_short_string", 50) { ctx.call("callRubyEchoMany", 1_000, "hello") }
suite.add("callback/1000_echo_utf8_string", 50) { ctx.call("callRubyEchoMany", 1_000, "Ā hello") }
suite.add("callback/1000_echo_emoji_string", 50) { ctx.call("callRubyEchoMany", 1_000, "😀 hello") }
suite.add("callback/1000_add_ints", 50) { ctx.call("callRubyAddMany", 1_000) }

selected_cases = if options[:only]
  suite.cases.select { |bench| bench.name.match?(options[:only]) }
else
  suite.cases
end

if options[:list]
  selected_cases.each { |bench| puts bench.name }
  exit
end

abort "No benchmarks matched" if selected_cases.empty?

metadata = {
  mini_racer_version: MiniRacer::VERSION,
  ruby_version: RUBY_DESCRIPTION,
  platform: RbConfig::CONFIG["platform"],
  timestamp: Time.now.utc.iso8601,
  scale: options[:scale],
  warmup: options[:warmup],
  rounds: options[:rounds],
  single_threaded: options[:single_threaded]
}

unless options[:json]
  puts "mini_racer #{MiniRacer::VERSION}"
  puts "ruby       #{RUBY_DESCRIPTION}"
  puts "scale      #{options[:scale]}"
  puts "rounds     #{options[:rounds]}"
  puts "single     #{options[:single_threaded]}"
  puts
end

results = suite.run(selected_cases, quiet: options[:json])
payload = { metadata: metadata, benchmarks: results }

if options[:compare]
  baseline = JSON.parse(File.read(options[:compare]), symbolize_names: true)
  baseline_by_name = baseline.fetch(:benchmarks).to_h { |row| [row.fetch(:name), row] }
  comparisons = results.filter_map do |row|
    old = baseline_by_name[row.fetch(:name)]
    next unless old

    old_value = old[:ms_per_iter] || old[:per_ms]
    next unless old_value

    old_per = old_value.to_f
    new_per = row.fetch(:ms_per_iter).to_f
    next if old_per.zero?

    {
      name: row.fetch(:name),
      baseline_ms_per_iter: old_per,
      current_ms_per_iter: new_per,
      ratio: new_per / old_per,
      change_pct: ((new_per / old_per) - 1.0) * 100.0
    }
  end
  payload[:comparisons] = comparisons

  unless options[:json]
    puts
    puts "Comparison against #{options[:compare]}"
    comparisons.each do |row|
      direction = row[:change_pct] <= 0 ? "faster" : "slower"
      puts "%-42s %8.3fms -> %8.3fms  %+7.2f%% %s" % [
        row[:name],
        row[:baseline_ms_per_iter],
        row[:current_ms_per_iter],
        row[:change_pct],
        direction
      ]
    end
  end
end

if options[:output]
  File.write(options[:output], JSON.pretty_generate(payload) << "\n")
end

puts JSON.pretty_generate(payload) if options[:json]
