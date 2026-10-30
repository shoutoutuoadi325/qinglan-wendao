"""Exercise cache identity/integrity with real files; these do not build Unreal."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools/ci/editor_cache_manifest.py"
SPEC = importlib.util.spec_from_file_location("editor_cache_manifest", HELPER)
cache = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cache)


class EditorCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="editor cache's ")
        self.base = Path(self.temp.name)
        self.repo = self.base / "project checkout"
        self.engine = self.base / "prepared engine"
        self.archive = self.base / "cache archive"
        self.inputs = self.archive / "editor-build-inputs.json"
        self.manifest = self.archive / cache.MANIFEST_NAME
        self.receipt = self.repo / "game/Saved/CI/editor-cache-verified.json"
        for name in cache.PROJECT_FILES:
            self.put(self.repo / name, "fixture source " + name)
        for name in ("game/Source/Runtime.cpp", "game/Source/EditorTests.cpp", "game/Config/DefaultEngine.ini",
                     "game/Plugins/Local/Source/Local.cpp", "game/Plugins/Local/Local.uplugin"):
            self.put(self.repo / name, "version one")
        for name in ("Engine/Source/Runtime/Core.cpp", "Engine/Source/ThirdParty/Library/lib/Windows.lib",
                     "Engine/Source/ThirdParty/Library/bin/Generator.exe", "Engine/Source/Programs/UBT/UBT.csproj",
                     "Engine/Source/ThirdParty/Library/Binaries/Windows.lib",
                     "Engine/Source/Programs/UBT/Content/EmbeddedResource.txt",
                     "Engine/Build/Build.version", "Engine/Build/BatchFiles/Build.bat",
                     "Engine/Plugins/Test/Source/Test.cpp", "Engine/Plugins/Test/Test.uplugin"):
            self.put(self.engine / name, "version one")
        for scope, root in (("engine", self.engine), ("project", self.repo)):
            for name in cache.REQUIRED_ARTIFACTS[scope]:
                self.put(root / name, "compiled bytes " + name)
        self.put(self.engine / "Engine/Binaries/Win64/UnrealEditor.modules", json.dumps({"BuildId": "same-build"}))
        self.put(self.repo / "game/Binaries/Win64/UnrealEditor.modules", json.dumps({
            "BuildId": "same-build", "Modules": {
                name: f"UnrealEditor-{name}.dll" for name in ("QinglanAscension", "QinglanAscensionEditor")}}))
        self.put(self.engine / "Engine/Plugins/Test/Binaries/Win64/UnrealEditor-Test.dll", "compiled plugin")
        for name in cache.ARCHIVES:
            self.put(self.archive / name, "fixture archive bytes " + name)
        self.seal()

    def tearDown(self):
        self.temp.cleanup()

    def put(self, path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def seal(self):
        cache.snapshot(self.repo, self.engine, self.inputs)
        cache.seal(self.repo, self.engine, self.archive, self.inputs)

    def verify(self):
        cache.verify_archives(self.repo, self.engine, self.archive)
        cache.verify_installed(self.repo, self.engine, self.manifest, self.receipt)

    def test_exact_inputs_and_artifacts_can_be_reused_and_receipt_rechecked(self):
        self.verify()
        self.assertEqual(json.loads(self.manifest.read_text()), json.loads(self.receipt.read_text()))
        cache.verify_installed(self.repo, self.engine, self.receipt)
        # Neither generated outputs nor current content/data should force an
        # editor rebuild. Import/cook read current assets after this code gate.
        for name in ("game/Intermediate/Build/Module.obj", "game/Content/NewAsset.uasset", "localization/en-US.json"):
            self.put(self.repo / name, "new current content")
        self.put(self.engine / "Engine/Source/Programs/UBT/obj/generated.cs", "generated compiler output")
        self.verify()

    def test_edits_to_existing_runtime_tests_rules_plugins_and_engine_reject_cache(self):
        paths = [self.repo / name for name in (
            "game/Source/Runtime.cpp", "game/Source/EditorTests.cpp", "game/QinglanAscension.uproject",
            "game/Config/DefaultEngine.ini", "game/Plugins/Local/Source/Local.cpp",
            "game/Plugins/Local/Local.uplugin", "tools/build/versions.lock", "tools/ci/windows-build-editor.ps1")]
        paths += [self.engine / name for name in (
            "Engine/Source/Runtime/Core.cpp", "Engine/Source/ThirdParty/Library/lib/Windows.lib",
            "Engine/Source/ThirdParty/Library/bin/Generator.exe",
            "Engine/Source/ThirdParty/Library/Binaries/Windows.lib",
            "Engine/Source/Programs/UBT/Content/EmbeddedResource.txt",
            "Engine/Build/Build.version", "Engine/Build/BatchFiles/Build.bat", "Engine/Plugins/Test/Source/Test.cpp")]
        for path in paths:
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"\nchanged without renaming a test")
                with self.assertRaisesRegex(cache.CacheError, "build inputs differ"):
                    self.verify()
                path.write_bytes(original)
        self.verify()

    def test_new_and_deleted_sources_are_part_of_identity(self):
        added = self.repo / "game/Source/NewTest.cpp"
        self.put(added, "new registration")
        with self.assertRaisesRegex(cache.CacheError, "build inputs differ"):
            self.verify()
        added.unlink()
        existing = self.repo / "game/Source/Runtime.cpp"
        existing.unlink()
        with self.assertRaisesRegex(cache.CacheError, "build inputs differ"):
            self.verify()

    def test_inputs_changed_during_build_cannot_be_sealed(self):
        self.manifest.unlink()
        self.put(self.repo / "game/Source/Runtime.cpp", "edited while compiler ran")
        with self.assertRaisesRegex(cache.CacheError, "build inputs differ"):
            cache.seal(self.repo, self.engine, self.archive, self.inputs)
        self.assertFalse(self.manifest.exists())

    def test_legacy_unsupported_and_malformed_manifests_fail_closed(self):
        for content in (None, "not json", "[]", '{"version": 99}', '{"version": 1}'):
            with self.subTest(content=content):
                if self.manifest.exists():
                    self.manifest.unlink()
                if content is not None:
                    self.put(self.manifest, content)
                with self.assertRaises(cache.CacheError):
                    self.verify()
                self.assertFalse(self.receipt.exists())

    def test_archive_damage_and_missing_game_archive_reject_before_restore(self):
        archive = self.archive / "editor-game.7z"
        original = archive.read_bytes()
        archive.write_bytes(original + b"corruption")
        with self.assertRaisesRegex(cache.CacheError, "archive integrity"):
            cache.verify_archives(self.repo, self.engine, self.archive)
        archive.unlink()
        with self.assertRaises(cache.CacheError):
            cache.verify_archives(self.repo, self.engine, self.archive)
        self.assertFalse(self.receipt.exists())

    def test_changed_missing_or_extra_restored_modules_are_rejected(self):
        module = self.repo / "game/Binaries/Win64/UnrealEditor-QinglanAscensionEditor.dll"
        original = module.read_bytes()
        module.write_bytes(b"old test implementation")
        with self.assertRaisesRegex(cache.CacheError, "Restored editor artifacts differ"):
            self.verify()
        module.unlink()
        with self.assertRaisesRegex(cache.CacheError, "Missing compiled editor artifact"):
            self.verify()
        module.write_bytes(original)
        extra = module.with_name("UnrealEditor-Obsolete.dll")
        self.put(extra, "left over from an older cache")
        with self.assertRaisesRegex(cache.CacheError, "Restored editor artifacts differ"):
            self.verify()
        extra.unlink()
        self.verify()
        self.put(module, "modified after restore")
        with self.assertRaises(cache.CacheError):
            cache.verify_installed(self.repo, self.engine, self.receipt)

    def test_missing_editor_module_or_mixed_engine_build_cannot_be_sealed(self):
        module = self.repo / "game/Binaries/Win64/UnrealEditor.modules"
        original = module.read_bytes()
        payload = json.loads(original)
        payload["BuildId"] = "other-engine-build"
        self.put(module, json.dumps(payload))
        with self.assertRaisesRegex(cache.CacheError, "BuildId"):
            self.seal()
        payload = json.loads(original)
        del payload["Modules"]["QinglanAscensionEditor"]
        self.put(module, json.dumps(payload))
        with self.assertRaisesRegex(cache.CacheError, "does not identify"):
            self.seal()

    def test_cli_has_nonzero_status_on_stale_inputs_and_handles_literal_paths(self):
        command = [sys.executable, str(HELPER), "verify-installed", "--repo", str(self.repo),
                   "--engine", str(self.engine), "--manifest", str(self.manifest)]
        accepted = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.put(self.repo / "game/Source/EditorTests.cpp", "same test name, new assertion")
        rejected = subprocess.run(command, text=True, capture_output=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("rebuild the editor cache", rejected.stderr)

    def test_powershell_callers_gate_restore_and_cook_with_checked_verifier_status(self):
        restore = (ROOT / "tools/ci/windows-editor-cache.ps1").read_text()
        package = (ROOT / "tools/ci/windows-package.ps1").read_text()
        producer = (ROOT / "tools/ci/windows-build-editor.ps1").read_text()
        self.assertLess(restore.index("$verifier verify-archives"), restore.index("& 7z x"))
        self.assertIn("$verifier verify-installed", restore)
        self.assertIn("--receipt $receipt", restore)
        self.assertLess(package.index("editor_cache_manifest.py"), package.index("$textAssets ="))
        self.assertIn("if ($LASTEXITCODE) { throw 'Cached editor verification failed", package)
        self.assertLess(producer.index("editor_cache_manifest.py"), producer.index("& $build QinglanAscensionEditor"))

    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell is unavailable on this host")
    def test_powershell_entry_points_parse(self):
        command = "$bad = @(); Get-ChildItem tools/ci/*.ps1 | ForEach-Object { $t=$null; $e=$null; [void][System.Management.Automation.Language.Parser]::ParseFile($_.FullName,[ref]$t,[ref]$e); $bad += $e }; if ($bad.Count) { $bad | Out-String | Write-Error; exit 1 }"
        result = subprocess.run(["pwsh", "-NoProfile", "-Command", command], cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
