$LOAD_PATH.unshift File.expand_path("../../lib", __FILE__)
require "mini_racer"

def test
  context = MiniRacer::Context.new(timeout: 10)
  context.attach(
    "echo",
    proc do |msg|
      GC.start
      msg
    end
  )

  GC.disable
  100.times { "foo" } # alloc a handful of objects
  GC.enable

  begin
    context.eval("while(true) echo('foo');")
  rescue StandardError
    nil
  end

  # give some time to clean up
  puts "we are done"
end

def test2
  context = MiniRacer::Context.new(timeout: 5)

  context.attach(
    "marsh",
    proc do |a, b, c|
      if a.is_a?(MiniRacer::FailedV8Conversion) ||
           b.is_a?(MiniRacer::FailedV8Conversion) ||
           c.is_a?(MiniRacer::FailedV8Conversion)
        return a, b, c
      end

      a[rand(10_000).to_s] = "a"
      b[rand(10_000).to_s] = "b"
      c[rand(10_000).to_s] = "c"
      [a, b, c]
    end
  )

  begin
    context.eval(
      "var a = [{},{},{}]; while(true) { a = marsh(a[0],a[1],a[2]); }"
    )
  rescue StandardError
    p "BOOM"
  end
end

def test3
  snapshot = MiniRacer::Snapshot.new("Math.sin = 1;")

  begin
    snapshot.warmup!("var a = Math.sin(1);")
  rescue StandardError
    # do nothing
  end

  context = MiniRacer::Context.new(snapshot: snapshot)

  assert_equal 1, context.eval("Math.sin")
end

500_000.times { test2 }

exit
