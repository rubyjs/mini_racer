#!/bin/bash

rm -rf Gemfile.lock pkg/

set -x

if [ $# -eq 1 ]; then
    eval "$(rbenv init -)"
    rbenv shell $1
fi;

set -e

ruby --version
gem --version

rbenv exec bundle install --no-deployment --path vendor/bundle

rbenv exec bundle exec rake build

git config user.email "engineering@sqreen.io"
git config user.name "Jenkins"

last_gem=pkg/$(ls -t pkg | head -n 1)

version=$(echo $last_gem | sed 's,pkg/sq_mini_racer-\(.*\).gem,\1,')

DO_TAG=${SQREEN_TAG_GEM}

if [ "${DO_TAG}" == "1" ]; then
    git tag -a gem-$version -m "Published $(date)"
fi;

if [ -r "`pwd`/.gem/credentials" ]; then
  HOME=`pwd` gem push ${last_gem} -k sqreen_rubygems_api_key
else
  echo "Not publishing to rubygems.org (no credentials is `pwd`/.gem/credentials )"
fi

if [ "${DO_TAG}" == "1" ]; then
  if [ -n "${PUBLISH_VERSION}" ]; then
    echo "SQREEN_VERSION=$version" > $PUBLISH_VERSION
  else
    git push origin gem-$version
  fi;
fi;
