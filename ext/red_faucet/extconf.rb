# frozen_string_literal: true

require "mkmf"

unless RUBY_PLATFORM =~ /darwin/
  abort "red_faucet only supports macOS: it depends on XPC (xpc/xpc.h) and " \
        "Mach virtual memory (mach/mach_vm.h), both of which are Darwin-only APIs."
end

# XPC, Mach VM and libdispatch (GCD) are all re-exported by libSystem on
# macOS, so no extra `-framework`/`-l` flag is required to link them (see
# README.md "macOS専用である理由"). We only need their headers to be
# discoverable, which have_header also verifies at configure time.
unless have_header("ruby/debug.h")
  abort "ruby/debug.h (rb_add_event_hook) is required but was not found."
end

unless have_header("xpc/xpc.h")
  abort "xpc/xpc.h was not found. Install Xcode Command Line Tools " \
        "(xcode-select --install) so the macOS SDK headers are available."
end

unless have_header("mach/mach_vm.h")
  abort "mach/mach_vm.h was not found. Install Xcode Command Line Tools " \
        "(xcode-select --install) so the macOS SDK headers are available."
end

have_header("dispatch/dispatch.h")

# _Atomic, __thread and flexible array members (trace_event_t events[]) are
# C11 features; clang on macOS defaults to gnu11-ish behavior already but we
# pin it explicitly so the build doesn't depend on the ambient default.
append_cflags(["-std=c11", "-Wall", "-Wextra"])

create_makefile("red_faucet_ext")
