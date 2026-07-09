# frozen_string_literal: true

require_relative "lib/orange_tap/version"

Gem::Specification.new do |spec|
  spec.name = "orange_tap"
  spec.version = OrangeTap::VERSION
  spec.authors = ["Uchio Kondo"]
  spec.email = ["uchio.kondo@smarthr.co.jp"]

  spec.summary = "Trace Ruby method calls with TracePoint and export the result as " \
                 "OpenTelemetry spans — no daemon, no shared memory, single process only."
  spec.description = "OrangeTap hooks Ruby method calls (call/return) with TracePoint, " \
                      "assembles them into OpenTelemetry-style spans on a background thread " \
                      "inside the same process, and writes the result as an OTLP/JSON file."
  spec.homepage = "https://github.com/udzura/orange_tap"
  spec.license = "MIT"
  spec.required_ruby_version = ">= 3.2.0"
  spec.metadata["homepage_uri"] = spec.homepage
  spec.metadata["source_code_uri"] = spec.homepage

  # Specify which files should be added to the gem when it is released.
  # The `git ls-files -z` loads the files in the RubyGem that have been added into git.
  gemspec = File.basename(__FILE__)
  spec.files = IO.popen(%w[git ls-files -z], chdir: __dir__, err: IO::NULL) do |ls|
    ls.readlines("\x0", chomp: true).reject do |f|
      (f == gemspec) ||
        f.start_with?(*%w[bin/ Gemfile .gitignore test/ .github/])
    end
  end
  spec.bindir = "exe"
  spec.executables = spec.files.grep(%r{\Aexe/}) { |f| File.basename(f) }
  spec.require_paths = ["lib"]

  # Uncomment to register a new dependency of your gem
  # spec.add_dependency "example-gem", "~> 1.0"

  # For more information and examples about making a new gem, check out our
  # guide at: https://bundler.io/guides/creating_gem.html
end
