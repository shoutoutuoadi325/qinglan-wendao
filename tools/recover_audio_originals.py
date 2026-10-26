#!/usr/bin/env python3
"""Recover the eight original WAVs from the committed UE editor SoundWaves.

Requires the matching UE source checkout, Python blake3, and macOS clang (CLT
is sufficient). Uses official Oodle/UEWavComp libraries; does not build UE or
implement either codec. Writes no WAV until all eight import MD5s match.
"""
import argparse
import ctypes
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import tempfile
from urllib.request import urlopen
import wave
import xml.etree.ElementTree as ET
import zlib

EXPECTED = (
    ("Music", "music_boss_epic_boss_battle_loop"),
    ("Music", "music_combat_alt_prepare_your_swords"),
    ("Music", "music_combat_fast_fight_loop"),
    ("Music", "music_hub_calm_orient_peace_valley"),
    ("SFX", "sfx_sword_swing"),
    ("SFX", "sfx_hit_impact"),
    ("SFX", "sfx_technique_release"),
    ("SFX", "sfx_telegraph_warn"),
)
MAGIC = bytes.fromhex("b7756362")
HEADER = struct.Struct(">IIBBBBIQQ32s")
IMPORT_JSON = re.compile(rb'\[\s*\{\s*"RelativeFilename"\s*:.*?\}\s*\]')
OODLE = "Engine/Source/Runtime/OodleDataCompression/Sdks/2.9.16/lib/Mac/liboo2coremac64.2.9.16.dylib"
UEWAV = "Engine/Source/Runtime/UEWavComp/Lib/uewav_mac.a"
WRAPPER = """extern void uewav_decode16(short *, short *, long long, long long);
__attribute__((visibility("default")))
void qinglan_uewav_decode16(short *s, short *scratch, long long n, long long c)
{ uewav_decode16(s, scratch, n, c); }
"""


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data, algorithm="sha256"):
    return hashlib.new(algorithm, data).hexdigest()


def source_info(data, group, base):
    """Read serialized FAssetImportInfo JSON; reject conflicting duplicates."""
    require(data.startswith(bytes.fromhex("c1832a9e")), "unsupported package magic")
    records = []
    for match in IMPORT_JSON.finditer(data):
        raw = match.group()
        require(match.start() >= 4 and struct.unpack_from("<i", data, match.start()-4)[0] == len(raw)+1,
                "unsupported import FString serialization")
        parsed = json.loads(raw)
        require(len(parsed) == 1, "expected exactly one import source")
        records.append(parsed[0])
    require(records and all(record == records[0] for record in records), "missing or conflicting import metadata")
    info = records[0]
    relative = PurePosixPath(info["RelativeFilename"])
    expected_suffix = ("audio", "original", group.lower(), base + ".wav")
    require(relative.parts[-4:] == expected_suffix, "unexpected import source path")
    require(re.fullmatch(r"[0-9a-f]{32}", info.get("FileMD5", "")) is not None, "missing import MD5")
    return info


def compressed_payload(data):
    """Accept only this checkout's unique UE FCompressedBuffer Oodle schema."""
    offsets = [match.start() for match in re.finditer(re.escape(MAGIC), data)]
    require(len(offsets) == 1, "expected one unambiguous compressed buffer")
    offset = offsets[0]
    require(len(data)-offset >= HEADER.size, "truncated compressed header")
    magic, crc, method, compressor, level, exponent, count, raw_size, size, raw_hash = HEADER.unpack_from(data, offset)
    require((method, compressor, level, exponent) == (3, 3, 3, 18), "unsupported compressed-buffer codec schema")
    require(44 <= raw_size <= 64*1024*1024, "unsafe raw payload size")
    require(count == (raw_size+(1 << exponent)-1) >> exponent, "inconsistent block count")
    header_size = HEADER.size + count*4
    require(header_size <= size <= len(data)-offset, "truncated compressed payload")
    header = data[offset:offset+header_size]
    require(zlib.crc32(header[8:]) == crc, "compressed header CRC32 mismatch")
    sizes = struct.unpack_from(">" + "I"*count, header, HEADER.size)
    require(header_size+sum(sizes) == size, "compressed block-size sum mismatch")
    blocks = []
    cursor = offset+header_size
    for index, block_size in enumerate(sizes):
        raw_block_size = min(1 << exponent, raw_size-(index << exponent))
        require(0 < block_size <= raw_block_size, "invalid compressed block size")
        blocks.append((data[cursor:cursor+block_size], raw_block_size))
        cursor += block_size
    return blocks, {"offset": offset, "method": method, "compressor": compressor,
                    "level": level, "block_size_exponent": exponent, "block_count": count,
                    "raw_size": raw_size, "compressed_size": size, "header_crc32": f"{crc:08x}",
                    "raw_blake3": raw_hash.hex()}


def official_library(engine, work, name):
    """Read one manifest blob; download only its bounded official pack if absent."""
    manifest = ET.parse(engine / "Engine/Build/Commit.gitdeps.xml").getroot()
    file = next(item for item in manifest.find("Files") if item.get("Name") == name)
    file_hash = file.get("Hash")
    local = engine/name
    receipt = {"manifest_file": name, "sha1": file_hash}
    if local.is_file():
        data = local.read_bytes()
        require(digest(data, "sha1") == file_hash, "local official-library SHA1 mismatch")
        receipt["source"] = "existing engine dependency"
    else:
        blob = next(item for item in manifest.find("Blobs") if item.get("Hash") == file_hash)
        pack = next(item for item in manifest.find("Packs") if item.get("Hash") == blob.get("PackHash"))
        require(int(pack.get("CompressedSize")) < 100*1024*1024, "refusing dependency pack >=100 MiB")
        url = f'{manifest.get("BaseUrl")}/{pack.get("RemotePath")}/{pack.get("Hash")}'
        cached = work / (pack.get("Hash") + ".gz")
        if not cached.is_file():
            with urlopen(url, timeout=60) as response:
                encoded = response.read(int(pack.get("CompressedSize"))+1)
            require(len(encoded) == int(pack.get("CompressedSize")), "dependency download size mismatch")
            cached.write_bytes(encoded)
        encoded = cached.read_bytes()
        require(len(encoded) == int(pack.get("CompressedSize")), "cached dependency size mismatch")
        raw = gzip.decompress(encoded)
        require(len(raw) == int(pack.get("Size")) and digest(raw, "sha1") == pack.get("Hash"), "dependency pack SHA1/size mismatch")
        start, size = int(blob.get("PackOffset")), int(blob.get("Size"))
        data = raw[start:start+size]
        require(len(data) == size and digest(data, "sha1") == file_hash, "official-library blob SHA1 mismatch")
        receipt.update(source=url, pack_sha1=pack.get("Hash"), pack_offset=start, pack_size=len(raw))
    target = work/Path(name).name
    target.write_bytes(data)
    receipt["sha256"] = digest(data)
    return target, receipt


class Decoder:
    def __init__(self, engine, work):
        try:
            from blake3 import blake3
        except ImportError as error:
            raise ValueError("install blake3 locally and include it in PYTHONPATH") from error
        self.blake3 = blake3
        work.mkdir(parents=True, exist_ok=True)
        oodle, self.oodle_receipt = official_library(engine, work, OODLE)
        uewav, self.uewav_receipt = official_library(engine, work, UEWAV)
        wrapper = work/"uewav-wrapper.c"
        wrapper.write_text(WRAPPER)
        dynamic = work/"libuewav.dylib"
        command = ["clang", "-dynamiclib", str(wrapper), str(uewav), "-o", str(dynamic)]
        subprocess.run(command, check=True)
        self.wrapper_receipt = {"command": command, "source": WRAPPER,
                                "linked_library_sha256": digest(dynamic.read_bytes())}
        self.oodle_lib = ctypes.CDLL(str(oodle.resolve()))
        self.uewav_lib = ctypes.CDLL(str(dynamic.resolve()))
        self.oodle = self.oodle_lib.OodleLZ_Decompress
        self.oodle.restype = ctypes.c_ssize_t
        self.oodle.argtypes = [ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_ssize_t,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t,
                              ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_int]
        self.uewav = self.uewav_lib.qinglan_uewav_decode16
        self.uewav.restype = None
        self.uewav.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_longlong, ctypes.c_longlong]

    def decode(self, data, info):
        blocks, header = compressed_payload(data)
        chunks = []
        for block, size in blocks:
            if len(block) == size:
                chunks.append(block)
            else:
                target = ctypes.create_string_buffer(size)
                actual = self.oodle(block, len(block), target, size, 1, 1, 0, None, 0, None, None, None, 0, 3)
                require(actual == size, "Oodle block decompression failed")
                chunks.append(target.raw)
        encoded_wave = b"".join(chunks)
        require(self.blake3(encoded_wave).hexdigest() == header["raw_blake3"], "compressed payload BLAKE3 mismatch")
        # Deliberately accept only the exact canonical mono RIFF layout in
        # these assets. FEditorAudioBulkData::GetPayload reverses UEWavComp
        # and sets the format tag from OODLE_WAVE (0xffff) back to PCM (1).
        require(encoded_wave[:4] == b"RIFF" and encoded_wave[8:12] == b"WAVE", "not RIFF/WAVE")
        require(struct.unpack_from("<I", encoded_wave, 4)[0]+8 == len(encoded_wave), "RIFF size mismatch")
        require(encoded_wave[12:20] == b"fmt \x10\0\0\0" and encoded_wave[36:40] == b"data", "unsupported WAV chunk layout")
        fmt = struct.unpack_from("<HHIIHH", encoded_wave, 20)
        require(fmt == (0xffff, 1, 44100, 88200, 2, 16), "unsupported UEWavComp sample format")
        sample_bytes = struct.unpack_from("<I", encoded_wave, 40)[0]
        require(sample_bytes == len(encoded_wave)-44 and sample_bytes % 2 == 0, "PCM size mismatch")
        target = ctypes.create_string_buffer(encoded_wave, len(encoded_wave))
        scratch = ctypes.create_string_buffer(sample_bytes)
        self.uewav(ctypes.byref(target, 44), scratch, sample_bytes//2, 1)
        wav = bytearray(target.raw)
        struct.pack_into("<H", wav, 20, 1)
        require(digest(wav, "md5") == info["FileMD5"], "recovered WAV does not match import FileMD5")
        with wave.open(io.BytesIO(wav), "rb") as audio:
            require((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (1, 2, 44100), "invalid recovered PCM")
            frames = audio.getnframes()
        return bytes(wav), dict(header, frames=frames, sample_rate=44100, channels=1, bits_per_sample=16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--assets-raw", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    decoder = Decoder(args.engine_root.resolve(), args.work_dir.resolve())
    pending = []
    for group, base in EXPECTED:
        asset = repo/"game/Content/Qinglan/Audio"/group/("SW_"+base+".uasset")
        data = asset.read_bytes()
        info = source_info(data, group, base)
        wav, details = decoder.decode(data, info)
        target = args.assets_raw/"audio/original"/group.lower()/(base+".wav")
        require(not target.exists() or target.read_bytes() == wav, "refusing to overwrite different existing WAV")
        record = dict(details, asset=str(asset.relative_to(repo)), asset_sha256=digest(data),
                      relative_output=f"audio/original/{group.lower()}/{base}.wav", bytes=len(wav),
                      import_md5=info["FileMD5"], recovered_md5=digest(wav, "md5"), sha256=digest(wav))
        pending.append((target, wav, record))
    # All eight originals have been validated before any destination write.
    if not args.verify_only:
        for target, wav, _ in pending:
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as temporary:
                temporary.write(wav)
                temporary_path = Path(temporary.name)
            temporary_path.replace(target)
    report = {"method": "official Oodle Kraken + official UEWavComp decode; no synthesis; no engine build",
              "engine_revision": subprocess.check_output(
                  ["git", "-C", str(args.engine_root), "rev-parse", "HEAD"], text=True).strip(),
              "verified_originals": len(pending), "all_import_md5_match": True, "verify_only": args.verify_only,
              "oodle_library": decoder.oodle_receipt, "uewav_library": decoder.uewav_receipt,
              "uewav_wrapper": decoder.wrapper_receipt,
              "source_contracts": ["Core/Private/Compression/CompressedBuffer.cpp FHeader/FBlockDecoder",
                                   "Engine/Private/SoundWave.cpp FEditorAudioBulkData::GetPayload",
                                   "Engine/Private/EditorFramework/AssetImportData.cpp FAssetImportInfo::ToJson"],
              "files": [record for _, _, record in pending]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+"\n")
    print(f"Verified {len(pending)} exact originals; " + ("no WAVs written" if args.verify_only else "restored all WAVs"))


if __name__ == "__main__":
    main()
