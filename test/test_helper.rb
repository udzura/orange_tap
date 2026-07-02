# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "red_faucet"

require "test-unit"

# RedFaucet's C-level state (ring buffer, XPC connection, active session id)
# is process-global, so the whole test-unit run shares a single in-process
# mock daemon (ext/red_faucet/mock_daemon.c) instead of a real central
# daemon. Every test starts from a freshly (re)configured ring buffer so
# leftover events from a previous test can never leak into the next one.
RedFaucet.send(:_enable_test_mock_daemon!)

module RedFaucetTestCase
  DEFAULT_CAPACITY = 1024

  def setup
    RedFaucet.configure(mach_service_name: "__test_mock__", ring_buffer_capacity: DEFAULT_CAPACITY)
  end
end
