# frozen_string_literal: true

require "mini_racer"
require "libv8-node"
require "rbconfig"

puts "RbConfig::CONFIG['LIBS']: #{RbConfig::CONFIG["LIBS"]}"
puts "RUBY_VERSION: #{RUBY_VERSION}"
puts "RUBY_PLATFORM: #{RUBY_PLATFORM}"
puts "MiniRacer::VERSION: #{MiniRacer::VERSION}"
puts "MiniRacer::LIBV8_NODE_VERSION: #{MiniRacer::LIBV8_NODE_VERSION}"
puts "Libv8::Node::VERSION: #{Libv8::Node::VERSION}"
puts "Libv8::Node::NODE_VERSION: #{Libv8::Node::NODE_VERSION}"
puts "Libv8::Node::LIBV8_VERSION: #{Libv8::Node::LIBV8_VERSION}"
puts "=" * 80

require "minitest/autorun"

class MiniRacerFunctionTest < Minitest::Test
  def test_minimal
    assert_equal MiniRacer::Context.new.eval("41 + 1"), 42
  end
end
