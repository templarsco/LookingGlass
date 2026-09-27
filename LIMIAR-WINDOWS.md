# Limiar Windows Client Workstream

This is Limiar's development fork of Looking Glass. Its goal is a native
Windows viewer on the physical PC, not just the Windows guest capturer.
The Windows client only shows the client's own synthetic test frames so
far; it cannot display a guest. Creating this fork is not a claim of
working guest display, GPU sharing, or 240 Hz performance.

Upstream starting revision:
`236efcb155f952f5d7d9fcd5891a3060ad254e68`.

The `master` branch preserves the upstream baseline. Limiar development is
on `limiar/windows-client`. Upstream licenses, copyright notices and
submodule origins remain intact. This component is separate from Limiar's
MIT/Apache-licensed launcher. The Windows client build and its tests live
in [windows-client](windows-client/README.md).

## Implementation Sequence

1. Build the shared protocol and applicable common Windows code with pinned
   dependencies. Preserve Linux builds. Do not assume that B7 and current
   IDD/LGProtocol components are wire-compatible.
   Status, September 27, 2026: LGProtocol, LGMP and `common` build for the
   Windows client with MinGW-w64, cross and native. A bounded KVMFR frame
   loopback test runs in CI on Windows and Linux. No window, local endpoint
   or guest is involved yet.
2. Add a Win32 display/input implementation for the existing client
   abstraction: DPI, resize, focus, cursor capture/release, relative mouse,
   keyboard state and disconnect recovery.
   Status, September 27, 2026: the client builds for Windows, cross and
   native, with a Win32 display server that covers these items. Windows
   only has the synthetic `test` transport. CI shows a known test frame in
   a native window and checks every pixel, using Mesa's software OpenGL;
   the endpoint half of the first gate below waits for step 3. The Win32
   input layer has unit tests. Focus, capture, DPI and reconnect behavior
   was checked by hand under Wine, not yet on a physical Windows PC.
3. Add an explicit Windows local shared-memory endpoint with per-user ACLs
   and a negotiated protocol version. Do not reinterpret a Linux DMA-BUF
   file descriptor as a Windows handle. Never expose an unauthenticated
   capture/input endpoint.
4. Present actual guest frames using a native Windows graphics path; verify
   nonblank content, correct stride/format and synchronization. A CPU-copy
   bring-up path must be reported as such, not advertised as zero-copy.
5. Integrate the QEMU transport/guest device and the compatible Windows
   capture component. GPU delivery is a separate prerequisite owned by
   Limiar's VM backend, not something the viewer creates.
   Limiar selected native Hyper-V with OpenHCL on September 25, 2026, after
   this sequence was written. IVSHMEM is a QEMU device, so the guest-to-host
   transport for that runtime is still an open design decision.
6. Add audio and complete reconnect/resolution-change handling. Measure
   frame pacing and input/display latency at 60/120/240 Hz on the actual
   host; a configured refresh rate is not a performance result.

## Initial Acceptance Gates

- One known test frame crosses the local endpoint and reaches a native
  Windows window with checked pixel contents.
- An animated producer survives resize, focus changes and reconnects.
- A real VM frame stream works without host desktop capture as a substitute.
- Input is released on disconnect/focus loss and cannot target another VM.
- The Windows client, Linux client and guest components retain their own
  bounded tests and reproducible build/version records.

This workstream does not wait for upstream to publish a Windows client.
Useful fixes should remain reviewable for contribution upstream.

Related VM project: [Limiar](https://github.com/templarsco/limiar).
