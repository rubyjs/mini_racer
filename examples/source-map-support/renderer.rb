require 'mini_racer'

ctx = MiniRacer::Context.new

# Make sure we pass the filename option so source-map-support can map properly
ctx.eval(File.read('./dist/main.js'), filename: 'main.js')

# Expose a function to retrieve the source map
ctx.attach('readSourceMap', proc { |filename| File.read("./dist/#{filename}.map")} )

# This will actually cause the error, and we will have a pretty backtrace!
ctx.eval('this.webpackLib.renderComponent()')
