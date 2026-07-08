# frozen_string_literal: true

require_relative "red_faucet/version"
require_relative "red_faucet/event"
require_relative "red_faucet/pending_span"
require_relative "red_faucet/otel_converter"
require_relative "red_faucet/config"
require_relative "red_faucet/method_registry"
require_relative "red_faucet/worker"
require_relative "red_faucet/session"

module RedFaucet
  class Error < StandardError; end
  class AlreadyOpenError < Error; end
  class NotOpenError < Error; end
  class UntraceableMethodError < Error; end

  module_function

  # RedFaucet.new -> Session, so `tape = RedFaucet.new; tape.open; ...; tape.stop`
  # reads like constructing a recorder, while RedFaucet itself stays a module.
  def new(**opts)
    Session.new(**opts)
  end

  def default_registry
    @default_registry ||= MethodRegistry.new
  end

  def config
    @config ||= Config.new
  end

  def trace_method(method_obj)
    default_registry.register(method_obj)
  end

  def untrace_method(method_obj)
    default_registry.unregister(method_obj)
  end

  def trace_all_instance_methods(klass)
    default_registry.register_all_instance_methods(klass)
  end

  def open(&block)
    tape = new
    tape.open
    return tape unless block

    begin
      block.call
      tape.stop
    rescue Exception # rubocop:disable Lint/RescueException
      # Make sure TracePoints are disabled and the worker is drained even
      # when the block raises. The output path is unrecoverable here, so we
      # re-raise the original error instead of returning it.
      begin
        tape.stop
      rescue StandardError
        nil
      end
      raise
    end
  end
end
