#!/usr/bin/env python3
"""Synthetic report/stub-process checks. These never launch Unreal Engine."""
import argparse
from contextlib import redirect_stderr, redirect_stdout
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch


REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("acceptance", REPO / "tools/build/run_acceptance.py")
acceptance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(acceptance)
MAIN = "Qinglan.Campaign.NewProfileMainJourney"
SIDE = "Qinglan.Campaign.NewProfileSideQuestJourney"


class ReportValidationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "index.json"
        self.fixture = json.loads((REPO / "tools/tests/fixtures/automation-main-success.json").read_text())

    def validate(self, fixture=None, expected=None, enabled=True):
        self.path.write_text(json.dumps(self.fixture if fixture is None else fixture))
        return acceptance.validate_report(self.path, expected or [MAIN], enabled)

    def test_success_requires_named_campaign_manifest(self):
        result = self.validate()
        self.assertEqual(result["named_successes"], 1)
        self.assertTrue(result["campaign_enabled"])

    def test_failed_skipped_unrun_and_incomplete_cases_are_rejected(self):
        for state in ("Fail", "Skipped", "NotRun", "InProcess"):
            with self.subTest(state=state):
                fixture = copy.deepcopy(self.fixture)
                fixture["tests"][0]["state"] = state
                with self.assertRaises(ValueError):
                    self.validate(fixture)

    def test_zero_tests_missing_expected_duplicate_and_summary_mismatch(self):
        for mutation in ("empty", "missing", "duplicate", "summary"):
            fixture = copy.deepcopy(self.fixture)
            if mutation == "empty":
                fixture["tests"] = []
            elif mutation == "missing":
                fixture["tests"][0]["fullTestPath"] = SIDE
            elif mutation == "duplicate":
                fixture["tests"] *= 2
            else:
                fixture["notRun"] = 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.validate(fixture)

    def test_success_with_opt_out_or_pruned_logs_is_not_campaign_evidence(self):
        for messages in ([], [acceptance.CAMPAIGNS[MAIN]["opt_out"]]):
            fixture = copy.deepcopy(self.fixture)
            fixture["tests"][0]["entries"] = [{"event": {"type": "Info", "message": message}} for message in messages]
            with self.subTest(messages=messages), self.assertRaises(ValueError):
                self.validate(fixture)

    def test_regular_suite_allows_only_explicit_campaign_opt_out(self):
        fixture = copy.deepcopy(self.fixture)
        fixture["tests"][0]["entries"][0]["event"]["message"] = acceptance.CAMPAIGNS[MAIN]["opt_out"]
        self.validate(fixture, enabled=False)
        fixture["tests"][0]["fullTestPath"] = "Qinglan.Combat.Example"
        with self.assertRaises(ValueError):
            self.validate(fixture, expected=["Qinglan.Combat.Example"], enabled=False)

    def test_error_events_cannot_be_hidden_by_success_state(self):
        fixture = copy.deepcopy(self.fixture)
        fixture["tests"][0]["entries"].append({"event": {"type": "Error", "message": "failed assertion"}})
        with self.assertRaises(ValueError):
            self.validate(fixture)


EDITOR_STUB = '''#!/usr/bin/env python3
import json, os, pathlib, sys
selection = next(arg.split("Automation RunTests ", 1)[1] for arg in sys.argv if arg.startswith("-ExecCmds="))
report = pathlib.Path(next(arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("-ReportExportPath=")))
enabled = os.environ["QINGLAN_RUN_CAMPAIGN"] == "1"
assert "-DDC=(ProjectPak,EnginePak,Local)" in sys.argv
assert "-ini:Engine:[/Script/AutomationController.AutomationControllerSettings]:bPruneLogsOnSuccess=False" in sys.argv
with open(os.environ["QINGLAN_TRACE"], "a") as log:
    log.write(json.dumps({"selection": selection, "campaign": enabled, "pid": os.getpid()}) + "\\n")
if os.environ.get("QINGLAN_STUB_MODE") == "no-report":
    sys.exit(0)
campaigns = {
"Qinglan.Campaign.NewProfileMainJourney": ("Campaign manifest: 30/30 scenes passed, 0 blocked", "Skipped heavy campaign journey; set QINGLAN_RUN_CAMPAIGN=1 to run it."),
"Qinglan.Campaign.NewProfileSideQuestJourney": ("Side-quest manifest: 24/24 stages passed, 0 blocked", "Skipped heavy side-quest campaign; set QINGLAN_RUN_CAMPAIGN=1 to run it.")}
names = ["Qinglan.Combat.Example", *campaigns] if selection == "Qinglan" else [selection]
tests = []
for name in names:
    messages = []
    if name in campaigns:
        messages = [campaigns[name][0 if enabled else 1]]
        if enabled and os.environ.get("QINGLAN_STUB_MODE") == "campaign-skip":
            messages = [campaigns[name][1]]
    tests.append({"fullTestPath": name, "state": "Success", "errors": 0, "entries": [{"event": {"type": "Info", "message": message}} for message in messages]})
report.joinpath("index.json").write_text(json.dumps({"tests": tests, "failed": 0, "notRun": 0, "inProcess": 0, "succeeded": len(tests), "succeededWithWarnings": 0}))
'''


class RunnerProcessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qinglan acceptance stub ")
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        scripts = self.repo / "tools/build"
        scripts.mkdir(parents=True)
        shutil.copy(REPO / "tools/build/common.sh", scripts / "common.sh")
        for order, name in enumerate(("build-game.sh", "regenerate-content.sh", "package-game.sh")):
            (scripts / name).write_text('''#!/usr/bin/env bash
printf '%s:%s\\n' 'WRAPPER' "$QINGLAN_RUN_CAMPAIGN" >> "$QINGLAN_TRACE"
[ "$QINGLAN_DDC" = '(ProjectPak,EnginePak,Local)' ] || exit 9
[ "${QINGLAN_STUB_FAIL:-}" != 'ORDER' ] || exit 7
'''.replace("WRAPPER", name).replace("ORDER", str(order)))
        with (scripts / "package-game.sh").open("a") as wrapper:
            wrapper.write('''
[ "${QINGLAN_STUB_MODE:-}" != 'empty-package' ] || exit 0
mkdir -p "$QINGLAN_ARCHIVE_DIR/QinglanAscension.app/Contents/MacOS" "$QINGLAN_ARCHIVE_DIR/Content/Paks"
printf 'synthetic-client' > "$QINGLAN_ARCHIVE_DIR/QinglanAscension.app/Contents/MacOS/QinglanAscension"
for extension in pak utoc ucas; do
  printf 'synthetic-container' > "$QINGLAN_ARCHIVE_DIR/Content/Paks/game.$extension"
done
''')
        project = self.repo / "game"
        (project / "Source/Test").mkdir(parents=True)
        (project / "QinglanAscension.uproject").write_text("{}")
        (project / "Source/Test/Tests.cpp").write_text("\n".join(
            f'IMPLEMENT_SIMPLE_AUTOMATION_TEST(F{i}, "{name}", Flags)' for i, name in enumerate(("Qinglan.Combat.Example", MAIN, SIDE))))
        engine = self.repo / "engine"
        (engine / "Engine/Build").mkdir(parents=True)
        (engine / "Engine/Build/Build.version").write_text('{"MajorVersion":5,"MinorVersion":8,"PatchVersion":3}')
        editor = engine / "fake-editor"
        editor.write_text(EDITOR_STUB)
        editor.chmod(0o755)
        self.args = argparse.Namespace(repo=str(self.repo), engine=str(engine), project=str(project),
                                       editor=str(editor), host="Mac", platform="Mac", timeout_seconds=10)
        self.trace = self.repo / "trace.log"

    def run_stub(self, **variables):
        with patch.dict(os.environ, {"QINGLAN_TRACE": str(self.trace), **variables}), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            rc = acceptance.run_baseline(self.args)
        manifests = sorted((self.repo / ".local/acceptance").glob("*/manifest.json"))
        return rc, json.loads(manifests[-1].read_text())

    def test_full_order_isolation_and_fresh_evidence(self):
        rc, manifest = self.run_stub(QINGLAN_RUN_CAMPAIGN="1")
        self.assertEqual(rc, 0)
        self.assertEqual(manifest["status"], "passed")
        self.assertEqual(len(manifest["steps"]), 6)
        trace = self.trace.read_text().splitlines()
        self.assertEqual(trace[:2], ["build-game.sh:0", "regenerate-content.sh:0"])
        editor_calls = [json.loads(line) for line in trace[2:5]]
        self.assertEqual([call["campaign"] for call in editor_calls], [False, True, True])
        self.assertEqual(len({call["pid"] for call in editor_calls}), 3)
        self.assertEqual(trace[5], "package-game.sh:0")
        reports = [step["report_export_path"] for step in manifest["steps"] if "report_export_path" in step]
        self.assertEqual(len(set(reports)), 3)
        self.assertTrue(all(step["log_sha256"] for step in manifest["steps"]))
        self.assertEqual(manifest["engine_version"]["PatchVersion"], 3)
        inventory_path = Path(manifest["package_artifacts"]["path"])
        inventory = json.loads(inventory_path.read_text())
        self.assertEqual(manifest["package_artifacts"]["sha256"], hashlib.sha256(inventory_path.read_bytes()).hexdigest())
        self.assertEqual(len(inventory["artifacts"]), 4)
        client = next(artifact for artifact in inventory["artifacts"] if artifact["kind"] == "native_client")
        self.assertEqual(client["sha256"], hashlib.sha256(b"synthetic-client").hexdigest())
        self.assertTrue(all(artifact["bytes"] > 0 for artifact in inventory["artifacts"]))
        second_rc, second = self.run_stub()
        self.assertEqual(second_rc, 0)
        self.assertNotEqual(manifest["run_directory"], second["run_directory"])

    def test_wrapper_failure_stops_before_tests_and_package(self):
        rc, manifest = self.run_stub(QINGLAN_STUB_FAIL="1")
        self.assertEqual(rc, 1)
        self.assertEqual(manifest["steps"][-1]["exit_code"], 7)
        self.assertEqual(len(manifest["steps"]), 2)

    def test_fresh_mac_build_resolves_editor_after_app_creation(self):
        built_editor = (Path(self.args.engine) /
                        "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor")
        self.assertFalse(built_editor.exists())
        self.args.editor = None
        with (self.repo / "tools/build/build-game.sh").open("a") as wrapper:
            wrapper.write('''
mkdir -p "$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS"
cp "$QINGLAN_ENGINE_ROOT/fake-editor" "$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
''')
        rc, manifest = self.run_stub()
        self.assertEqual(rc, 0)
        self.assertTrue(built_editor.exists())
        self.assertEqual([step["editor"] for step in manifest["steps"] if "editor" in step],
                         [str(built_editor.resolve())] * 3)

    def test_editor_exit_zero_without_report_cannot_pass(self):
        rc, manifest = self.run_stub(QINGLAN_STUB_MODE="no-report")
        self.assertEqual(rc, 1)
        self.assertEqual(manifest["steps"][-1]["name"], "03-suite")
        self.assertEqual(manifest["steps"][-1]["exit_code"], 0)
        self.assertEqual(manifest["steps"][-1]["status"], "failed")

    def test_missing_editor_or_invalid_override_cannot_pass(self):
        for override in (None, str(self.repo / "missing-editor")):
            with self.subTest(override=override):
                if override:
                    built_editor = (Path(self.args.engine) /
                                    "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor")
                    built_editor.parent.mkdir(parents=True)
                    shutil.copy(Path(self.args.engine) / "fake-editor", built_editor)
                self.args.editor = override
                rc, manifest = self.run_stub()
                self.assertEqual(rc, 1)
                self.assertIn("Built editor executable is unavailable", manifest["error"])
                self.assertEqual(len(manifest["steps"]), 2)

    def test_campaign_success_state_with_skip_cannot_reach_package(self):
        rc, manifest = self.run_stub(QINGLAN_STUB_MODE="campaign-skip")
        self.assertEqual(rc, 1)
        self.assertEqual(manifest["steps"][-1]["name"], "04-main-campaign")
        self.assertIn("skipped", manifest["error"])

    def test_real_source_discovery_includes_new_asset_path_test(self):
        names, inputs = acceptance.discover_tests(REPO / "game")
        self.assertIn("Qinglan.Content.RawAssetRootPrecedence", names)
        self.assertGreaterEqual(len(names), 71)
        self.assertTrue(any(item["path"].endswith("QinglanRawAssetsTests.cpp") for item in inputs))

    def test_interruption_preserves_final_manifest_and_step_status(self):
        def interrupt(command, cwd, env, log_path, timeout):
            log_path.write_text("Synthetic interruption; no engine launched.\n")
            raise KeyboardInterrupt()
        with patch.object(acceptance, "run_logged", side_effect=interrupt):
            rc, manifest = self.run_stub()
        self.assertEqual(rc, 130)
        self.assertEqual(manifest["status"], "interrupted")
        self.assertEqual(manifest["steps"][0]["status"], "interrupted")
        self.assertIn("ended_utc", manifest)
        self.assertIn("project_after", manifest)

    def test_package_zero_exit_without_current_artifacts_is_rejected(self):
        rc, manifest = self.run_stub(QINGLAN_STUB_MODE="empty-package")
        self.assertEqual(rc, 1)
        self.assertEqual(manifest["steps"][-1]["name"], "06-package")
        self.assertEqual(manifest["steps"][-1]["exit_code"], 0)
        self.assertNotIn("package_artifacts", manifest)
        self.assertIn("no native client and pak", manifest["error"])


if __name__ == "__main__":
    unittest.main()
