"""Regression checks for exact audio recovery and refusal of unsafe inputs."""
import importlib.util
import os
from pathlib import Path
import struct
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("recover_audio_originals", ROOT/"tools/recover_audio_originals.py")
RECOVERY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECOVERY)
ASSET = ROOT/"game/Content/Qinglan/Audio/SFX/SW_sfx_hit_impact.uasset"


class AudioRecoverySchemaTests(unittest.TestCase):
    def setUp(self):
        self.data = ASSET.read_bytes()
        self.offset = self.data.index(RECOVERY.MAGIC)

    def test_existing_asset_metadata_and_block_contract(self):
        info = RECOVERY.source_info(self.data, "SFX", "sfx_hit_impact")
        blocks, header = RECOVERY.compressed_payload(self.data)
        self.assertEqual(info["FileMD5"], "80355c78e81b93ea006d7b08bb6464b9")
        self.assertEqual(header["raw_size"], 48554)
        self.assertEqual(len(blocks), 1)

    def test_unknown_package_schema_rejected(self):
        with self.assertRaisesRegex(ValueError, "package magic"):
            RECOVERY.source_info(b"BAD!"+self.data[4:], "SFX", "sfx_hit_impact")

    def test_corrupted_header_rejected(self):
        data = bytearray(self.data)
        data[self.offset+40] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC32"):
            RECOVERY.compressed_payload(data)

    def test_unknown_codec_rejected(self):
        data = bytearray(self.data)
        data[self.offset+8] = 4  # LZ4 is deliberately outside the supported schema.
        with self.assertRaisesRegex(ValueError, "codec schema"):
            RECOVERY.compressed_payload(data)

    def test_valid_crc_with_inconsistent_block_table_rejected(self):
        data = bytearray(self.data)
        data[self.offset+67] ^= 1
        struct.pack_into(">I", data, self.offset+4, zlib.crc32(data[self.offset+8:self.offset+68]))
        with self.assertRaisesRegex(ValueError, "block-size sum"):
            RECOVERY.compressed_payload(data)

    def test_truncated_payload_rejected(self):
        _, header = RECOVERY.compressed_payload(self.data)
        with self.assertRaisesRegex(ValueError, "truncated compressed payload"):
            RECOVERY.compressed_payload(self.data[:self.offset+header["compressed_size"]-1])

    def test_ambiguous_payload_rejected(self):
        with self.assertRaisesRegex(ValueError, "unambiguous"):
            RECOVERY.compressed_payload(self.data+RECOVERY.MAGIC)

    def test_conflicting_import_hashes_rejected(self):
        data = self.data.replace(b"80355c78e81b93ea006d7b08bb6464b9", b"00355c78e81b93ea006d7b08bb6464b9", 1)
        with self.assertRaisesRegex(ValueError, "conflicting import metadata"):
            RECOVERY.source_info(data, "SFX", "sfx_hit_impact")


@unittest.skipUnless(os.environ.get("QINGLAN_TEST_AUDIO_DECODE") == "1",
                     "optional native decode test requires local official libraries and blake3")
class AudioRecoveryNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        engine = ROOT/".local/toolchains/UnrealEngine"
        work = ROOT/".local/downloads/oodle-audio-recovery"
        # Explicitly guard against a test causing dependency downloads.
        if not (engine/RECOVERY.OODLE).is_file() and not (work/"5c593314bca2615530837f03e2ad1485ef64bc58.gz").is_file():
            raise unittest.SkipTest("official Oodle library/pack not cached")
        if not (engine/RECOVERY.UEWAV).is_file():
            raise unittest.SkipTest("official UEWavComp library not cached")
        cls.decoder = RECOVERY.Decoder(engine, work)

    def test_all_eight_originals_match_import_md5(self):
        for group, base in RECOVERY.EXPECTED:
            with self.subTest(base=base):
                data = (ROOT/"game/Content/Qinglan/Audio"/group/("SW_"+base+".uasset")).read_bytes()
                info = RECOVERY.source_info(data, group, base)
                wav, _ = self.decoder.decode(data, info)
                self.assertEqual(RECOVERY.digest(wav, "md5"), info["FileMD5"])

    def test_wrong_import_md5_rejected(self):
        data = ASSET.read_bytes()
        info = RECOVERY.source_info(data, "SFX", "sfx_hit_impact")
        info["FileMD5"] = "0"*32
        with self.assertRaisesRegex(ValueError, "does not match import FileMD5"):
            self.decoder.decode(data, info)


if __name__ == "__main__":
    unittest.main()
