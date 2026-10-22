# Mac Shipping package boot (headless, 2026-10-21)

The archived package was launched with `-nullrhi` so no WindowServer surface is required:
```
"<repo>/.local/acceptance/20261021T085259.654345Z/package/Mac/QinglanAscension-Mac-Shipping.app/Contents/MacOS/QinglanAscension-Mac-Shipping" -nullrhi -windowed -nosplash -ResX=1280 -ResY=720 -UserDir=<isolated> -QinglanShot=nullrhi -log -abslog=<isolated>/client.log
```

The process initialized the engine and entered the tick loop (sampled at 90 s, ~3.7% CPU, sleeping on the max-tick-rate timer). The Shipping target writes no log file, so this confirms process/engine boot only, not the hub frame or gameplay.

```text
    423 Thread_18128145: GameThread
    + 423 thread_start  (in libsystem_pthread.dylib) + 8  [0x196c40cec]
    +   423 _pthread_start  (in libsystem_pthread.dylib) + 136  [0x196c45d00]
    +     423 __NSThread__start__  (in Foundation) + 732  [0x198402ca8]
    +       423 -[FCocoaGameThread main]  (in QinglanAscension-Mac-Shipping) + 184  [0x1036f72ac]
    +         423 -[UEAppDelegate runGameThread:]  (in QinglanAscension-Mac-Shipping) + 76  [0x1076cc07c]
    +           423 GuardedMain(char16_t const*)  (in QinglanAscension-Mac-Shipping) + 616  [0x1076c26c4]
    +             419 FEngineLoop::Tick()  (in QinglanAscension-Mac-Shipping) + 364  [0x1076c04c4]
    +             ! 417 UEngine::UpdateTimeAndHandleMaxTickRate()  (in QinglanAscension-Mac-Shipping) + 1068  [0x105cae5ec]
    +             ! : 417 usleep  (in libsystem_c.dylib) + 68  [0x196adaf6c]
    +             ! :   417 nanosleep  (in libsystem_c.dylib) + 220  [0x196adb054]
```

A packaged client with the Metal RHI still blocks in `FMacWindow::WaitForFullScreenTransition` while the display is locked; the boot-frame and rendered checks remain owner actions. See [mac-display-locked-2026-10-21.md](mac-display-locked-2026-10-21.md).
