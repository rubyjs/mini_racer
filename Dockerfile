ARG RUBY_VERSION=3.2
FROM ruby:${RUBY_VERSION}

RUN test ! -f /etc/alpine-release || apk add --no-cache build-base git

COPY Gemfile mini_racer.gemspec /code/
COPY lib/mini_racer/version.rb /code/lib/mini_racer/version.rb
WORKDIR /code
RUN bundle install

COPY Rakefile /code/
COPY ext /code/ext/
RUN bundle exec rake compile

COPY . /code/
CMD bundle exec irb -rmini_racer
