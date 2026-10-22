# Exact original audio recovery — 2026-10-20

All eight source WAVs were restored under `.local/assets-raw/audio/original/`.
Every recovered file matches the `FileMD5` recorded in its committed SoundWave import metadata.
The generator script remains absent; these are recovered originals, with no synthesized replacement.

The extraction follows the matching UE source checkout (`396c9f059903aed5fec78ecd3d437a40c6415368`):

1. Parse the unique 64-byte big-endian `FCompressedBuffer` header and block table. Validate CRC32 and size bounds.
2. Decompress each Oodle Kraken block through the official Oodle 2.9.16 library. Validate the complete raw BLAKE3.
3. Reverse the inner UEWavComp transform through the official `uewav_decode16` routine, as `FEditorAudioBulkData::GetPayload` does, and restore the PCM format tag.
4. Validate mono 44.1 kHz, 16-bit PCM and the import MD5. All eight files must pass before any WAV is written.

The Oodle library was taken from its manifest-defined official CDN dependency pack (987,406 bytes compressed). Both pack and blob SHA1 were checked. The existing UEWavComp static library was also checked against the manifest. A small C wrapper was linked with macOS Command Line Tools; the engine was not built or started.

| Original WAV | Duration | Import MD5 = recovered MD5 |
| --- | ---: | --- |
| `audio/original/music/music_boss_epic_boss_battle_loop.wav` | 12.00 s | `28225cd0adc93839b08f8a0fedf45b4c` |
| `audio/original/music/music_combat_alt_prepare_your_swords.wav` | 10.00 s | `29a1393af9f4e9775e4bae242316af96` |
| `audio/original/music/music_combat_fast_fight_loop.wav` | 8.00 s | `67739464110e2278d00866f041548854` |
| `audio/original/music/music_hub_calm_orient_peace_valley.wav` | 16.00 s | `81e763792f735cae6b703ab7205e9c7b` |
| `audio/original/sfx/sfx_sword_swing.wav` | 0.50 s | `85494a2ac8a138856e718925fca2bf2f` |
| `audio/original/sfx/sfx_hit_impact.wav` | 0.55 s | `80355c78e81b93ea006d7b08bb6464b9` |
| `audio/original/sfx/sfx_technique_release.wav` | 1.00 s | `4a39d3933746400fbe454cdf6809a66a` |
| `audio/original/sfx/sfx_telegraph_warn.wav` | 0.80 s | `382ed204383f6c177f5feea16185ad06` |


Full SHA256 values, source asset hashes, buffer offsets, block counts and library receipts are in [the JSON evidence](audio-original-recovery-2026-10-20.json).

Reproduce after installing `blake3` into the workspace dependency directory:

```sh
python3 -m pip install --target .local/python-packages blake3
PYTHONPATH=.local/python-packages python3 tools/recover_audio_originals.py \
  --engine-root .local/toolchains/UnrealEngine \
  --assets-raw .local/assets-raw \
  --work-dir .local/downloads/oodle-audio-recovery \
  --report .local/assets-raw/provenance/audio-recovery-check.json --verify-only
QINGLAN_TEST_AUDIO_DECODE=1 PYTHONPATH=.local/python-packages \
  python3 -m unittest discover -s tools/tests -p test_audio_original_recovery.py -v
```

All 10 tests passed, including header corruption, unknown codec, inconsistent block sizes, truncation, ambiguous payloads, conflicting import metadata and wrong final MD5 rejection. The native test independently decodes all eight originals. A separate `--verify-only` run also passed. No committed Content binary was changed.
