#!/usr/bin/env python3
"""
Continuously monitors a GitHub Actions run with resilient retry logic,
downloads artifacts upon successful completion, and runs validation.
"""
import sys
import time
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

def log(msg: str):
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    print(f"[{now}] {msg}", flush=True)

def run_cmd(cmd: list, retries=5, delay=5) -> tuple:
    for attempt in range(1, retries + 1):
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            if res.returncode == 0:
                return 0, res.stdout
            log(f"Warning: Command failed (attempt {attempt}/{retries}): {res.stderr.strip()[:200]}")
        except Exception as e:
            log(f"Warning: Exception executing command (attempt {attempt}/{retries}): {e}")
        time.sleep(delay)
    return 1, ""

def main():
    run_id = sys.argv[1] if len(sys.argv) > 1 else "39958964619"
    repo = sys.argv[2] if len(sys.argv) > 2 else "shoutoutuoadi325/qinglan-wendao"
    output_dir = Path("dist/store").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    log(f"Starting resilient watcher for Run ID: {run_id} (Repo: {repo})")
    log(f"Target download directory: {output_dir}")

    consecutive_errors = 0
    start_time = time.time()

    while True:
        code, out = run_cmd([
            "gh", "run", "view", run_id,
            "--repo", repo,
            "--json", "status,conclusion,jobs,databaseId,startedAt"
        ])

        if code != 0 or not out:
            consecutive_errors += 1
            log(f"Failed to query run status ({consecutive_errors} consecutive failures). Will retry in 30s...")
            if consecutive_errors > 20:
                log("ERROR: Exceeded 20 consecutive query failures. Exiting.")
                sys.exit(1)
            time.sleep(30)
            continue

        consecutive_errors = 0
        try:
            data = json.loads(out)
        except Exception as e:
            log(f"JSON decode error: {e}")
            time.sleep(30)
            continue

        status = data.get("status")
        conclusion = data.get("conclusion")
        jobs = data.get("jobs", [])

        # Find package job
        pkg_job = next((j for j in jobs if j.get("name") == "package"), None)
        active_step = "Unknown"
        if pkg_job:
            steps = pkg_job.get("steps", [])
            for step in steps:
                if step.get("status") == "in_progress":
                    active_step = f"Step {step.get('number')}: {step.get('name')}"
                    break
            if active_step == "Unknown" and pkg_job.get("status") == "in_progress":
                active_step = "Package job running"

        elapsed_mins = int((time.time() - start_time) / 60)
        log(f"Status: {status} | Conclusion: '{conclusion}' | Active: {active_step} | Elapsed: {elapsed_mins}m")

        if status == "completed":
            log(f"Run {run_id} completed with conclusion: {conclusion}")
            if conclusion == "success":
                log("Build succeeded! Downloading Microsoft Store artifacts...")
                # Download artifacts
                dl_code, dl_out = run_cmd([
                    "gh", "run", "download", run_id,
                    "--repo", repo,
                    "--dir", str(output_dir)
                ], retries=5, delay=10)
                
                log(f"Download result code: {dl_code}")
                if dl_code != 0:
                    log("ERROR: Failed to download artifacts after multiple retries.")
                    sys.exit(1)

                log("Artifacts downloaded. Running package validation...")
                verify_script = Path("tools/ci/verify_store_package.py").resolve()
                if verify_script.exists():
                    v_res = subprocess.run([sys.executable, str(verify_script), str(output_dir)], capture_output=True, text=True)
                    print(v_res.stdout)
                    if v_res.returncode != 0:
                        print(v_res.stderr)
                        log("ERROR: Package verification failed!")
                        sys.exit(1)
                    log("SUCCESS: All package verification checks passed!")
                else:
                    log("Warning: verify_store_package.py not found.")

                log("DELIVERY COMPLETE: Microsoft Store package ready in dist/store")
                sys.exit(0)
            else:
                log("Run completed with failure. Fetching failed logs...")
                _, fail_logs = run_cmd(["gh", "run", "view", run_id, "--repo", repo, "--log-failed"])
                print(fail_logs[-2000:])
                sys.exit(1)

        time.sleep(60)

if __name__ == "__main__":
    main()
