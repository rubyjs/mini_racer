#!/bin/bash

rm -f Gemfile.lock

set -x

if [ $# -eq 1 ]; then
    eval "$(rbenv init -)"
    rbenv shell $1
fi;

set -e

ruby --version
gem --version

rbenv exec bundle install --no-deployment --path vendor/bundle

rbenv exec ruby -v
rbenv exec bundle exec rake compile test
