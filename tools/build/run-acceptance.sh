#!/usr/bin/env bash
# Native-host baseline: build, regenerate, suite, isolated campaigns, package.
# Each invocation gets fresh logs/reports under .local/acceptance/<UTC time>/.
# Success requires named test results, not just an editor exit code of zero.
set -o pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
fail() { echo "run-acceptance.sh: ERROR: $*" >&2; exit 1; }
source "$SCRIPT_DIR/local-env.sh" || exit 1
source "$SCRIPT_DIR/common.sh"
qinglan_init_host
export QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$REPO_ROOT/game}"
command -v python3 >/dev/null 2>&1 || fail "Python 3 is required"
exec python3 "$SCRIPT_DIR/run_acceptance.py" \
  --repo "$REPO_ROOT" --engine "$QINGLAN_ENGINE_ROOT" \
  --project "$QINGLAN_PROJECT_DIR" \
  --host "$QINGLAN_HOST" --platform "$QINGLAN_PLATFORM" "$@"
