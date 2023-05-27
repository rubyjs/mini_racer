# coding: utf-8
lib = File.expand_path('../lib', __FILE__)
$LOAD_PATH.unshift(lib) unless $LOAD_PATH.include?(lib)
require 'mini_racer/version'

Gem::Specification.new do |spec|
  spec.name          = "mini_racer"
  spec.version       = MiniRacer::VERSION
  spec.authors       = ["Sam Saffron"]
  spec.email         = ["sam.saffron@gmail.com"]

  spec.summary       = %q{Minimal embedded v8 for Ruby}
  spec.description   = %q{Minimal embedded v8 engine for Ruby}
  spec.homepage      = "https://github.com/discourse/mini_racer"
  spec.license       = "MIT"

  spec.metadata      = {
    "bug_tracker_uri"   => "https://github.com/discourse/mini_racer/issues",
    "changelog_uri"     => "https://github.com/discourse/mini_racer/blob/v#{spec.version}/CHANGELOG",
    "documentation_uri" => "https://www.rubydoc.info/gems/mini_racer/#{spec.version}",
    "source_code_uri"   => "https://github.com/discourse/mini_racer/tree/v#{spec.version}",
  }

  spec.files         = `git ls-files -z`.split("\x0").reject { |f| f.match(%r{^(\.github|bin|benchmark|test|spec|features|examples)/}) }
  spec.require_paths = ["lib"]

  spec.add_development_dependency "bundler"
  spec.add_development_dependency "rake", ">= 12.3.3"
  spec.add_development_dependency "minitest", "~> 5.0"
  spec.add_development_dependency "rake-compiler"
  spec.add_development_dependency "m"

  spec.add_dependency 'libv8-node', MiniRacer::LIBV8_NODE_VERSION
  spec.require_paths = ["lib", "ext"]

  spec.extensions = ["ext/mini_racer_loader/extconf.rb", "ext/mini_racer_extension/extconf.rb"]

  spec.required_ruby_version = '>= 3.0'
end
