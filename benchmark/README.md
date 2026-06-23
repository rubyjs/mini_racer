# Benchmarks

Run all maintained MiniRacer benchmarks through one entry point:

```sh
bundle exec ruby benchmark/run.rb --tags all
```

The runner executes each selected benchmark suite in a separate Ruby process so
MiniRacer platform flags, especially `--single-threaded`, are isolated per job.

## Tags

List available tags and jobs:

```sh
bundle exec ruby benchmark/run.rb --list
```

Current tags:

- `all`: every maintained benchmark job.
- `serde` / `boundary`: Ruby ↔ V8 serialization/deserialization benchmarks.
- `transpile` / `realworld`: Babel transpilation of pinned real-world JS.
- `default`: normal MiniRacer platform.
- `single-threaded` / `single`: MiniRacer single-threaded platform.

Examples:

```sh
# Everything on the current working tree
bundle exec ruby benchmark/run.rb --tags all

# Compare specific git tags plus the current working tree.
# Multiple refs are printed side-by-side; each cell shows ms/iter and how much
# slower it is than the fastest ref for that benchmark row.
bundle exec ruby benchmark/run.rb --git-tags v0.21.0,v0.21.3,working --tags all

# Compare only the real-world single-threaded transpile benchmark across tags
bundle exec ruby benchmark/run.rb \
  --git-tags v0.21.0,v0.21.3,working \
  --tags transpile,single-threaded \
  --transpile-iterations 1 \
  --rounds 3

# Only single-threaded jobs
bundle exec ruby benchmark/run.rb --tags single-threaded

# Only serde in single-threaded mode
bundle exec ruby benchmark/run.rb --tags serde,single-threaded

# Only real-world transpile on the default platform
bundle exec ruby benchmark/run.rb --tags transpile,default

# Quick smoke run
bundle exec ruby benchmark/run.rb --tags all --quick

# Save machine-readable results
bundle exec ruby benchmark/run.rb --tags all --json --output /tmp/mini_racer-benchmarks.json
```

The runner creates temporary detached worktrees for non-`working` refs, copies the
current benchmark scripts into them, and by default runs `bundle install` plus
`bundle exec rake compile` before benchmarking each ref. Use
`--no-setup-git-refs` only when those worktrees are already prepared.

Useful shared options:

```sh
--git-tags REFS             Comma-separated git tags/refs to run; use `working` for this checkout
--worktree-root PATH        Temporary worktree root for --git-tags
--no-setup-git-refs         Skip bundle install + rake compile for git refs
--rounds N                  Timed samples per case
--warmup N                  Warmup iterations per case
--only REGEX                Run matching case names inside selected suites
--serde-scale SCALE         Scale serde iteration counts
--transpile-iterations N    Iterations per transpile timed sample
--transpile-source NAME     Source from benchmark/transpile/resources.json
--transpile-cache-dir PATH  Shared download cache for transpile resources
```

The older `benchmark/babel` and `benchmark/exec_js_uglify` directories are left
as historical/legacy comparisons. The unified runner covers the maintained serde
and real-world transpile suites.
