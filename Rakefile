require "bundler/gem_tasks"
require "rake/testtask"
require "rake/extensiontask"

Rake::TestTask.new(:test) do |t|
  t.libs << "test"
  t.libs << "lib"
  t.test_files = FileList['test/**/*_test.rb']
end

task :default => [:compile, :test]

gem = Gem::Specification.load( File.dirname(__FILE__) + '/mini_racer.gemspec' )
Rake::ExtensionTask.new( 'mini_racer_extension', gem )


# via http://blog.flavorjon.es/2009/06/easily-valgrind-gdb-your-ruby-c.html
namespace :test do
  # partial-loads-ok and undef-value-errors necessary to ignore
  # spurious (and eminently ignorable) warnings from the ruby
  # interpreter
  VALGRIND_BASIC_OPTS = "--num-callers=50 --error-limit=no \
                         --partial-loads-ok=yes --undef-value-errors=no"

  desc "run test suite under valgrind with basic ruby options"
  task :valgrind => :compile do
    cmdline = "valgrind #{VALGRIND_BASIC_OPTS} ruby test/test_leak.rb"
    puts cmdline
    system cmdline
  end

  desc "run test suite under valgrind with leak-check=full"
  task :valgrind_leak_check => :compile do
    cmdline = "valgrind #{VALGRIND_BASIC_OPTS} --leak-check=full ruby test/test_leak.rb"
    puts cmdline
    require 'open3'
    _, stderr = Open3.capture3(cmdline)

    section = ""
    stderr.split("\n").each do |line|

      if line =~ /==.*==\s*$/
        if (section =~ /mini_racer|SUMMARY/)
          puts
          puts section
          puts
        end
        section = ""
      else
        section << line << "\n"
      end
    end
  end
end

desc 'run clang-tidy linter on mini_racer_extension.cc'
task :lint do
  require 'mkmf'
  require 'libv8'

  Libv8.configure_makefile

  conf = RbConfig::CONFIG.merge('hdrdir' => $hdrdir.quote, 'srcdir' => $srcdir.quote,
                                'arch_hdrdir' => $arch_hdrdir.quote,
                                'top_srcdir' => $top_srcdir.quote)
  if $universal and (arch_flag = conf['ARCH_FLAG']) and !arch_flag.empty?
    conf['ARCH_FLAG'] = arch_flag.gsub(/(?:\G|\s)-arch\s+\S+/, '')
  end

  checks = %W(bugprone-*
              cert-*
              cppcoreguidelines-*
              clang-analyzer-*
              performance-*
              portability-*
              readability-*).join(',')

  sh RbConfig::expand("clang-tidy -checks='#{checks}' ext/mini_racer_extension/mini_racer_extension.cc -- #$INCFLAGS #$CPPFLAGS", conf)
end
