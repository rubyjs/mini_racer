# Serialization / deserialization benchmarks

This directory contains focused benchmarks for the MiniRacer boundary between
Ruby and V8. They are intended to catch regressions in the hot paths that are
not covered by the larger Babel/Uglifier benchmarks.

Run from the repository root after compiling the extension:

```sh
bundle exec rake compile
bundle exec ruby benchmark/serde/bench.rb
```

Useful modes:

```sh
# Run only the numeric/string deserialization cases
bundle exec ruby benchmark/serde/bench.rb --only 'js_to_ruby/return_10k'

# Quick smoke run with fewer iterations
BENCH_SCALE=0.1 bundle exec ruby benchmark/serde/bench.rb

# Use multiple timed samples per case and report the median
bundle exec ruby benchmark/serde/bench.rb --rounds 3

# Run on MiniRacer's single-threaded platform
BENCH_SINGLE_THREADED=1 bundle exec ruby benchmark/serde/bench.rb
# or:
bundle exec ruby benchmark/serde/bench.rb --single-threaded

# Save a baseline, then compare another build against it
bundle exec ruby benchmark/serde/bench.rb --output /tmp/mini_racer-serde-baseline.json
bundle exec ruby benchmark/serde/bench.rb --compare /tmp/mini_racer-serde-baseline.json

# Machine-readable output for CI or local scripts
bundle exec ruby benchmark/serde/bench.rb --json
```

Benchmark groups:

- `js_to_ruby/*`: V8 `ValueSerializer` output decoded into Ruby objects.
- `ruby_to_js/*`: Ruby values serialized to V8 and returned back through
  `Context#call`.
- `callback/*`: JS calling attached Ruby procs, including callback argument and
  return value serialization.

String cases distinguish ASCII, Latin-1, valid UTF-8 BMP characters, and emoji.
V8 serializes JS strings as one-byte Latin-1 or UTF-16LE; valid UTF-16LE strings
are exported to UTF-8 Ruby strings during deserialization.

The first `js_to_ruby/return_10k_*` and `ruby_to_js/call_*` cases mirror the
synthetic regression repro used to catch numeric deserialization slowdowns.
