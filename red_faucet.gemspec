# frozen_string_literal: true

require_relative "lib/red_faucet/version"

Gem::Specification.new do |spec|
  spec.name = "red_faucet"
  spec.version = RedFaucet::VERSION
  spec.authors = ["Uchio Kondo"]
  spec.email = ["uchio.kondo@smarthr.co.jp"]

  spec.summary = "Low-overhead method call/return tracing for macOS, streamed to a central daemon via shared memory + XPC."
  spec.description = "RedFaucet hooks Ruby method call/return events with rb_add_event_hook and writes them " \
                      "to a lock-free shared-memory ring buffer, handing the memory off to a central daemon " \
                      "process over XPC (Mach send rights). macOS only."
  spec.homepage = "https://github.com/udzura/red_faucet"
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
  spec.extensions = ["ext/red_faucet/extconf.rb"]

  # Uncomment to register a new dependency of your gem
  # spec.add_dependency "example-gem", "~> 1.0"

  # For more information and examples about making a new gem, check out our
  # guide at: https://bundler.io/guides/creating_gem.html
end
