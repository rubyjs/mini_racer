# coding: utf-8
lib = File.expand_path('../lib', __FILE__)
$LOAD_PATH.unshift(lib) unless $LOAD_PATH.include?(lib)
require 'sqreen/mini_racer/version'

Gem::Specification.new do |spec|
  spec.name          = "sq_mini_racer"
  spec.version       = Sqreen::MiniRacer::VERSION
  spec.authors       = ["Sam Saffron"]
  spec.email         = ["sam.saffron@gmail.com"]

  spec.summary       = %q{Minimal embedded v8 for Ruby}
  spec.description   = %q{Minimal embedded v8 engine for Ruby (Sqreen's fork)}
  spec.homepage      = "https://github.com/sqreen/mini_racer"
  spec.license       = "MIT"


  REJECTS            = %r{\A((benchmark|test|spec|features|examples)/|bench\.rb|.+\.sh|Jenkinsfile)}
  spec.files         = `git ls-files -z`.split("\x0").reject { |f| f.match(REJECTS) }
  spec.bindir        = "exe"
  spec.executables   = spec.files.grep(%r{^exe/}) { |f| File.basename(f) }
  spec.require_paths = ["lib"]

  spec.add_development_dependency "bundler", "~> 1.12"
  spec.add_development_dependency "rake", "~> 10.0"
  spec.add_development_dependency "minitest", "~> 5.0"
  spec.add_development_dependency "rake-compiler"

  spec.require_paths = ["lib", "ext"]

  spec.extensions = ["ext/mini_racer_extension/extconf.rb", "ext/prv_ext_loader/extconf.rb"]

  spec.required_ruby_version = '>= 1.9.3'
end
