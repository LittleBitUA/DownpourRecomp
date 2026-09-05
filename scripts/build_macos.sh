#!/usr/bin/env bash
# Build the Downpour host shell for Apple Silicon.
#
# You supply your own legally-owned dump. This script never downloads, contains
# or distributes game code or game data - it only drives codegen and the build
# against files you already have.
#
# Usage:
#   REXSDK_DIR=/path/to/rexglue-sdk-dpour ./scripts/build_macos.sh [preset]
#
# preset defaults to macos-arm64-relwithdebinfo.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-macos-arm64-relwithdebinfo}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "this script targets macOS"
[[ "$(uname -m)" == "arm64" ]]  || die "this script targets Apple Silicon"

for tool in cmake ninja; do
  command -v "${tool}" >/dev/null 2>&1 || die "${tool} not found on PATH"
done

[[ -x /opt/homebrew/opt/llvm/bin/clang++ ]] ||
  die "Homebrew LLVM not found. Install it with: brew install llvm"

[[ -e /opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib ]] ||
  die "MoltenVK not found. Install it with: brew install molten-vk"

: "${REXSDK_DIR:=${root}/../rexglue-sdk-dpour}"
[[ -d "${REXSDK_DIR}" ]] ||
  die "REXSDK_DIR does not exist: ${REXSDK_DIR}
Point it at your checkout of the macos-arm64 branch of rexglue-sdk-dpour."

# The manifest reads the game files from assets/. Fail early and specifically -
# a missing dump otherwise surfaces as an opaque codegen error.
for required in default.xex default.xexp default.tu1.xex; do
  [[ -f "${root}/assets/${required}" ]] ||
    die "missing assets/${required}

Provide your own dump of Silent Hill: Downpour (title id 4B4E0823) with Title
Update 1 merged. See docs/MACOS_PORT.md. Game files must never be committed."
done

rexglue="${REXGLUE:-}"
if [[ -z "${rexglue}" ]]; then
  for candidate in \
      "${REXSDK_DIR}/out/macos-arm64/rexglue" \
      "${REXSDK_DIR}/out/build/macos-arm64/rexglue"; do
    [[ -x "${candidate}" ]] && { rexglue="${candidate}"; break; }
  done
fi
[[ -n "${rexglue}" ]] ||
  die "rexglue codegen tool not found. Build the SDK first, or set REXGLUE=/path/to/rexglue"

printf '==> codegen\n'
( cd "${root}" && "${rexglue}" codegen downpour_manifest.toml )

printf '==> configure (%s)\n' "${preset}"
cmake --preset "${preset}" -DREXSDK_DIR="${REXSDK_DIR}"

printf '==> build\n'
cmake --build --preset "${preset}" --target downpour --parallel "$(sysctl -n hw.ncpu)"

printf '\nBuilt: %s/out/build/%s/downpour\n' "${root}" "${preset}"
printf 'Copy your downpour.toml next to it, then run "Start Downpour.command".\n'
