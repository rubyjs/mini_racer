# Real-world transpilation benchmark

This benchmark runs Babel standalone inside MiniRacer and transpiles a pinned
real-world modern JavaScript module toward an ES2015-ish target.

No large benchmark inputs are committed. `resources.json` stores only resource
locations, byte sizes, and SHA-256 checksums. The benchmark downloads resources
into `tmp/benchmark/transpile` on first run and verifies each checksum before use.

Default input:

- `pdfjs_worker_4_10_38`
- URL: `https://unpkg.com/pdfjs-dist@4.10.38/build/pdf.worker.mjs`
- SHA-256: `7c237f83fa56bce645d8af51d183c9c56ba7b2d2928ff42754dc7020bea36323`
- Size: 2,209,730 bytes (~2.1 MiB)

Compiler:

- `@babel/standalone@7.26.10`
- URL: `https://unpkg.com/@babel/standalone@7.26.10/babel.min.js`
- SHA-256: `7acf405141ca9e611b6d40aeef52071432d5f387cf906fc3556fb9f0b1a28601`

Run from the repository root:

```sh
bundle exec ruby benchmark/transpile/bench.rb
```

Useful modes:

```sh
# Show pinned downloadable resources
bundle exec ruby benchmark/transpile/bench.rb --list-resources

# Download and checksum resources without running the benchmark
bundle exec ruby benchmark/transpile/bench.rb --fetch-only

# Quick one-iteration smoke run; use rounds=1 to opt out of the stable-sample
# floor used for multi-round transpile runs.
BENCH_ITERATIONS=1 BENCH_ROUNDS=1 BENCH_WARMUP=1 bundle exec ruby benchmark/transpile/bench.rb

# Run on MiniRacer's single-threaded platform
BENCH_SINGLE_THREADED=1 bundle exec ruby benchmark/transpile/bench.rb
# or:
bundle exec ruby benchmark/transpile/bench.rb --single-threaded

# Use the smaller pdf.js API module instead of the worker
bundle exec ruby benchmark/transpile/bench.rb --source pdfjs_api_4_10_38

# Run only the case that returns the generated code to Ruby
bundle exec ruby benchmark/transpile/bench.rb --only 'transpile_call_source_to_es6_code'

# Machine-readable output
bundle exec ruby benchmark/transpile/bench.rb --json --output /tmp/mini_racer-transpile.json
```

Before timing each benchmark case, the suite performs untimed warmup iterations.
Multi-round runs collect at least seven timed samples because Babel/pdf.js
transpilation has regular V8 GC outliers; with only two or three samples, the
median can report an arbitrary GC phase instead of a stable center. Use
`--rounds 1` only for smoke runs.

Benchmark cases:

- `babel/load_standalone`: creates a context and loads Babel standalone.
- `babel/transpile_cached_source_to_es6_length`: source is already stored in JS;
  returns only generated code length.
- `babel/transpile_call_source_to_es6_length`: passes the large source string
  from Ruby each iteration; returns only generated code length.
- `babel/transpile_call_source_to_es6_code`: passes source from Ruby and returns
  the generated code string to Ruby, exercising both large input and output
  boundary costs.
