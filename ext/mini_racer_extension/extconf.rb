require 'mkmf'

$srcs = ["mini_racer_extension.c", "mini_racer_v8.cc"]

if RUBY_ENGINE == "truffleruby"
  File.write("Makefile", dummy_makefile($srcdir).join(""))
  return
end

require_relative '../../lib/mini_racer/version'
gem 'libv8-node', MiniRacer::LIBV8_NODE_VERSION
require 'libv8-node'

# Fix for missing aarch64-linux-musl gem in libv8-node
# On some platforms (like ARM64 Alpine), the musl-specific gem might be missing.
# We fallback to the glibc gem's directory if the musl one isn't found.
if RUBY_PLATFORM.include?('musl')
  module Libv8::Node::Paths
    def self.platform
      @platform ||= begin
        # Replicate libv8-node's platform detection logic without side effects
        detected_platform = Gem::Platform.local.dup
        if detected_platform.version.nil?
          detected_platform.instance_eval { @version = 'musl' }
        end

        platform_name = detected_platform.to_s.gsub(/-darwin-?\d+/, '-darwin')

        # If the musl-specific directory is missing, try falling back to glibc
        unless File.exist?(File.join(vendored_source_path, platform_name))
          fallback = platform_name.gsub('-musl', '')
          if File.exist?(File.join(vendored_source_path, fallback))
            platform_name = fallback
          end
        end

        platform_name
      end
    end
  end
end

IS_DARWIN = RUBY_PLATFORM =~ /darwin/

have_library('pthread')
have_library('objc') if IS_DARWIN
$CXXFLAGS += " -Wall" unless $CXXFLAGS.split.include? "-Wall"
$CXXFLAGS += " -g" unless $CXXFLAGS.split.include? "-g"
$CXXFLAGS += " -rdynamic" unless $CXXFLAGS.split.include? "-rdynamic"
$CXXFLAGS += " -fPIC" unless $CXXFLAGS.split.include? "-rdynamic" or IS_DARWIN
$CXXFLAGS += " -std=c++20"
$CXXFLAGS += " -fpermissive"
$CXXFLAGS += " -fno-rtti"
$CXXFLAGS += " -fno-exceptions"
$CXXFLAGS += " -fno-strict-aliasing"
#$CXXFLAGS += " -DV8_COMPRESS_POINTERS"
$CXXFLAGS += " -fvisibility=hidden "

# __declspec gets used by clang via ruby 3.x headers...
$CXXFLAGS += " -fms-extensions"

$CXXFLAGS += " -Wno-reserved-user-defined-literal" if IS_DARWIN

if IS_DARWIN
  $LDFLAGS.insert(0, " -stdlib=libc++ ")
else
  $LDFLAGS.insert(0, " -lstdc++ ")
end

# check for missing symbols at link time
# $LDFLAGS += " -Wl,--no-undefined " unless IS_DARWIN
# $LDFLAGS += " -Wl,-undefined,error " if IS_DARWIN

if ENV['CXX']
  puts "SETTING CXX"
  CONFIG['CXX'] = ENV['CXX']
end

CONFIG['LDSHARED'] = '$(CXX) -shared' unless IS_DARWIN
if CONFIG['warnflags']
  CONFIG['warnflags'].gsub!('-Wdeclaration-after-statement', '')
  CONFIG['warnflags'].gsub!('-Wimplicit-function-declaration', '')
end

if enable_config('debug') || enable_config('asan')
  CONFIG['debugflags'] << ' -ggdb3 -O0'
end

Libv8::Node.configure_makefile

# --exclude-libs is only for i386 PE and ELF targeted ports
append_ldflags("-Wl,--exclude-libs=ALL ")

if enable_config('asan')
  $CXXFLAGS.insert(0, " -fsanitize=address ")
  $LDFLAGS.insert(0, " -fsanitize=address ")
end

# there doesn't seem to be a CPP macro for this in Ruby 2.6:
if RUBY_ENGINE == 'ruby'
  $CPPFLAGS += ' -DENGINE_IS_CRUBY '
end

create_makefile 'mini_racer_extension'
