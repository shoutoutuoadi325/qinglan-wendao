#!/usr/bin/env python3
"""Bind a Win64 editor cache to build inputs and verify its restored payload.

This is a reproducibility/integrity gate, not a runtime test. Source trees are
hashed by content (including local edits and new files), not just Git revision.
Generated compiler outputs are excluded from input identity and inventoried
separately as artifacts. Only exact matches may skip the editor build.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

VERSION = 1
MANIFEST_NAME = "editor-cache-manifest.json"
ARCHIVES = ("editor-engine.7z", "editor-game.7z")
# Content is consumed from the current checkout by the cooker, not compiled into editor DLLs.
GENERATED = {"binaries", "intermediate", "saved", "receipts", "content", ".git", "__pycache__"}
PROJECT_FILES = (
    "game/QinglanAscension.uproject", "tools/build/versions.lock",
    ".github/workflows/windows-build.yml", "tools/ci/windows-build-editor.ps1",
    "tools/ci/windows-editor-cache.ps1", "tools/ci/windows-engine.ps1",
    "tools/ci/windows-package.ps1", "tools/ci/editor_cache_manifest.py",
)
REQUIRED_ARTIFACTS = {
    "engine": (
        "Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
        "Engine/Binaries/Win64/ShaderCompileWorker.exe",
        "Engine/Binaries/Win64/UnrealPak.exe",
        "Engine/Binaries/Win64/UnrealEditor.modules",
    ),
    "project": (
        "game/Binaries/Win64/UnrealEditor-QinglanAscension.dll",
        "game/Binaries/Win64/UnrealEditor-QinglanAscensionEditor.dll",
        "game/Binaries/Win64/UnrealEditor.modules",
        "game/Binaries/Win64/QinglanAscensionEditor.target",
    ),
}


class CacheError(ValueError):
    pass


def file_record(path: Path) -> dict:
    if not path.is_file() or path.is_symlink():
        raise CacheError(f"Required regular file is missing: {path.name}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def tree_files(root: Path, *, inputs: bool = False):
    if not root.is_dir():
        return
    for directory, directories, filenames in os.walk(root):
        # C# bin/obj outputs change during UBT compilation. A third-party
        # library's similarly named bin directory is still a build input.
        managed_project = any(name.casefold().endswith(".csproj") for name in filenames)
        in_source = root.name.casefold() == "source" or any(
            part.casefold() == "source" for part in Path(directory).relative_to(root).parts)
        # Third-party libraries and embedded resources under Source are inputs,
        # including folders named Binaries or Content. Plugin output trees at
        # the plugin root still belong to the separate artifact inventory.
        excluded = GENERATED - {"binaries", "saved", "receipts", "content"} if in_source else GENERATED
        directories[:] = sorted(name for name in directories
                                if not inputs or (name.casefold() not in excluded
                                    and not (managed_project and name.casefold() in {"bin", "obj"})))
        for name in directories:
            if (Path(directory) / name).is_symlink():
                raise CacheError("Cache trees must not contain directory symlinks")
        for name in sorted(filenames):
            if inputs and (name in {".DS_Store", "Thumbs.db"} or name.endswith((".pyc", ".log", ".tmp"))):
                continue
            yield Path(directory) / name


def fingerprint(root: Path, files) -> dict:
    digest = hashlib.sha256()
    paths = sorted({path.relative_to(root).as_posix(): path for path in files}.items())
    for relative, path in paths:
        # Unambiguous records include names and bytes, catching add/delete/rename
        # as well as edits to an existing test whose registration name is stable.
        record = {"path": relative, **file_record(path)}
        digest.update(json.dumps(record, sort_keys=True, separators=(",", ":")).encode() + b"\n")
    return {"sha256": digest.hexdigest(), "file_count": len(paths)}


def build_inputs(repo: Path, engine: Path) -> dict:
    for required in PROJECT_FILES:
        if not (repo / required).is_file():
            raise CacheError(f"Missing project build input: {required}")
    for root, relative in ((repo, "game/Source"), (engine, "Engine/Source"), (engine, "Engine/Build/Build.version")):
        if not (root / relative).exists():
            raise CacheError(f"Missing build input: {relative}")
    project_files = [repo / name for name in PROJECT_FILES]
    for relative in ("game/Source", "game/Config", "game/Build", "game/Plugins"):
        project_files.extend(tree_files(repo / relative, inputs=True))
    engine_files = []
    # Include third-party headers/libraries and engine/plugin build rules. The
    # engine acquisition job removes .git, so HEAD/Build.version alone cannot
    # establish that the compiled code matches the current prepared engine.
    for relative in ("Engine/Source", "Engine/Build", "Engine/Config", "Engine/Shaders", "Engine/Plugins"):
        engine_files.extend(tree_files(engine / relative, inputs=True))
    return {"project": fingerprint(repo, project_files), "engine": fingerprint(engine, engine_files),
            "target": "QinglanAscensionEditor", "platform": "Win64", "configuration": "Development"}


def artifact_roots(root: Path, scope: str) -> list[Path]:
    prefix = "Engine" if scope == "engine" else "game"
    roots = [root / prefix / "Binaries/Win64", root / prefix / "Build/Receipts"]
    plugins = root / prefix / "Plugins"
    if plugins.exists():
        for directory, directories, _ in os.walk(plugins):
            if Path(directory).name == "Binaries" and "Win64" in directories:
                roots.append(Path(directory) / "Win64")
            directories[:] = [name for name in directories if name not in {"Binaries", "Intermediate", "Content"}]
    return [path for path in roots if path.is_dir()]


def artifact_inventory(repo: Path, engine: Path) -> dict:
    result = {}
    for scope, root in (("engine", engine), ("project", repo)):
        result[scope] = {path.relative_to(root).as_posix(): file_record(path)
                         for directory in artifact_roots(root, scope) for path in tree_files(directory)}
        for required in REQUIRED_ARTIFACTS[scope]:
            if required not in result[scope] or result[scope][required]["bytes"] == 0:
                raise CacheError(f"Missing compiled editor artifact: {required}")
    engine_modules = read_json(engine / "Engine/Binaries/Win64/UnrealEditor.modules")
    game_modules = read_json(repo / "game/Binaries/Win64/UnrealEditor.modules")
    build_id = engine_modules.get("BuildId")
    if not isinstance(build_id, str) or not build_id or build_id != game_modules.get("BuildId"):
        raise CacheError("Engine and project module BuildId values do not match")
    modules = game_modules.get("Modules")
    if not isinstance(modules, dict):
        raise CacheError("Project module manifest has no valid Modules map")
    for name in ("QinglanAscension", "QinglanAscensionEditor"):
        if modules.get(name) != f"UnrealEditor-{name}.dll":
            raise CacheError(f"Project module manifest does not identify {name}")
    return result


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        raise CacheError(f"Missing or invalid cache metadata: {path.name}") from error
    if not isinstance(value, dict):
        raise CacheError(f"Cache metadata must be an object: {path.name}")
    return value


def write_json(path: Path, value: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def snapshot(repo: Path, engine: Path, output: Path):
    write_json(output, {"version": VERSION, "inputs": build_inputs(repo, engine)})


def require_inputs(manifest: dict, repo: Path, engine: Path):
    if manifest.get("version") != VERSION:
        raise CacheError("Legacy or unsupported editor cache; rebuild the editor cache")
    current = build_inputs(repo, engine)
    if manifest.get("inputs") != current:
        raise CacheError("Editor cache build inputs differ from this checkout/engine; rebuild the editor cache")


def seal(repo: Path, engine: Path, archive: Path, before_build: Path):
    identity = read_json(before_build)
    require_inputs(identity, repo, engine)
    manifest = {**identity, "artifacts": artifact_inventory(repo, engine),
                "archives": {name: file_record(archive / name) for name in ARCHIVES}}
    write_json(archive / MANIFEST_NAME, manifest)


def verify_archives(repo: Path, engine: Path, archive: Path):
    manifest = read_json(archive / MANIFEST_NAME)
    require_inputs(manifest, repo, engine)
    actual = {name: file_record(archive / name) for name in ARCHIVES}
    if actual != manifest.get("archives"):
        raise CacheError("Editor cache archive integrity check failed")
    # Do not extract legacy manifests or incomplete game caches, even when
    # their engine archive contains a runnable editor executable.
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise CacheError("Editor cache manifest has no artifact inventory")
    for scope, names in REQUIRED_ARTIFACTS.items():
        if not isinstance(artifacts.get(scope), dict) or not all(name in artifacts[scope] for name in names):
            raise CacheError("Editor cache manifest omits required compiled project/engine artifacts")


def verify_installed(repo: Path, engine: Path, manifest_path: Path, receipt: Path | None = None):
    manifest = read_json(manifest_path)
    require_inputs(manifest, repo, engine)
    if artifact_inventory(repo, engine) != manifest.get("artifacts"):
        raise CacheError("Restored editor artifacts differ from the verified cache payload")
    if receipt is not None:
        write_json(receipt, manifest)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("snapshot", "seal", "verify-archives", "verify-installed"))
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--build-inputs", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args(argv)
    try:
        repo, engine = args.repo.resolve(), args.engine.resolve()
        if args.action == "snapshot":
            if not args.build_inputs: parser.error("snapshot requires --build-inputs")
            snapshot(repo, engine, args.build_inputs)
        elif args.action == "seal":
            if not args.archive or not args.build_inputs: parser.error("seal requires --archive and --build-inputs")
            seal(repo, engine, args.archive, args.build_inputs)
        elif args.action == "verify-archives":
            if not args.archive: parser.error("verify-archives requires --archive")
            verify_archives(repo, engine, args.archive)
        else:
            if not args.manifest: parser.error("verify-installed requires --manifest")
            verify_installed(repo, engine, args.manifest, args.receipt)
    except (CacheError, OSError) as error:
        print(f"Editor cache rejected: {error}", file=sys.stderr)
        return 1
    print(f"Editor cache {args.action}: verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
