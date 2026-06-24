#!/usr/bin/env ruby
# frozen_string_literal: true

require "bundler/setup"
require "digest"
require "fileutils"
require "json"
require "open-uri"
require "optparse"
require "rbconfig"
require "time"
require "mini_racer"

ROOT = File.expand_path("../..", __dir__)
RESOURCE_FILE = File.join(__dir__, "resources.json")
RESOURCES = JSON.parse(File.read(RESOURCE_FILE))
DEFAULT_BABEL = "babel_standalone_7_26_10"
DEFAULT_SOURCE = "pdfjs_worker_4_10_38"

DEFAULT_STABLE_ROUNDS = 7

CaseResult = Struct.new(:name, :iterations, :samples, keyword_init: true) do
  def median_seconds
    samples.sort[samples.length / 2]
  end

  def to_h
    elapsed = median_seconds
    {
      name: name,
      iterations: iterations,
      rounds: samples.length,
      total_ms: elapsed * 1000.0,
      ms_per_iter: elapsed * 1000.0 / iterations,
      samples_ms: samples.map { |sample| sample * 1000.0 }
    }
  end
end

options = {
  cache_dir: File.join(ROOT, "tmp", "benchmark", "transpile"),
  source: DEFAULT_SOURCE,
  iterations: Integer(ENV.fetch("BENCH_ITERATIONS", "3")),
  rounds: Integer(ENV.fetch("BENCH_ROUNDS", DEFAULT_STABLE_ROUNDS.to_s)),
  warmup: Integer(ENV.fetch("BENCH_WARMUP", "5")),
  timeout: Integer(ENV.fetch("BENCH_TIMEOUT", "300000")),
  only: nil,
  json: false,
  output: nil,
  fetch_only: false,
  list_resources: false,
  single_threaded: ENV["BENCH_SINGLE_THREADED"] == "1"
}

OptionParser.new do |parser|
  parser.banner = "Usage: bundle exec ruby benchmark/transpile/bench.rb [options]"

  parser.on("--source NAME", "Input resource from resources.json; default: #{DEFAULT_SOURCE}") do |value|
    options[:source] = value
  end

  parser.on("--iterations COUNT", Integer, "Iterations per timed sample; default: BENCH_ITERATIONS or 3") do |value|
    options[:iterations] = value
  end

  parser.on("--rounds COUNT", Integer, "Timed samples per case; default: BENCH_ROUNDS or #{DEFAULT_STABLE_ROUNDS}") do |value|
    options[:rounds] = value
  end

  parser.on("--warmup COUNT", Integer, "Untimed warmup iterations before timed samples; default: BENCH_WARMUP or 5") do |value|
    options[:warmup] = value
  end

  parser.on("--timeout MS", Integer, "MiniRacer timeout in milliseconds; default: BENCH_TIMEOUT or 300000") do |value|
    options[:timeout] = value
  end

  parser.on("--only REGEX", "Run only benchmark names matching REGEX") do |value|
    options[:only] = Regexp.new(value)
  end

  parser.on("--cache-dir PATH", "Resource cache directory; default: tmp/benchmark/transpile") do |value|
    options[:cache_dir] = value
  end

  parser.on("--list-resources", "List downloadable resources and exit") do
    options[:list_resources] = true
  end

  parser.on("--fetch-only", "Download/verify resources and exit") do
    options[:fetch_only] = true
  end

  parser.on("--single-threaded", "Run V8 on MiniRacer's single-threaded platform") do
    options[:single_threaded] = true
  end

  parser.on("--json", "Print JSON instead of human-readable output") do
    options[:json] = true
  end

  parser.on("--output PATH", "Write JSON results to PATH") do |value|
    options[:output] = value
  end
end.parse!

abort "--iterations must be positive" if options[:iterations] < 1
abort "--rounds must be positive" if options[:rounds] < 1
abort "--warmup must be non-negative" if options[:warmup] < 0
abort "unknown source resource: #{options[:source]}" unless RESOURCES.key?(options[:source])

MiniRacer::Platform.set_flags!(:single_threaded) if options[:single_threaded]

if options[:list_resources]
  RESOURCES.each do |name, resource|
    puts "#{name}"
    puts "  url:    #{resource.fetch("url")}"
    puts "  sha256: #{resource.fetch("sha256")}"
    puts "  bytes:  #{resource.fetch("bytes")}"
    puts "  #{resource["description"]}" if resource["description"]
  end
  exit
end

def cache_path(cache_dir, name, resource)
  uri_path = URI(resource.fetch("url")).path
  extension = File.extname(uri_path)
  File.join(cache_dir, "#{name}-#{resource.fetch("sha256")[0, 12]}#{extension}")
end

def fetch_resource(cache_dir, name)
  resource = RESOURCES.fetch(name)
  path = cache_path(cache_dir, name, resource)
  expected_sha = resource.fetch("sha256")

  if File.exist?(path)
    actual_sha = Digest::SHA256.file(path).hexdigest
    return path if actual_sha == expected_sha

    warn "discarding #{path}: expected #{expected_sha}, got #{actual_sha}"
    FileUtils.rm_f(path)
  end

  FileUtils.mkdir_p(cache_dir)
  tmp_path = "#{path}.#{$$}.tmp"
  warn "fetching #{name} from #{resource.fetch("url")}" unless ENV["QUIET_FETCH"]
  URI.open(resource.fetch("url"), "rb", read_timeout: 120) do |input|
    File.binwrite(tmp_path, input.read)
  end

  actual_sha = Digest::SHA256.file(tmp_path).hexdigest
  unless actual_sha == expected_sha
    FileUtils.rm_f(tmp_path)
    abort "#{name}: expected sha256 #{expected_sha}, got #{actual_sha}"
  end

  FileUtils.mv(tmp_path, path)
  path
ensure
  FileUtils.rm_f(tmp_path) if tmp_path && File.exist?(tmp_path)
end

babel_path = fetch_resource(options[:cache_dir], DEFAULT_BABEL)
source_path = fetch_resource(options[:cache_dir], options[:source])
exit if options[:fetch_only]

requested_rounds = options[:rounds]
if options[:rounds] > 1 && options[:rounds] < DEFAULT_STABLE_ROUNDS
  warn "raising transpile --rounds from #{options[:rounds]} to #{DEFAULT_STABLE_ROUNDS} for stable medians; use --rounds 1 for smoke runs"
  options[:rounds] = DEFAULT_STABLE_ROUNDS
end

babel_source = File.binread(babel_path)
transpile_source = File.binread(source_path)
source_resource = RESOURCES.fetch(options[:source])

BABEL_SETUP = <<~JS
  const __miniRacerBabelOptions = {
    presets: [["env", {
      targets: { chrome: "51" },
      modules: false,
      bugfixes: true
    }]],
    sourceType: "module",
    ast: false,
    sourceMaps: false,
    comments: false,
    compact: true
  };

  let __miniRacerTranspileSource = "";

  function __miniRacerSetTranspileSource(source) {
    __miniRacerTranspileSource = source;
    return source.length;
  }

  function __miniRacerTransformLength(source) {
    return Babel.transform(source, __miniRacerBabelOptions).code.length;
  }

  function __miniRacerTransformCachedLength() {
    return __miniRacerTransformLength(__miniRacerTranspileSource);
  }

  function __miniRacerTransformCode(source) {
    return Babel.transform(source, __miniRacerBabelOptions).code;
  }
JS

def new_context(timeout, babel_source, source = nil)
  ctx = MiniRacer::Context.new(timeout: timeout)
  ctx.eval(babel_source)
  ctx.eval(BABEL_SETUP)
  ctx.call("__miniRacerSetTranspileSource", source) if source
  ctx
end

BenchmarkCase = Struct.new(:name, :iterations, :block, keyword_init: true)

def measure_case(bench, warmup:, rounds:)
  samples = []

  warmup.times do
    GC.start
    bench.block.call
  end

  rounds.times do
    begin
      GC.start
      GC.disable
      started_at = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      bench.iterations.times { bench.block.call }
      samples << (Process.clock_gettime(Process::CLOCK_MONOTONIC) - started_at)
    ensure
      GC.enable
    end
  end

  samples
end

ctx = new_context(options[:timeout], babel_source, transpile_source)

cases = [
  BenchmarkCase.new(
    name: "babel/load_standalone",
    iterations: options[:iterations],
    block: lambda do
      MiniRacer::Context.new(timeout: options[:timeout]).eval(babel_source)
    end
  ),
  BenchmarkCase.new(
    name: "babel/transpile_cached_source_to_es6_length",
    iterations: options[:iterations],
    block: -> { ctx.call("__miniRacerTransformCachedLength") }
  ),
  BenchmarkCase.new(
    name: "babel/transpile_call_source_to_es6_length",
    iterations: options[:iterations],
    block: -> { ctx.call("__miniRacerTransformLength", transpile_source) }
  ),
  BenchmarkCase.new(
    name: "babel/transpile_call_source_to_es6_code",
    iterations: options[:iterations],
    block: -> { ctx.call("__miniRacerTransformCode", transpile_source).bytesize }
  )
]

selected = if options[:only]
  cases.select { |bench| bench.name.match?(options[:only]) }
else
  cases
end
abort "No benchmarks matched" if selected.empty?

results = []
selected.each do |bench|
  samples = measure_case(bench, warmup: options[:warmup], rounds: options[:rounds])
  result = CaseResult.new(name: bench.name, iterations: bench.iterations, samples: samples).to_h
  results << result
  unless options[:json]
    puts "%-48s n=%-4d total=%10.3fms per=%10.3fms" % [
      result.fetch(:name),
      result.fetch(:iterations),
      result.fetch(:total_ms),
      result.fetch(:ms_per_iter)
    ]
  end
end

payload = {
  metadata: {
    mini_racer_version: MiniRacer::VERSION,
    ruby_version: RUBY_DESCRIPTION,
    platform: RbConfig::CONFIG["platform"],
    timestamp: Time.now.utc.iso8601,
    babel_resource: DEFAULT_BABEL,
    source_resource: options[:source],
    source_url: source_resource.fetch("url"),
    source_sha256: source_resource.fetch("sha256"),
    source_bytes: transpile_source.bytesize,
    target: "Babel preset-env to Chrome 51 / ES2015-ish, modules preserved",
    single_threaded: options[:single_threaded],
    iterations: options[:iterations],
    requested_rounds: requested_rounds,
    rounds: options[:rounds],
    warmup: options[:warmup]
  },
  benchmarks: results
}

if options[:output]
  File.write(options[:output], JSON.pretty_generate(payload) << "\n")
end

if options[:json]
  puts JSON.pretty_generate(payload)
else
  puts
  puts "source: #{options[:source]}"
  puts "url:    #{source_resource.fetch("url")}"
  puts "sha256: #{source_resource.fetch("sha256")}"
  puts "bytes:  #{transpile_source.bytesize}"
  puts "single-threaded: #{options[:single_threaded]}"
end
