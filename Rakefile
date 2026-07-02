# frozen_string_literal: true

require "bundler/gem_tasks"
require "rake/testtask"
require "rake/clean"

EXT_DIR = File.expand_path("ext/red_faucet", __dir__)
EXT_SO = File.join(EXT_DIR, "red_faucet_ext.#{RbConfig::CONFIG["DLEXT"]}")

CLEAN.include("#{EXT_DIR}/*.o", "#{EXT_DIR}/*.bundle", "#{EXT_DIR}/*.so", "#{EXT_DIR}/Makefile")

file EXT_SO => FileList["#{EXT_DIR}/*.c", "#{EXT_DIR}/*.h", "#{EXT_DIR}/extconf.rb"] do
  cd(EXT_DIR) do
    ruby "extconf.rb"
    sh "make"
  end
end

desc "Build the native extension"
task compile: EXT_SO

Rake::TestTask.new(:test) do |t|
  t.libs << "test"
  t.libs << "lib"
  t.libs << "ext/red_faucet"
  t.test_files = FileList["test/**/*_test.rb"]
end

task test: :compile

task default: :test
