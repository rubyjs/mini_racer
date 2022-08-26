source 'https://rubygems.org'

# Specify your gem's dependencies in mini_racer.gemspec
gemspec

if File.exist?(ENV['LIBV8_NODE_PATH'].to_s)
  gem 'libv8-node',
      path: ENV['LIBV8_NODE_PATH']
elsif (ENV['LIBV8_NODE_GIT'] || ENV['LIBV8_NODE_GIT_BRANCH'])
  gem 'libv8-node',
      git: ([nil, 'true', 'y', 'yes', '1'].include?(ENV['LIBV8_NODE_GIT']) ? 'https://github.com/rubyjs/libv8-node' : ENV['LIBV8_NODE_GIT']),
      branch: (ENV['LIBV8_NODE_GIT_BRANCH'] || 'master')
end
