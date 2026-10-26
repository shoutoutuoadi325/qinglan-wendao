#!/usr/bin/env python3
"""Restore three small UAT compile inputs omitted by a Mac-only GitDependencies sync."""
import argparse
import datetime
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

REQUIRED_FILES = (
    'Engine/Binaries/ThirdParty/IOS/BouncyCastle.Crypto.dll',
    'Engine/Source/Programs/AutomationTool/IOS/Resources/GreenCheck.png',
    'Engine/Source/Programs/AutomationTool/IOS/Resources/GreyCheck.png',
)
OFFICIAL_BASE_URL = 'https://cdn.unrealengine.com/dependencies'
MAX_COMPRESSED_PACK = 4 * 1024 * 1024
MAX_RAW_PACK = 8 * 1024 * 1024
MAX_TOTAL_DOWNLOAD = 8 * 1024 * 1024


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def checked_hash(value):
    if not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Invalid manifest SHA1')
    return value


def inside(root, path):
    if not path.resolve().is_relative_to(root):
        raise ValueError(f'Path escapes its declared root: {path}')
    if path.is_symlink():
        raise ValueError(f'Refusing a symlink input: {path}')
    return path


def publish_new(path, data, mode, validate_existing):
    """Create a complete file atomically; never replace an existing input."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.qinglan-uat-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        os.chmod(temporary, mode)
        try:
            os.link(temporary, path)
            return 'restored'
        except FileExistsError:
            validate_existing(path.read_bytes())
            return 'concurrent-present-verified'
    finally:
        Path(temporary).unlink(missing_ok=True)


def validate_pack(compressed, pack):
    if len(compressed) != pack['compressed_size']:
        raise ValueError('Compressed pack size mismatch')
    with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
        raw = stream.read(MAX_RAW_PACK + 1)
    if len(raw) != pack['size'] or sha1(raw) != pack['hash']:
        raise ValueError('Uncompressed pack size/SHA1 mismatch')
    return raw


def restore_inputs(engine, cache, evidence, verify_only=False):
    engine, cache = Path(engine).resolve(), Path(cache).resolve()
    evidence = Path(evidence).resolve()
    if evidence.is_relative_to(engine) or evidence.is_relative_to(cache):
        raise ValueError('Evidence must be outside the engine and dependency cache')
    manifest = engine / 'Engine/Build/Commit.gitdeps.xml'
    manifest_data = manifest.read_bytes()
    root = ET.fromstring(manifest_data)
    if root.attrib.get('BaseUrl', '').rstrip('/') != OFFICIAL_BASE_URL:
        raise ValueError('Manifest must use the official HTTPS dependency CDN')
    files = {item.attrib['Name']: item.attrib for item in root.find('Files')}
    blobs = {item.attrib['Hash']: item.attrib for item in root.find('Blobs')}
    packs = {item.attrib['Hash']: item.attrib for item in root.find('Packs')}
    selected, required_packs = [], {}
    # Validate every existing target before acquiring or writing any dependency.
    for name in REQUIRED_FILES:
        source = files[name]
        file_hash = checked_hash(source['Hash'])
        blob = blobs[file_hash]
        pack_hash = checked_hash(blob['PackHash'])
        source_pack = packs[pack_hash]
        compressed_size, raw_size = int(source_pack['CompressedSize']), int(source_pack['Size'])
        if not 0 < compressed_size <= MAX_COMPRESSED_PACK or not 0 < raw_size <= MAX_RAW_PACK:
            raise ValueError('Required pack exceeds the small-input size limit')
        if not re.fullmatch(r'UnrealEngine-[0-9]+', source_pack['RemotePath']):
            raise ValueError('Invalid official dependency remote path')
        size, offset = int(blob['Size']), int(blob['PackOffset'])
        if size < 0 or offset < 0 or offset + size > raw_size:
            raise ValueError('Blob range exceeds its declared pack')
        target = inside(engine, engine / name)
        if evidence == target:
            raise ValueError('Evidence path must not overwrite a dependency')
        item = dict(name=name, sha1=file_hash, size=size, offset=offset, pack=pack_hash,
                    executable=source.get('IsExecutable') == 'true', target=target)
        if target.exists():
            if target.stat().st_size != size:
                raise ValueError(f'Existing dependency differs; refusing overwrite: {name}')
            data = target.read_bytes()
            if len(data) != size or sha1(data) != file_hash:
                raise ValueError(f'Existing dependency differs; refusing overwrite: {name}')
            item['status'] = 'already-present-verified'
        else:
            if verify_only:
                raise ValueError(f'Required dependency missing in verify-only mode: {name}')
            required_packs[pack_hash] = dict(hash=pack_hash, compressed_size=compressed_size,
                                           size=raw_size, remote=source_pack['RemotePath'])
        selected.append(item)
    if len(required_packs) > 2 or sum(p['compressed_size'] for p in required_packs.values()) > MAX_TOTAL_DOWNLOAD:
        raise ValueError('Required dependencies exceed the two-small-pack download limit')
    raw_packs, pack_evidence = {}, []
    download_bytes = 0
    for pack_hash, pack in required_packs.items():
        cache_file = inside(cache, cache / pack_hash[:2] / pack_hash)
        if evidence == cache_file:
            raise ValueError('Evidence path must not overwrite a pack')
        if cache_file.exists():
            if cache_file.stat().st_size != pack['compressed_size']:
                raise ValueError('Compressed pack size mismatch')
            compressed = cache_file.read_bytes()
            raw = validate_pack(compressed, pack)
            origin = 'cache'
        else:
            url = f"{OFFICIAL_BASE_URL}/{pack['remote']}/{pack_hash}"
            with urllib.request.urlopen(url, timeout=90) as response:
                if response.geturl() != url:
                    raise ValueError('Dependency download redirected away from its official manifest URL')
                compressed = response.read(pack['compressed_size'] + 1)
            raw = validate_pack(compressed, pack)
            publish_new(cache_file, compressed, 0o644, lambda data: validate_pack(data, pack))
            download_bytes += len(compressed)
            origin = 'official-download'
        raw_packs[pack_hash] = raw
        pack_evidence.append(dict(sha1=pack_hash, compressed_bytes=pack['compressed_size'],
                                  uncompressed_bytes=pack['size'], source=origin))
    # Verify all extracted slices before publishing any of the target files.
    pending = []
    for item in selected:
        if 'status' in item:
            continue
        data = raw_packs[item['pack']][item['offset']:item['offset'] + item['size']]
        if len(data) != item['size'] or sha1(data) != item['sha1']:
            raise ValueError(f"Extracted file SHA1 mismatch: {item['name']}")
        pending.append((item, data))
    for item, data in pending:
        def validate_existing(existing, expected=item):
            if len(existing) != expected['size'] or sha1(existing) != expected['sha1']:
                raise ValueError(f"Concurrent dependency differs; refusing overwrite: {expected['name']}")
        item['status'] = publish_new(item['target'], data, 0o755 if item['executable'] else 0o644,
                                     validate_existing)
    result = dict(timestamp_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  engine=str(engine), manifest_sha256=hashlib.sha256(manifest_data).hexdigest(),
                  verify_only=verify_only, download_bytes=download_bytes, packs=pack_evidence,
                  files=[{k: v for k, v in item.items() if k not in ('target', 'offset', 'executable')}
                         for item in selected],
                  scope='Official immutable inputs only; no Unreal, UBT, or UAT build executed')
    evidence.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.qinglan-uat-evidence-', dir=evidence.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
        os.replace(temporary, evidence)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True, type=Path)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--verify-only', action='store_true', help='Check existing inputs without restoring or downloading')
    args = parser.parse_args()
    try:
        result = restore_inputs(args.engine, args.cache, args.evidence, args.verify_only)
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        parser.exit(1, f'restore-uat-inputs: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
