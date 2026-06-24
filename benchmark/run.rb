#!/usr/bin/env ruby
# frozen_string_literal: true

require "bundler/setup"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "rbconfig"
require "shellwords"
require "time"

ROOT = File.expand_path("..", __dir__)

Job = Struct.new(:name, :tags, :script, :args, keyword_init: true)
Target = Struct.new(:name, :ref, :dir, :working_tree, keyword_init: true)

JOBS = [
  Job.new(
    name: "serde/default",
    tags: %w[all serde boundary default],
    script: "benchmark/serde/bench.rb",
    args: []
  ),
  Job.new(
    name: "serde/single-threaded",
    tags: %w[all serde boundary single-threaded single],
    script: "benchmark/serde/bench.rb",
    args: ["--single-threaded"]
  ),
  Job.new(
    name: "transpile/default",
    tags: %w[all transpile realworld default],
    script: "benchmark/transpile/bench.rb",
    args: []
  ),
  Job.new(
    name: "transpile/single-threaded",
    tags: %w[all transpile realworld single-threaded single],
    script: "benchmark/transpile/bench.rb",
    args: ["--single-threaded"]
  )
].freeze

ALL_TAGS = JOBS.flat_map(&:tags).uniq.sort.freeze
WORKING_REFS = %w[working worktree current .].freeze

options = {
  tags: ["all"],
  git_refs: nil,
  worktree_root: File.join(ROOT, "tmp", "benchmark", "worktrees"),
  setup_git_refs: true,
  list: false,
  json: false,
  output: nil,
  rounds: nil,
  warmup: nil,
  only: nil,
  quick: false,
  serde_scale: nil,
  transpile_iterations: nil,
  transpile_source: nil,
  transpile_cache_dir: nil
}

OptionParser
  .new do |parser|
    parser.banner = "Usage: bundle exec ruby benchmark/run.rb [options]"

    parser.on(
      "--tags TAGS",
      "Comma-separated benchmark tag filter; default: all"
    ) { |v| options[:tags] = v.split(",").map(&:strip).reject(&:empty?) }
    parser.on(
      "--git-tags REFS",
      "Comma-separated git tags/refs to benchmark; use 'working' for this checkout"
    ) { |v| options[:git_refs] = v.split(",").map(&:strip).reject(&:empty?) }
    parser.on(
      "--git-refs REFS",
      "Alias for --git-tags; accepts tags, branches, SHAs, and 'working'"
    ) { |v| options[:git_refs] = v.split(",").map(&:strip).reject(&:empty?) }
    parser.on(
      "--worktree-root PATH",
      "Temp worktree root for --git-tags; default: tmp/benchmark/worktrees"
    ) { |v| options[:worktree_root] = v }
    parser.on(
      "--[no-]setup-git-refs",
      "For git refs, run bundle install + rake compile; default: true"
    ) { |v| options[:setup_git_refs] = v }
    parser.on("--list", "List jobs/tags and exit") { options[:list] = true }
    parser.on(
      "--quick",
      "Short smoke run: serde scale 0.1, transpile iterations 1, rounds 1"
    ) { options[:quick] = true }
    parser.on(
      "--rounds COUNT",
      Integer,
      "Timed samples per benchmark case"
    ) { |v| options[:rounds] = v }
    parser.on(
      "--warmup COUNT",
      Integer,
      "Warmup iterations per benchmark case"
    ) { |v| options[:warmup] = v }
    parser.on(
      "--only REGEX",
      "Pass case-name regex through to selected suites"
    ) { |v| options[:only] = v }
    parser.on(
      "--serde-scale SCALE",
      Float,
      "Scale serde iteration counts"
    ) { |v| options[:serde_scale] = v }
    parser.on(
      "--transpile-iterations COUNT",
      Integer,
      "Iterations per transpile timed sample"
    ) { |v| options[:transpile_iterations] = v }
    parser.on(
      "--transpile-source NAME",
      "Transpile resource name from benchmark/transpile/resources.json"
    ) { |v| options[:transpile_source] = v }
    parser.on(
      "--transpile-cache-dir PATH",
      "Shared download cache for transpile resources"
    ) { |v| options[:transpile_cache_dir] = v }
    parser.on("--json", "Print machine-readable JSON") { options[:json] = true }
    parser.on("--output PATH", "Write JSON results to PATH") do |v|
      options[:output] = v
    end
  end
  .parse!

if options[:quick]
  options[:serde_scale] ||= 0.1
  options[:transpile_iterations] ||= 1
  options[:rounds] ||= 1
  options[:warmup] ||= 1
end

unknown_tags = options[:tags] - ALL_TAGS
unless unknown_tags.empty?
  abort "Unknown benchmark tag(s): #{unknown_tags.join(", ")}. Known tags: #{ALL_TAGS.join(", ")}"
end

selected_jobs =
  if options[:tags] == ["all"] || options[:tags].include?("all")
    JOBS
  else
    JOBS.select { |job| (options[:tags] - job.tags).empty? }
  end
if selected_jobs.empty?
  abort "No benchmark jobs matched tags: #{options[:tags].join(", ")}"
end

if options[:list]
  puts "Tags: #{ALL_TAGS.join(", ")}"
  puts
  puts "Git refs: use --git-tags v0.21.0,v0.21.3,working"
  puts
  puts "Jobs:"
  JOBS.each do |job|
    puts "  #{job.name}"
    puts "    tags: #{job.tags.join(", ")}"
    puts "    script: #{job.script} #{job.args.join(" ")}".rstrip
  end
  exit
end

def bundle_env(dir)
  { "BUNDLE_GEMFILE" => File.join(dir, "Gemfile") }
end

def run_command!(argv, chdir:, quiet: false, env: {})
  warn "    #{argv.shelljoin}" unless quiet
  stdout, stderr, status =
    Bundler.with_unbundled_env { Open3.capture3(env, *argv, chdir: chdir) }
  warn stderr unless stderr.empty? || quiet
  return stdout if status.success?

  warn stdout unless stdout.empty?
  abort "command failed in #{chdir}: #{argv.shelljoin}"
end

def safe_ref_name(ref)
  ref.gsub(/[^A-Za-z0-9_.-]+/, "_")
end

def sync_benchmark_files!(target_dir)
  benchmark_dir = File.join(target_dir, "benchmark")
  FileUtils.mkdir_p(benchmark_dir)

  %w[run.rb README.md].each do |name|
    src = File.join(ROOT, "benchmark", name)
    FileUtils.cp(src, File.join(benchmark_dir, name)) if File.exist?(src)
  end

  %w[serde transpile].each do |name|
    dst = File.join(benchmark_dir, name)
    FileUtils.rm_rf(dst)
    FileUtils.cp_r(File.join(ROOT, "benchmark", name), dst)
  end
end

def prepare_target(ref, options)
  if WORKING_REFS.include?(ref)
    return(
      Target.new(name: "working", ref: "working", dir: ROOT, working_tree: true)
    )
  end

  FileUtils.mkdir_p(options[:worktree_root])
  dir = File.join(options[:worktree_root], safe_ref_name(ref))
  if File.exist?(dir)
    run_command!(
      %w[git worktree remove --force] + [dir],
      chdir: ROOT,
      quiet: true
    )
  end
  FileUtils.rm_rf(dir)
  run_command!(
    %w[git worktree add --detach] + [dir, ref],
    chdir: ROOT,
    quiet: options[:json]
  )
  sync_benchmark_files!(dir)

  if options[:setup_git_refs]
    warn "==> setting up #{ref}" unless options[:json]
    env = bundle_env(dir)
    run_command!(
      [RbConfig.ruby, "-S", "bundle", "install"],
      chdir: dir,
      quiet: options[:json],
      env: env
    )
    run_command!(
      [RbConfig.ruby, "-S", "bundle", "exec", "rake", "compile"],
      chdir: dir,
      quiet: options[:json],
      env: env
    )
  end

  Target.new(name: ref, ref: ref, dir: dir, working_tree: false)
end

def command_for(job, options)
  args = [RbConfig.ruby, job.script, "--json", *job.args]
  args += ["--rounds", options[:rounds].to_s] if options[:rounds]
  args += ["--warmup", options[:warmup].to_s] if options[:warmup]
  args += ["--only", options[:only]] if options[:only]

  case job.name
  when %r{\Aserde/}
    args += ["--scale", options[:serde_scale].to_s] if options[:serde_scale]
  when %r{\Atranspile/}
    args += ["--iterations", options[:transpile_iterations].to_s] if options[
      :transpile_iterations
    ]
    args += ["--source", options[:transpile_source]] if options[
      :transpile_source
    ]
    args += ["--cache-dir", options[:transpile_cache_dir]] if options[
      :transpile_cache_dir
    ]
  end

  args
end

def run_job(job, target, options)
  command = command_for(job, options)
  unless options[:json]
    warn "==> #{target.name} #{job.name}: #{command.shelljoin}"
  end
  stdout, stderr, status =
    Bundler.with_unbundled_env do
      Open3.capture3(bundle_env(target.dir), *command, chdir: target.dir)
    end
  warn stderr unless stderr.empty?
  unless status.success?
    warn stdout unless stdout.empty?
    abort "#{target.name} #{job.name} failed with exit status #{status.exitstatus}"
  end

  parsed = JSON.parse(stdout)
  {
    name: job.name,
    tags: job.tags,
    command: command,
    metadata: parsed.fetch("metadata"),
    benchmarks: parsed.fetch("benchmarks")
  }
rescue JSON::ParserError => e
  warn stdout
  abort "#{target.name} #{job.name} did not produce valid JSON: #{e.message}"
end

started_at = Process.clock_gettime(Process::CLOCK_MONOTONIC)
refs = options[:git_refs] || ["working"]
targets = refs.map { |ref| prepare_target(ref, options) }
target_results =
  targets.map do |target|
    {
      name: target.name,
      ref: target.ref,
      dir: target.dir,
      working_tree: target.working_tree,
      jobs: selected_jobs.map { |job| run_job(job, target, options) }
    }
  end
elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started_at

payload = {
  metadata: {
    timestamp: Time.now.utc.iso8601,
    ruby_version: RUBY_DESCRIPTION,
    platform: RbConfig::CONFIG["platform"],
    selected_tags: options[:tags],
    git_refs: refs,
    elapsed_ms: elapsed * 1000.0
  },
  targets: target_results,
  jobs: target_results.length == 1 ? target_results.first.fetch(:jobs) : nil
}.compact

if options[:output]
  File.write(options[:output], JSON.pretty_generate(payload) << "\n")
end

if options[:json]
  puts JSON.pretty_generate(payload)
  exit
end

def format_ms(value)
  if value >= 100
    "%.1f" % value
  elsif value >= 1
    "%.3f" % value
  else
    "%.5f" % value
  end
end

def format_pct(value)
  "%+.1f%%" % value
end

def ascii_table(headers, rows, alignments = [])
  widths =
    headers.map.with_index do |header, index|
      ([header.to_s.length] + rows.map { |row| row[index].to_s.length }).max
    end
  separator = "+" + widths.map { |width| "-" * (width + 2) }.join("+") + "+"

  puts separator
  puts "| " +
         headers
           .each_with_index
           .map { |header, index| header.to_s.ljust(widths[index]) }
           .join(" | ") + " |"
  puts separator
  rows.each do |row|
    cells =
      row.each_with_index.map do |cell, index|
        alignment = alignments[index] || :left
        if alignment == :right
          cell.to_s.rjust(widths[index])
        else
          cell.to_s.ljust(widths[index])
        end
      end
    puts "| " + cells.join(" | ") + " |"
  end
  puts separator
end

def print_single_target_job(job)
  metadata = job.fetch(:metadata)
  puts "== #{job.fetch(:name)} =="
  puts "mini_racer: #{metadata["mini_racer_version"]}"
  puts "single:     #{metadata.fetch("single_threaded", false)}"
  if metadata["source_resource"]
    puts "source:     #{metadata["source_resource"]} (#{metadata["source_bytes"]} bytes)"
  end
  puts

  rows =
    job
      .fetch(:benchmarks)
      .map do |bench|
        [
          bench.fetch("name"),
          format_ms(bench.fetch("ms_per_iter")),
          format_ms(bench.fetch("total_ms")),
          bench.fetch("iterations"),
          bench.fetch("rounds")
        ]
      end
  ascii_table(
    ["benchmark", "ms/iter", "total ms", "iters", "rounds"],
    rows,
    %i[left right right right right]
  )
  puts
end

def print_comparison_job(job_name, target_results)
  targets = target_results.map { |target| target.fetch(:name) }
  jobs_by_target =
    target_results.to_h do |target|
      [
        target.fetch(:name),
        target.fetch(:jobs).find { |job| job.fetch(:name) == job_name }
      ]
    end
  first_job = jobs_by_target.fetch(targets.first)
  first_metadata = first_job.fetch(:metadata)
  benchmark_names =
    first_job.fetch(:benchmarks).map { |bench| bench.fetch("name") }

  puts "== #{job_name} =="
  puts "single: #{first_metadata.fetch("single_threaded", false)}"
  if first_metadata["source_resource"]
    puts "source: #{first_metadata["source_resource"]} (#{first_metadata["source_bytes"]} bytes)"
  end
  puts "lower is better; target cells are ms/iter with percent slower than the fastest target for that row"
  puts

  rows =
    benchmark_names.map do |benchmark_name|
      values =
        targets.to_h do |target|
          bench =
            jobs_by_target
              .fetch(target)
              .fetch(:benchmarks)
              .find { |row| row.fetch("name") == benchmark_name }
          [target, bench.fetch("ms_per_iter")]
        end
      best_target, best_value = values.min_by { |_target, value| value }
      slowest_value = values.values.max

      target_cells =
        targets.map do |target|
          value = values.fetch(target)
          delta = ((value / best_value) - 1.0) * 100.0
          suffix = target == best_target ? "best" : format_pct(delta)
          "#{format_ms(value)} (#{suffix})"
        end

      [
        benchmark_name,
        *target_cells,
        best_target,
        format_pct(((slowest_value / best_value) - 1.0) * 100.0)
      ]
    end

  ascii_table(["benchmark", *targets, "fastest", "spread"], rows)
  puts
end

puts
puts "MiniRacer benchmark results"
puts "benchmark tags: #{options[:tags].join(", ")}"
puts "git refs:       #{refs.join(", ")}"
puts "elapsed:        %.3fs" % elapsed
puts

if target_results.length == 1
  target = target_results.first
  puts "Target: #{target.fetch(:name)}"
  puts "dir:    #{target.fetch(:dir)}"
  puts
  target.fetch(:jobs).each { |job| print_single_target_job(job) }
else
  puts "Targets:"
  target_results.each do |target|
    versions =
      target
        .fetch(:jobs)
        .map { |job| job.fetch(:metadata)["mini_racer_version"] }
        .uniq
    puts "  #{target.fetch(:name)}: mini_racer #{versions.join("/")}  dir=#{target.fetch(:dir)}"
  end
  puts
  selected_jobs.each { |job| print_comparison_job(job.name, target_results) }
end
