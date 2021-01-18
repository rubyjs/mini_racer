ARG RUBY_VERSION=2.7
FROM ruby:${RUBY_VERSION}

RUN test ! -f /etc/alpine-release || apk add --no-cache build-base git

# without this `COPY .git`, we get the following error:
#   fatal: not a git repository (or any of the parent directories): .git
# but with it we need the full gem just to compile the extension because
# of gemspec's `git --ls-files`
# COPY .git /code/.git
COPY Gemfile mini_racer.gemspec /code/
COPY lib/mini_racer/version.rb /code/lib/mini_racer/version.rb
WORKDIR /code
RUN bundle install

COPY Rakefile /code/
COPY ext /code/ext/
RUN bundle exec rake compile

COPY . /code/
CMD bundle exec irb -rmini_racer
