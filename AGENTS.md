# AGENTS.md

## Project

`mini_racer` is a Ruby gem with a CRuby C/C++ extension around V8 and a separate
TruffleRuby implementation. Keep changes focused and preserve behavior across
supported Ruby versions and platforms.

## Setup and checks

```sh
bundle install
bundle exec rake compile
bundle exec rake test
bundle exec rake lint
```

Run focused tests while iterating, for example:

```sh
bundle exec ruby -Itest test/mini_racer_test.rb
bundle exec ruby -Itest test/single_threaded_test.rb
```

Format changed Ruby files with `bundle exec stree write <files>`. CI runs
`bundle exec rake lint`, which checks Syntax Tree formatting and RuboCop lint.
After native changes, always recompile before testing.

## Conventions

- Follow nearby Ruby and native code style; avoid unrelated refactors.
- Add or update tests for behavior changes. Use isolated subprocesses for
  platform-global, fork, crash, or process-shutdown behavior.
- Preserve the distinction between default-platform and `:single_threaded` V8
  lifecycle paths, especially around threads, locks, callbacks, and forks.
- Do not edit generated files under `tmp/` or compiled extension artifacts.
- Before finishing, run relevant focused tests, `bundle exec rake lint`,
  `bundle exec rake test`, and `git diff --check`.
