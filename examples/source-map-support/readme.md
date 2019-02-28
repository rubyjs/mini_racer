# Source Map Support

This example shows how to map source maps using webpack. Webpack production
builds will compile and minify code aggressively, so source maps are very
important when debugging production issues. This example shows how to give
readable stack traces to make debugging easier.

## Running the example

1. Install the dependencies: `yarn install`
2. Build the js bundle: `yarn build`
3. Run the ruby code which triggers an error: `bundle exec ruby renderer.rb`

After running that, you will see the correct source code locations where the
error occurred. The result will intentionally throw an error which looks like:

```
Traceback (most recent call last):
        10: from renderer.rb:12:in `<main>'
         9: from /home/ianks/Development/adhawk/mini_racer/lib/mini_racer.rb:176:in `eval'
         8: from /home/ianks/Development/adhawk/mini_racer/lib/mini_racer.rb:176:in `synchronize'
         7: from /home/ianks/Development/adhawk/mini_racer/lib/mini_racer.rb:178:in `block in eval'
         6: from /home/ianks/Development/adhawk/mini_racer/lib/mini_racer.rb:264:in `timeout'
         5: from /home/ianks/Development/adhawk/mini_racer/lib/mini_racer.rb:179:in `block (2 levels) in eval'
         4: from /home/ianks/Development/adhawk/mini_racer/lib/mini_racer.rb:179:in `eval_unsafe'
         3: from JavaScript at <anonymous>:1:17
         2: from JavaScript at Module.ErrorCausingComponent (/webpack:/webpackLib/index.jsx:19:3)
         1: from JavaScript at throwSomeError (/webpack:/webpackLib/error-causing-component.jsx:8:3)
JavaScript at /webpack:/webpackLib/error-causing-component.jsx:2:9: Error: ^^ Look! These stack traces map to the actual source code :) (MiniRacer::RuntimeError)
```
