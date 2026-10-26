# Mac display session state (2026-10-21)

The packaged Mac client was built and staged, but the owner's display session
was locked while this evidence pass ran. A locked session has no presentable
WindowServer surface, so the client cannot complete the window transition and
never reaches its first rendered frame.

## Measured state (16:52-16:57 CST)

```text
$ ioreg -n Root -d1 -a | plutil -convert json -o - - | jq ...
IOConsoleLocked: true
IOConsoleUsers[0].CGSSessionScreenIsLocked: true
IOConsoleUsers[0].CGSSessionScreenLockedTime: 1792571034
IOConsoleUsers[0].kCGSSessionUserNameKey: <owner>
```

A full-display capture at the same time is black:
`docs/dev/evidence/rendered/mac-display-asleep-black-2026-10-21.png`
(3840x2160 PNG produced by `screencapture -x`).

## Client behavior in this state (before the window-mode fix)

At capture time the startup graphics service called
`UGameUserSettings::ApplySettings(false)`, which ignored `-windowed` and
re-applied the saved fullscreen mode. Launching the staged Shipping app
(directly or through LaunchServices with `-windowed -ResX=1280 -ResY=720
-QinglanShot=...`) therefore blocked before any engine log was written. A 1 ms `sample` shows the game thread in:

```text
FEngineLoop::Init()
  UGameEngine::SwitchGameWindowToUseGameViewport()
    FSceneViewport::ResizeFrame()
      SWindow::SetWindowMode()
        FMacWindow::ApplySizeAndModeChanges()
          FMacWindow::UpdateFullScreenState()
            FMacWindow::WaitForFullScreenTransition()
              FGenericPlatformProcess::Sleep() / ProcessGameThreadEvents()
```

`WaitForFullScreenTransition` polls until the native window reports the target
mode; a locked session never delivers that transition. The process spins at
~100% CPU, writes no log, and produces no screenshot. No evidence file was
fabricated from this attempt.

The settings bug is fixed (`bCheckForCommandLineOverrides=true`), so the
packaged client now starts windowed and rendered the frames in
[metal-rendering-2026-10-21.md](metal-rendering-2026-10-21.md). The lock still
blocks menu input and screen capture.

## What remains owner-only

Unlock the display session and select a profile slot, then capture the hub
gameplay frame and run the interactive/performance routes:
`QINGLAN_TRIAL` capture for AUD-42, the hub frame for AUD-59, and the
performance procedure in `performance-capture.md` for AUD-55. The headless
suite, both campaign journeys and the packaging step do not need the display.
