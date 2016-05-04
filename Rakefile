require "bundler/gem_tasks"
require "rake/testtask"
require "rake/extensiontask"

Rake::TestTask.new(:test) do |t|
  t.libs << "test"
  t.libs << "lib"
  t.test_files = FileList['test/**/*_test.rb']
end

task :default => :test

gem = Gem::Specification.load( File.dirname(__FILE__) + '/mini_racer.gemspec' )
Rake::ExtensionTask.new( 'mini_racer_extension', gem )

