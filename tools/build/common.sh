#!/usr/bin/env bash
# Shared native-host dispatch. Source from a wrapper after defining fail().
# QINGLAN_HOST_PLATFORM is an optional override for controlled toolchain tests.
qinglan_init_host() {
  case "${QINGLAN_HOST_PLATFORM:-$(uname -s)}" in
    Linux) QINGLAN_HOST=Linux ;;
    Darwin|Mac) QINGLAN_HOST=Mac ;;
    MINGW*|MSYS*|CYGWIN*|Win64) QINGLAN_HOST=Win64 ;;
    *) fail "unsupported build host: ${QINGLAN_HOST_PLATFORM:-$(uname -s)}" ;;
  esac
  QINGLAN_PLATFORM="${QINGLAN_PLATFORM:-$QINGLAN_HOST}"
  case "$QINGLAN_PLATFORM" in Linux|Mac|Win64) ;; *) fail "unsupported target platform: $QINGLAN_PLATFORM" ;; esac
}

qinglan_timestamp() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }

# Only the host selects engine tools; the target is passed to UBT/UAT.
qinglan_build_tool() {
  case "$QINGLAN_HOST" in
    Linux|Mac) printf '%s/Engine/Build/BatchFiles/%s/Build.sh\n' "$QINGLAN_ENGINE_ROOT" "$QINGLAN_HOST" ;;
    Win64) printf '%s/Engine/Build/BatchFiles/Build.bat\n' "$QINGLAN_ENGINE_ROOT" ;;
  esac
}
qinglan_uat_tool() {
  case "$QINGLAN_HOST" in
    Win64) printf '%s/Engine/Build/BatchFiles/RunUAT.bat\n' "$QINGLAN_ENGINE_ROOT" ;;
    *) printf '%s/Engine/Build/BatchFiles/RunUAT.sh\n' "$QINGLAN_ENGINE_ROOT" ;;
  esac
}
qinglan_editor_tool() {
  case "$QINGLAN_HOST" in
    Linux) printf '%s/Engine/Binaries/Linux/UnrealEditor-Cmd\n' "$QINGLAN_ENGINE_ROOT" ;;
    Mac)
      local candidate
      # Source and installed distributions differ in their commandlet layout.
      for candidate in \
        "$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" \
        "$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd.app/Contents/MacOS/UnrealEditor-Cmd" \
        "$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"; do
        if [ -f "$candidate" ]; then printf '%s\n' "$candidate"; return; fi
      done
      printf '%s/Engine/Binaries/Mac/UnrealEditor-Cmd\n' "$QINGLAN_ENGINE_ROOT"
      ;;
    Win64) printf '%s/Engine/Binaries/Win64/UnrealEditor-Cmd.exe\n' "$QINGLAN_ENGINE_ROOT" ;;
  esac
}
qinglan_native_path() {
  if [ "$QINGLAN_HOST" = Win64 ]; then
    command -v cygpath >/dev/null 2>&1 || fail "Windows wrappers require Git Bash/MSYS cygpath"
    cygpath -w "$1"
  else
    printf '%s\n' "$1"
  fi
}

qinglan_run() {
  if [ "$(id -u)" = 0 ] && id qdev >/dev/null 2>&1; then
    runuser -u qdev -- "$@"
  else
    "$@"
  fi
}

# .bat cannot be exec'd as a POSIX script. Quote every cmd.exe argument and
# disable delayed expansion. Reject cmd expansion characters that cannot be
# transported literally instead of evaluating caller-supplied shell text.
qinglan_run_engine() {
  local tool="$1"; shift
  if [ "$QINGLAN_HOST" = Win64 ] && [[ "$tool" = *.bat ]]; then
    local command_line='' arg
    for arg in "$(qinglan_native_path "$tool")" "$@"; do
      case "$arg" in *'"'*|*'%'*|*$'\r'*|*$'\n'*) fail 'Windows batch arguments cannot contain double quotes, percent signs, or newlines' ;; esac
      command_line="$command_line\"$arg\" "
    done
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' qinglan_run cmd.exe /d /v:off /s /c "\"${command_line% }\""
  else
    qinglan_run "$tool" "$@"
  fi
}

qinglan_find_client() {
  local root="$1" platform="$2"
  case "$platform" in
    Linux) find "$root" -type f -path '*/Binaries/Linux/QinglanAscension*' ! -name '*.debug' ! -name '*.sym' ! -name '*.so' ! -name '*.target' -print 2>/dev/null ;;
    Win64) find "$root" -type f -path '*/Binaries/Win64/QinglanAscension*.exe' -print 2>/dev/null ;;
    Mac) find "$root" -type f \( -path '*/QinglanAscension*.app/Contents/MacOS/QinglanAscension*' -o -path '*/Binaries/Mac/QinglanAscension*.app/Contents/MacOS/QinglanAscension*' \) -print 2>/dev/null ;;
  esac
}
