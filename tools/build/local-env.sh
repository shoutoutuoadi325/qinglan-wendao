#!/usr/bin/env bash
# Source in bash/zsh: source tools/build/local-env.sh
# Scope is the current shell and its children. No global developer selection,
# shell profile, HOME, system toolchain, or launchd preference is changed.
if [ -n "${BASH_VERSION:-}" ]; then
  QINGLAN_ENV_SOURCE="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
  QINGLAN_ENV_SOURCE="${(%):-%N}"
else
  echo 'local-env.sh requires bash or zsh' >&2
  return 1 2>/dev/null || exit 1
fi
QINGLAN_ENV_REPO="$(cd "$(dirname "$QINGLAN_ENV_SOURCE")/../.." && pwd)" || return 1
export QINGLAN_LOCAL_ROOT="${QINGLAN_LOCAL_ROOT:-$QINGLAN_ENV_REPO/.local}"
export QINGLAN_ENGINE_ROOT="${QINGLAN_ENGINE_ROOT:-$QINGLAN_LOCAL_ROOT/toolchains/UnrealEngine}"
export QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$QINGLAN_ENV_REPO/game}"
export QINGLAN_ASSETS_RAW="${QINGLAN_ASSETS_RAW:-$QINGLAN_LOCAL_ROOT/assets-raw}"
export TMPDIR="$QINGLAN_LOCAL_ROOT/tmp/"
export TMP="$TMPDIR"
export TEMP="$TMPDIR"
export XDG_CACHE_HOME="$QINGLAN_LOCAL_ROOT/cache"
export CLANG_MODULE_CACHE_PATH="$QINGLAN_LOCAL_ROOT/cache/clang-modules"
export SWIFT_MODULECACHE_PATH="$QINGLAN_LOCAL_ROOT/cache/swift-modules"
export DOTNET_CLI_HOME="$QINGLAN_LOCAL_ROOT/cache/dotnet"
export NUGET_PACKAGES="$QINGLAN_LOCAL_ROOT/cache/nuget"
export UBA_ROOT="$QINGLAN_LOCAL_ROOT/cache/UnrealBuildAccelerator"
# The engine's command-line build tools do not need an HTTPS developer cert.
export DOTNET_GENERATE_ASPNET_CERTIFICATE=false
# These variables redirect cache data. Native UE macOS settings and Zen
# executable installation may still use ~/Library/Application Support/Epic;
# use a filesystem-only DDC graph when a fully local editor run is required.
# macOS/Linux translate Unreal's dash-separated environment variable names to
# underscores. Local DDC and Zen data remain under the removable workspace.
export UE_LocalDataCachePath="$QINGLAN_LOCAL_ROOT/cache/UnrealDerivedData"
export UE_SharedDataCachePath=None
export UE_ZenDataPath="$QINGLAN_LOCAL_ROOT/cache/UnrealZen"
# A file-only graph avoids installing the default Zen service in user Library.
export QINGLAN_DDC="${QINGLAN_DDC:-(ProjectPak,EnginePak,Local)}"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" "$CLANG_MODULE_CACHE_PATH" \
  "$SWIFT_MODULECACHE_PATH" "$DOTNET_CLI_HOME" "$NUGET_PACKAGES" \
  "$UE_LocalDataCachePath" "$UE_ZenDataPath" "$UBA_ROOT" || return 1
if [ -d "$QINGLAN_LOCAL_ROOT/toolchains/Xcode.app/Contents/Developer" ]; then
  export DEVELOPER_DIR="$QINGLAN_LOCAL_ROOT/toolchains/Xcode.app/Contents/Developer"
fi
unset QINGLAN_ENV_SOURCE QINGLAN_ENV_REPO
