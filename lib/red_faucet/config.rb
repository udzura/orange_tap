# frozen_string_literal: true

require "tmpdir"

module RedFaucet
  class Config
    attr_accessor :output_dir, :service_name, :otel_converter

    def initialize
      @output_dir = File.join(Dir.tmpdir, "red_faucet")
      @service_name = "red_faucet"
      @otel_converter = RedFaucet::OtelConverter
    end
  end
end
