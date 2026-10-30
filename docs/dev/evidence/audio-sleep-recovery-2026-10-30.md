# Audio runtime review — 2026-10-30

**Conclusion:** the overnight cold Profile-menu log does not establish a project audio lifecycle or repeated-submission defect. Its timing and renderer stacks strongly support audio hardware callbacks stopping around Mac sleep/dark wake, with limited recovery in the pinned CoreAudio backend. This is a remaining physical audio validation item, not evidence that the game repeatedly starts music.

## Runtime evidence

Source log: .local/gameplay-qa/20261029/rebuilt-engine.log. Engine timestamps are UTC; pmset -g log timestamps are +0800.

- First hardware timeout: log line 1561, 2026-10-29 15:16:30.331 UTC. System power log: 2026-10-29 23:16:30 +0800, entering **Clamshell Sleep**. Dark wakes at 23:17:26 and 23:22:08 align with subsequent bursts of engine timeout logging; maintenance sleep follows each.
- All six queue diagnostics show render phase Finished, and all six stack dumps show FPThreadEvent::Wait -> IAudioMixerPlatformInterface::RunInternal. The named inflight task is the single nonprocedural hub music wave. At line 1592 the queue contains 9,525 commands with no completed pump for 159.61 seconds; later it contains 317,776 commands over 5,302.75 seconds. These rates are compatible with ordinary mixer updates accumulating while the consumer stalls. Command types are not logged, so the precise producer is unproven.
- There are 2,509 hardware timeout lines and six allocation warnings, ending at a reported 63 MB allocation. These are real engine queue-growth warnings; they are not themselves repeated PlaySound evidence.
- Full user/lid wake occurred at 2026-10-30 09:21:36 +0800. Last timeouts are at 01:21:36.328 and 01:21:38.926 UTC (lines 4178–4179); there are no later timeouts before normal requested exit at 01:32:34.988 UTC. This does **not** prove audible recovery.

## Project call path

Core/QinglanAudioSubsystem.cpp/.h has no tick, timer, or completion callback that resubmits music. SetMusicState (38–60) returns for the same state when its current-world component is playing; otherwise it fades out the old component and creates one new component. Hub BeginPlay calls it once before the cold-profile early return (Core/QinglanGameModeBase.cpp:291–308). Other calls are gameplay events. Imported music is authored with looping enabled.

UE CreateSound2D defaults to a world-associated, auto-destroyed, nonpersistent UI-sound component (GameplayStatics.cpp:1649–1688). UI music continuing during game pause is expected. The subsystem's lack of a Deinitialize override alone does not establish this failure or a leak.

## Backend evidence and limits

In the checked-in local UE 5.8.3 source (audio implementation last changed by 396c9f059903aed5fec78ecd3d437a40c6415368; targeted working-tree diffs for these audio implementation files were empty):

- CoreAudio's hardware callback calls ReadNextBuffer; that function triggers the event awaited by the renderer (AudioMixerPlatformCoreAudio.cpp:407–434, AudioMixer.cpp:583–660, 867–921). Timeout recovery calls RequestDeviceSwap, but FMixerPlatformCoreAudio directly inherits IAudioMixerPlatformInterface and does not override its default no-op/false implementation (AudioMixer.h:492). CoreAudio device-change and move-device methods also return false/TODO (AudioMixerPlatformCoreAudio.cpp:200–204, 332–336). This explains why that recovery request cannot repair this backend, but is not direct proof of the hardware failure's cause.
- Explicit suspend/resume methods stop/start the AUGraph, but neither marker occurs in the runtime log.
- Startup Error querying Sample Rate: 2003332927 has a source-level explanation: the backend reuses the default-device selector and device-ID size while querying the device into GraphSampleRate (85–105). GetPlatformDeviceInfo never supplies its name (338–350), explaining Unknown. Neither warning independently proves faulty/missing physical output.

## Remaining validation

Use real audible output: cold-menu idle while the Mac stays awake; lid sleep and full wake; default-output changes and headphone/Bluetooth reconnect if available. Check that music and gameplay SFX remain audible or recover, music does not duplicate, and queues do not continue growing after full wake. A silent/headless automation pass cannot establish these results. No engine launch, build, test, repository edit, or engine edit was performed for this review.
