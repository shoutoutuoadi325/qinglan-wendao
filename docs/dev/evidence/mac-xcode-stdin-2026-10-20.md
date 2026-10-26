# Mac Xcode app-finalization repair

Host: M4 MacBook Air, macOS 27.0.1, Xcode 27.1. Engine HEAD:
`396c9f059903aed5fec78ecd3d437a40c6415368` (UE 5.8.3).

The first native editor attempt compiled and linked the project modules but
failed its Xcode app-finalization step. Xcode reported a failed
`Touch UBT generated tiles` shell pre-action without a script diagnostic.
Disabling UBA detouring did not fix it.

Controlled direct Xcode invocations against the same generated workspace:

| Invocation | Exit | Result |
| --- | --- | --- |
| Normal stdin, workspace temp root, result bundle enabled | 0 | Build succeeded |
| Normal stdin, UBA's longer temp root | 0 | Build succeeded |
| Normal stdin, no result-bundle argument | 0 | Build succeeded |
| File descriptor 0 closed, otherwise normal environment | 65 | Same shell pre-action failure |

Logs and result bundles are under `.local/engine-build/`, named
`xcode-postbuild-attempt1`, `xcode-postbuild-long-tmp`,
`xcode-postbuild-no-resultbundle` and `xcode-postbuild-closed-stdin`.
The last case closes descriptor 0 in the child before launching Xcode.
Running the generated pre-action script directly also exits 0.

The locked engine's `UbaProcess.cpp` sets `POSIX_SPAWN_CLOEXEC_DEFAULT` on Mac
and explicitly maps stdout/stderr, leaving stdin unpreserved. Its executor
still uses the UBA process launcher when detouring is disabled. The tracked
[host-tool patch](../../../tools/build/patches/ue583-mac-xcode-stdin.patch)
sets `RedirectStandardInput = true` for the Xcode process in
`AppleToolChain.cs`, supplying a valid pipe.

After applying it, `BuildUBT.sh` builds and publishes with zero warnings/errors.
The regular project build wrapper then returns 0 with `Result: Succeeded`:
`.local/acceptance/20261020T095017.651759Z/01-build-editor.log`.
Its command line uses the normal UBA configuration. The successful incremental
build contains 259 actions and takes 14.63 seconds, reusing the earlier native
compilation. The editor app is a Mach-O arm64 executable.

This establishes the native editor build and the workaround for this host.
It does not establish content regeneration, automation-test success, packaged
gameplay, GPU performance or target-platform release acceptance.
