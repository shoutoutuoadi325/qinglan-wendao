#!/usr/bin/env python3
"""Run a fresh native baseline; never change audit dispositions.

Report schema verified against UE 5.8 AutomationControllerManager.h
(FAutomatedTestPassResults / FAutomatedTestResult), NoExportTypes.h
(FAutomationExecutionEntry / FAutomationEvent), and JsonObjectConverter.cpp
(StandardizeCase). AutomationControllerManager.cpp writes index.json.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


CAMPAIGNS = {
    "Qinglan.Campaign.NewProfileMainJourney": {
        "manifest": "Campaign manifest: 30/30 scenes passed, 0 blocked",
        "opt_out": "Skipped heavy campaign journey; set QINGLAN_RUN_CAMPAIGN=1 to run it.",
    },
    "Qinglan.Campaign.NewProfileSideQuestJourney": {
        "manifest": "Side-quest manifest: 24/24 stages passed, 0 blocked",
        "opt_out": "Skipped heavy side-quest campaign; set QINGLAN_RUN_CAMPAIGN=1 to run it.",
    },
}


def utc_now():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def git_info(root):
    def git(*args):
        result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None
    status = git("status", "--porcelain=v1", "--untracked-files=normal")
    return {"root": str(root), "revision": git("rev-parse", "HEAD"),
            "branch": git("branch", "--show-current"),
            "dirty": bool(status) if status is not None else None,
            "status_porcelain": status}


def discover_tests(project):
    """Resolve this project's static SIMPLE registrations; reject unknown forms."""
    pattern = re.compile(r'IMPLEMENT_SIMPLE_AUTOMATION_TEST\s*\(\s*\w+\s*,\s*"(Qinglan\.[^"]+)"')
    tests, inputs = [], []
    for path in sorted((project / "Source").rglob("*.cpp")):
        source = path.read_text(encoding="utf-8")
        # Preserve strings while removing comments so commented registrations
        # and documentation examples cannot inflate the expected test set.
        source = re.sub(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*',
                        lambda match: match[0] if match[0].startswith('"') else "", source, flags=re.S)
        names = pattern.findall(source)
        if re.search(r'IMPLEMENT_(?!SIMPLE_)\w*AUTOMATION_TEST\s*\(', source):
            raise ValueError(f"Unsupported test registration in {path}; extend source discovery before accepting a partial suite")
        declaration_count = len(re.findall(r'IMPLEMENT_SIMPLE_AUTOMATION_TEST\s*\(', source))
        if declaration_count != len(names):
            raise ValueError(f"Nonliteral/non-Qinglan registration in {path}; expected names must be reviewed")
        tests.extend(names)
        if names:
            inputs.append({"path": str(path.relative_to(project)),
                           "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    if not tests or len(tests) != len(set(tests)):
        raise ValueError("Expected a nonempty set of unique Qinglan test registrations")
    if not set(CAMPAIGNS).issubset(tests):
        raise ValueError("Both campaign journeys must be present in source")
    return sorted(tests), inputs


def validate_report(report_file, expected, campaign_enabled):
    """Fail closed on absent, partial, duplicate, skipped, or failed results."""
    report = json.loads(report_file.read_text(encoding="utf-8-sig"))
    tests = report.get("tests")
    if not isinstance(tests, list) or not tests:
        raise ValueError("Automation report has no tests")
    actual = [test.get("fullTestPath") for test in tests]
    if len(actual) != len(set(actual)):
        raise ValueError("Automation report contains duplicate test paths")
    if set(actual) != set(expected):
        raise ValueError(f"Test selection mismatch: missing={sorted(set(expected) - set(actual))}; unexpected={sorted(set(actual) - set(expected))}")
    for field in ("failed", "notRun", "inProcess"):
        if report.get(field) != 0:
            raise ValueError(f"Automation summary {field} is not zero")
    if not all(type(report.get(field)) is int for field in ("succeeded", "succeededWithWarnings")):
        raise ValueError("Automation report is missing success counters")
    if report["succeeded"] + report["succeededWithWarnings"] != len(tests):
        raise ValueError("Automation success counters do not match named results")
    for test in tests:
        name = test["fullTestPath"]
        if test.get("state") != "Success" or test.get("errors") != 0:
            raise ValueError(f"{name} did not finish with Success and zero errors")
        entries = test.get("entries")
        if not isinstance(entries, list):
            raise ValueError(f"{name} is missing event entries")
        events = [entry["event"] for entry in entries]
        if any(event.get("type") == "Error" for event in events):
            raise ValueError(f"{name} contains error events")
        messages = [event["message"] for event in events]
        if name in CAMPAIGNS:
            if campaign_enabled:
                if any(re.search(r"\bskip(?:ped|ping)?\b", message, re.I) for message in messages):
                    raise ValueError(f"{name} was skipped despite campaign opt-in")
                if CAMPAIGNS[name]["manifest"] not in messages:
                    raise ValueError(f"{name} lacks the completed campaign manifest (success logs must not be pruned)")
            elif CAMPAIGNS[name]["opt_out"] not in messages:
                raise ValueError(f"{name} did not record the expected regular-suite opt-out")
        elif any(message.startswith("Skipped heavy ") or message.startswith("Skipped because ") for message in messages):
            raise ValueError(f"{name} was skipped")
    return {"named_successes": len(tests), "expected": sorted(expected),
            "campaign_enabled": campaign_enabled,
            "report_sha256": hashlib.sha256(report_file.read_bytes()).hexdigest()}


def native_path(path, host):
    if host == "Win64":
        return subprocess.check_output(["cygpath", "-w", str(path)], text=True).strip()
    return str(path)


def resolve_editor(editor, scripts, repo, env, host):
    """Select the host's layout after building a fresh source checkout."""
    if editor:
        path = Path(editor).resolve()
    else:
        result = subprocess.run(
            ["bash", "-c",
             'fail() { echo "$*" >&2; exit 1; }; source "$1"; qinglan_init_host; qinglan_editor_tool',
             "qinglan-acceptance-editor", str(scripts / "common.sh")],
            cwd=repo, env=env, capture_output=True, text=True, check=True)
        path = Path(result.stdout.strip()).resolve()
    if not path.is_file() or (host != "Win64" and not os.access(path, os.X_OK)):
        raise ValueError(f"Built editor executable is unavailable: {path}")
    return path


def package_inventory(archive, platform):
    """Hash this invocation's native client and cooked containers, not engine libs."""
    clients, containers = [], []
    for path in sorted(archive.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        parts = path.relative_to(archive).parts
        name = path.name
        client = False
        if platform == "Mac":
            client = (len(parts) >= 4 and parts[-3:-1] == ("Contents", "MacOS")
                      and parts[-4].startswith("QinglanAscension") and parts[-4].endswith(".app")
                      and name.startswith("QinglanAscension"))
        elif platform == "Linux":
            client = (any(parts[i:i + 2] == ("Binaries", "Linux") for i in range(len(parts) - 2))
                      and name.startswith("QinglanAscension") and path.suffix not in (".debug", ".sym", ".so", ".target"))
        elif platform == "Win64":
            client = (any(parts[i:i + 2] == ("Binaries", "Win64") for i in range(len(parts) - 2))
                      and name.startswith("QinglanAscension") and path.suffix.lower() == ".exe")
        if client:
            clients.append(path)
        elif path.suffix.lower() in (".pak", ".utoc", ".ucas"):
            containers.append(path)
    if not clients or not any(path.suffix.lower() == ".pak" for path in containers):
        raise ValueError("Package step produced no native client and pak in this invocation's archive")
    artifacts = []
    for path in clients + containers:
        if path.stat().st_size == 0:
            raise ValueError(f"Package artifact is empty: {path.relative_to(archive)}")
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        artifacts.append({"path": str(path.relative_to(archive)), "bytes": path.stat().st_size,
                          "kind": "native_client" if path in clients else "cooked_container",
                          "sha256": digest.hexdigest()})
    return {"archive": str(archive), "platform": platform, "recorded_utc": utc_now(), "artifacts": artifacts,
            "scope": "Artifact identity only; does not establish that the packaged client boots or renders."}


def run_logged(command, cwd, env, log_path, timeout):
    """Terminate the child tree on timeout/interruption, including the editor."""
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=os.name != "nt")
        try:
            return process.wait(timeout=timeout)
        except BaseException:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], stdout=log, stderr=log)
            else:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass
                except ProcessLookupError:
                    pass
                # The shell may exit before an editor child that ignores TERM.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.wait()
            raise


def run_baseline(args):
    repo, engine, project = (Path(value).resolve() for value in (args.repo, args.engine, args.project))
    run_dir = repo / ".local/acceptance" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run_dir.mkdir(parents=True, exist_ok=False)
    manifest = {"started_utc": utc_now(), "status": "running", "host": args.host,
                "platform": args.platform, "run_directory": str(run_dir),
                "repository_before": git_info(repo), "project_before": git_info(project),
                "engine": git_info(engine), "steps": []}
    manifest_path = run_dir / "manifest.json"
    write_json(manifest_path, manifest)
    print(f"Acceptance evidence: {run_dir}", flush=True)
    try:
        if args.platform != args.host:
            raise ValueError("Baseline acceptance requires a native target matching its editor host")
        manifest["engine_version"] = json.loads((engine / "Engine/Build/Build.version").read_text())
        expected, inputs = discover_tests(project)
        manifest["expected_tests"] = expected
        manifest["test_source_inputs"] = inputs
        env = os.environ.copy()
        env.update(QINGLAN_ENGINE_ROOT=str(engine), QINGLAN_PROJECT_DIR=str(project),
                   QINGLAN_HOST_PLATFORM=args.host, QINGLAN_PLATFORM=args.platform,
                   QINGLAN_DDC="(ProjectPak,EnginePak,Local)")
        scripts = repo / "tools/build"
        steps = [("01-build-editor", "build-game.sh", None),
                 ("02-regenerate-content", "regenerate-content.sh", None),
                 ("03-suite", None, "Qinglan"),
                 ("04-main-campaign", None, list(CAMPAIGNS)[0]),
                 ("05-side-campaign", None, list(CAMPAIGNS)[1]),
                 ("06-package", "package-game.sh", None)]
        for name, wrapper, selection in steps:
            step_env = env.copy()
            step_env["QINGLAN_RUN_CAMPAIGN"] = "1" if name in ("04-main-campaign", "05-side-campaign") else "0"
            step_env["QINGLAN_CONFIG"] = "Shipping" if name == "06-package" else "Development"
            step_env["QINGLAN_TARGET"] = "QinglanAscensionEditor"
            if name == "06-package":
                step_env["QINGLAN_ARCHIVE_DIR"] = str(run_dir / "package")
            step = {"name": name, "started_utc": utc_now(), "status": "running",
                    "log": str(run_dir / (name + ".log")),
                    "campaign_environment": step_env["QINGLAN_RUN_CAMPAIGN"],
                    "ddc_graph": step_env["QINGLAN_DDC"]}
            if selection:
                editor = resolve_editor(args.editor, scripts, repo, step_env, args.host)
                step["editor"] = str(editor)
                report_dir = run_dir / (name + "-report")
                report_dir.mkdir()
                step["report_export_path"] = str(report_dir)
                step["expected_tests"] = expected if selection == "Qinglan" else [selection]
                command = ["bash", "-c",
                           'fail() { echo "$*" >&2; exit 1; }; source "$1"; qinglan_init_host; qinglan_run_engine "$2" "${@:3}"',
                           "qinglan-acceptance", str(scripts / "common.sh"), str(editor),
                           native_path(project / "QinglanAscension.uproject", args.host),
                           "-ExecCmds=Automation RunTests " + selection,
                           "-TestExit=Automation Test Queue Empty", "-ReportExportPath=" + native_path(report_dir, args.host),
                           "-abslog=" + native_path(run_dir / (name + "-engine.log"), args.host),
                           "-ini:Engine:[/Script/AutomationController.AutomationControllerSettings]:bPruneLogsOnSuccess=False",
                           "-DDC=" + step_env["QINGLAN_DDC"],
                           "-unattended", "-nop4", "-nosplash", "-nullrhi", "-stdout", "-FullStdOutLogOutput"]
            else:
                command = ["bash", str(scripts / wrapper)]
            step["command"] = command
            manifest["steps"].append(step)
            write_json(manifest_path, manifest)
            print(f"Running {name}; log: {step['log']}", flush=True)
            started = time.monotonic()
            try:
                step["exit_code"] = run_logged(command, repo, step_env, Path(step["log"]), args.timeout_seconds)
                if step["exit_code"] != 0:
                    raise ValueError(f"{name} process exited {step['exit_code']}")
                if selection:
                    step["report_validation"] = validate_report(report_dir / "index.json", step["expected_tests"],
                                                                 step_env["QINGLAN_RUN_CAMPAIGN"] == "1")
                elif name == "06-package":
                    inventory_path = run_dir / "package-artifacts.json"
                    write_json(inventory_path, package_inventory(Path(step_env["QINGLAN_ARCHIVE_DIR"]), args.platform))
                    inventory_reference = {"path": str(inventory_path),
                                           "sha256": hashlib.sha256(inventory_path.read_bytes()).hexdigest()}
                    step["package_artifacts"] = inventory_reference
                    manifest["package_artifacts"] = inventory_reference
                step["status"] = "passed"
            except (Exception, KeyboardInterrupt) as error:
                step["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
                step["error"] = str(error) or "Interrupted by user"
                raise
            finally:
                step["ended_utc"] = utc_now()
                step["elapsed_seconds"] = round(time.monotonic() - started, 3)
                step["log_sha256"] = hashlib.sha256(Path(step["log"]).read_bytes()).hexdigest()
                write_json(manifest_path, manifest)
        manifest["status"] = "passed"
        return_code = 0
    except (Exception, KeyboardInterrupt) as error:
        manifest["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
        manifest["error"] = str(error) or "Interrupted by user"
        print(f"Acceptance {manifest['status']}: {manifest['error']}", file=sys.stderr)
        return_code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        manifest["ended_utc"] = utc_now()
        manifest["project_after"] = git_info(project)
        write_json(manifest_path, manifest)
    print(f"Acceptance {manifest['status']}; manifest: {manifest_path}", flush=True)
    return return_code


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("repo", "engine", "project", "host", "platform"):
        parser.add_argument("--" + argument, required=True)
    parser.add_argument("--editor", help="Optional editor override; otherwise resolve the built host editor before tests")
    parser.add_argument("--timeout-seconds", type=int, default=14400, help="Maximum seconds per step (default: 14400)")
    arguments = parser.parse_args()
    if arguments.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    raise SystemExit(run_baseline(arguments))
