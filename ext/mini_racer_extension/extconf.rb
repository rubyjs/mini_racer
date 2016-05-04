require 'mkmf'

have_library('pthread')
have_library('objc') if RUBY_PLATFORM =~ /darwin/
$CPPFLAGS += " -Wall" unless $CPPFLAGS.split.include? "-Wall"
$CPPFLAGS += " -g" unless $CPPFLAGS.split.include? "-g"
$CPPFLAGS += " -rdynamic" unless $CPPFLAGS.split.include? "-rdynamic"
$CPPFLAGS += " -fPIC" unless $CPPFLAGS.split.include? "-rdynamic" or RUBY_PLATFORM =~ /darwin/
$CPPFLAGS += " -std=c++0x"
$CPPFLAGS += " -fpermissive"

CONFIG['LDSHARED'] = '$(CXX) -shared' unless RUBY_PLATFORM =~ /darwin/
if CONFIG['warnflags']
  CONFIG['warnflags'].gsub!('-Wdeclaration-after-statement', '')
  CONFIG['warnflags'].gsub!('-Wimplicit-function-declaration', '')
end
if enable_config('debug')
  $CFLAGS += " -O0 -ggdb3"
end

LIBV8_COMPATIBILITY = '~> 5.0.71.35.0'

begin
  require 'rubygems'
  gem 'libv8', LIBV8_COMPATIBILITY
rescue Gem::LoadError
  warn "Warning! Unable to load libv8 #{LIBV8_COMPATIBILITY}."
rescue LoadError
  warn "Warning! Could not load rubygems. Please make sure you have libv8 #{LIBV8_COMPATIBILITY} installed."
ensure
  require 'libv8'
end

Libv8.configure_makefile

# Temp Hack
find_header('v8.h', '/home/sam/.rbenv/versions/2.3.1/lib/ruby/gems/2.3.0/gems/libv8-5.0.71.35.0/vendor/v8/include')

find_header('libplatform/libplatform.h', '/home/sam/.rbenv/versions/2.3.1/lib/ruby/gems/2.3.0/gems/libv8-5.0.71.35.0/vendor/v8/include')

create_makefile 'mini_racer_extension'
