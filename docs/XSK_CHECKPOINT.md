# XSK lifecycle checkpoint

The setup path now reports ring-rebuild failures during unbind and rollback.
The bind error remains the original failure; successful AF_XDP pool teardown is
not falsely represented as a recoverable binding after the pool is unmapped.
The test tool also records the negotiated copy/zero-copy mode and frame/completion
counts for the dedicated link checks.

`python3 tools/xdp/test_xsk_pool_setup.py` extracts production setup and rebuild
functions into host regressions. The 31 cases cover setup/rollback and rebuild
with and without NAPI; AddressSanitizer and UndefinedBehaviorSanitizer runs pass.
They model external kernel services and do not prove real DMA, NAPI, allocator
failure or concurrency safety.

The pre-push local `xsktest` rebuild could not run because the environment lacks
the libxdp development header `xdp/xsk.h`; no packages were installed to bypass
that prerequisite. Host regression success is not a substitute for that build.

The already-installed driver has supported actual Mayami single-port dual-stack
zero-copy tests. **This repaired module has not been loaded or hardware fault
tested.** Its validation must remain separate from those successful data-path
tests. Persistent installation and full two-port/long-running acceptance are not
part of this checkpoint.
