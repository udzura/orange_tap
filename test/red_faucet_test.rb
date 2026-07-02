# frozen_string_literal: true

require "test_helper"

class RedFaucetTest < Test::Unit::TestCase
  test "VERSION" do
    assert do
      ::RedFaucet.const_defined?(:VERSION)
    end
  end

  test "RedFaucet is a class with an instance-level open/stop pair" do
    assert_kind_of(Class, RedFaucet)
    assert_respond_to(RedFaucet.new, :open)
    assert_respond_to(RedFaucet.new, :stop)
  end
end
