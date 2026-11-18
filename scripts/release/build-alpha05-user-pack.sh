#!/usr/bin/env bash
# Build and package NAX5 Alpha 0.5 production launcher (Inno Setup exe in zip).
# Run from MSYS2 MINGW64.
set -euo pipefail

export PATH="/mingw64/bin:/usr/bin:/bin:${PATH}"
export MSYSTEM=MINGW64
export PKG_CONFIG_PATH="/mingw64/lib/pkgconfig"
export LDD_TIMEOUT=10

resolve_powershell() {
  local candidate
  for candidate in \
    "${POWERSHELL:-}" \
    /c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe \
    /c/Windows/SysWOW64/WindowsPowerShell/v1.0/powershell.exe \
    powershell.exe
  do
    [[ -n "$candidate" ]] || continue
    if command -v "$candidate" >/dev/null 2>&1; then
      printf '%s\n' "$candidate"
      return 0
    fi
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  echo "powershell.exe not found" >&2
  return 1
}

POWERSHELL_BIN="$(resolve_powershell)"

if ! /mingw64/bin/python3 -c 'import google.protobuf' >/dev/null 2>&1; then
  echo "Missing mingw-w64-x86_64-python-protobuf. Install with:" >&2
  echo "  pacman -S mingw-w64-x86_64-python-protobuf" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${NAX5_BUILD_DIR:-$ROOT/build-alpha05}"
OUT="${NAX5_RELEASE_DIR:-$ROOT/artifacts/alpha-0.5}"
if [[ -z "${GIT:-}" ]]; then
  GIT="/c/Program Files/Git/cmd/git.exe"
fi
STAGE="$OUT/portable/_stage"
USER_DIR="$OUT/portable/NAX5"
PORTABLE_ZIP="$OUT/NAX5-windows-portable.zip"
LEGACY_PORTABLE_ZIP="$OUT/NAX5-Alpha-0.5-Windows-x64.zip"
LAUNCHER_EXE="$OUT/NAX5.exe"
LAUNCHER_ZIP="$OUT/NAX5-windows.zip"
INSTALLER="$OUT/NAX5-windows-installer.exe"
EXE="$BUILD/gui/chiaki.exe"
if [[ ! -f "$ROOT/VERSION" ]]; then
  echo "Missing $ROOT/VERSION" >&2
  exit 1
fi
RELEASE_TAG="$(head -n1 "$ROOT/VERSION" | tr -d '\r\n')"

cd "$ROOT"

SHA="$("$GIT" -C "$ROOT" rev-parse HEAD)"
BRANCH="$("$GIT" -C "$ROOT" rev-parse --abbrev-ref HEAD)"
DESCRIBE="$("$GIT" -C "$ROOT" describe --tags --always --dirty)"
DIRTY_LINES="$("$GIT" -C "$ROOT" status --porcelain)"
DIRTY_COUNT=0
if [[ -n "$DIRTY_LINES" ]]; then
  DIRTY_COUNT="$(printf '%s\n' "$DIRTY_LINES" | sed -n '$=')"
fi

echo "HEAD=$SHA"
echo "DESCRIBE=$DESCRIBE"
echo "BRANCH=$BRANCH"
echo "DIRTY_COUNT=$DIRTY_COUNT"

if [[ "$DIRTY_COUNT" -ne 0 ]]; then
  echo "Refusing to build a release tag with a dirty worktree:" >&2
  printf '%s\n' "$DIRTY_LINES" >&2
  exit 1
fi

# Release tags already carry the reviewed assets. Regenerating them on every
# runner made the build depend on unrelated image-processing packages.
if [[ ! -s "$ROOT/gui/nax5.ico" || ! -s "$ROOT/gui/res/nax5-logo.png" ]]; then
  if [[ -f "$ROOT/assets/branding/nax5-brand-sheet.jpg" ]]; then
    /mingw64/bin/python3 "$ROOT/scripts/branding/import-nax5-logo.py"
  else
    /mingw64/bin/python3 "$ROOT/scripts/branding/generate-nax5-ico.py"
  fi
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCHIAKI_ENABLE_CLI=OFF \
  -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
  -DCHIAKI_ENABLE_TESTS=ON \
  -DNAX5_STREAM_REPLAY=OFF

echo "building and checking the standalone D3D11 lab..."
cmake --build "$BUILD" --config Release --target rcp-d3d11-lab
"$BUILD/gui/rcp-d3d11-lab.exe" --warp-smoke

echo "building chiaki..."
cmake --build "$BUILD" --config Release --target chiaki
echo "building unit tests..."
cmake --build "$BUILD" --config Release --target nax5-auth-unit nax5-session-unit nax5-connection-unit nax5-telemetry-unit rcp-input-unit rcp-audio-unit

"$BUILD/gui/nax5-auth-unit.exe"
"$BUILD/gui/nax5-session-unit.exe"
"$BUILD/gui/nax5-connection-unit.exe"
"$BUILD/gui/nax5-telemetry-unit.exe"
"$BUILD/gui/rcp-input-unit.exe"
"$BUILD/gui/rcp-audio-unit.exe"
echo "unit tests passed"

test -f "$EXE"

EXE_WIN="$(cygpath -w "$EXE")"
DESC="$(
  "$POWERSHELL_BIN" -NoProfile -Command "\$info=[System.Diagnostics.FileVersionInfo]::GetVersionInfo('${EXE_WIN}'); \$info.FileDescription" \
    | tr -d '\r'
)"
ORIG="$(
  "$POWERSHELL_BIN" -NoProfile -Command "\$info=[System.Diagnostics.FileVersionInfo]::GetVersionInfo('${EXE_WIN}'); \$info.OriginalFilename" \
    | tr -d '\r'
)"
if [[ "$DESC" != "NAX5 Remote Play Client" ]]; then
  echo "Unexpected FileDescription: [$DESC]" >&2
  exit 1
fi
if [[ "$ORIG" != "chiaki.exe" ]]; then
  echo "Unexpected OriginalFilename: [$ORIG]" >&2
  exit 1
fi

rm -rf "$STAGE" "$USER_DIR"
mkdir -p "$STAGE" "$OUT/portable"

bash "$ROOT/scripts/deploy-windows-msys2.sh" \
  "$STAGE" \
  "$EXE" \
  "$BUILD/third-party/cpp-steam-tools" \
  /mingw64 \
  gui/src/qml

printf '%s\n' '[Paths]' 'Prefix = .' > "$STAGE/qt.conf"

BUILT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
EXE_SIZE="$(wc -c < "$STAGE/chiaki.exe" | tr -d '[:space:]')"

cp -a "$STAGE" "$USER_DIR"
rm -f "$USER_DIR"/start-nax5-*.cmd
rm -f "$USER_DIR"/ALPHA0*.md

cp -f "$ROOT/docs/acceptance/ALPHA05-USER-TEST.md" "$USER_DIR/ALPHA05-USER-TEST.md"
cp -f "$ROOT/scripts/release/README-USER-alpha05.txt" "$USER_DIR/README-USER.txt"
cp -f "$ROOT/docs/RCP-QUICKSTART.md" "$USER_DIR/RCP-QUICKSTART.md"
cp -f "$ROOT/docs/RCP-D3D11-LAB.md" "$USER_DIR/RCP-D3D11-LAB.md"
cp -f "$BUILD/gui/rcp-d3d11-lab.exe" "$USER_DIR/rcp-d3d11-lab.exe"
lab_demo_args=(
  -nostdin -hide_banner -loglevel error -y
  -f lavfi -i testsrc2=size=1920x1080:rate=60 -frames:v 120
  -an -pix_fmt yuv420p -c:v libx265 -preset ultrafast -tune zerolatency
  -x265-params pools=2:frame-threads=2:log-level=error
  -color_primaries bt709 -colorspace bt709 -color_trc bt709 -color_range tv
  "$USER_DIR/rcp-d3d11-demo.mkv"
)
ffmpeg "${lab_demo_args[@]}"
cp -f "$ROOT/COPYING" "$ROOT/THIRD-PARTY-NOTICES.md" "$USER_DIR/"
cp -a "$ROOT/LICENSES" "$USER_DIR/"

{
  echo "NAX5 Alpha 0.5 portable Windows x64"
  echo "release: ${RELEASE_TAG}"
  echo "profile: product"
  echo "repo: nax5-client"
  echo "branch: ${BRANCH}"
  echo "commit: ${SHA}"
  echo "describe: ${DESCRIBE}"
  echo "built: ${BUILT}"
  echo "configuration: Release"
  echo "chiaki.exe bytes: ${EXE_SIZE}"
  echo "default API: https://cloudgta6.com"
  echo "launch: NAX5.exe (Inno Setup installer) -> chiaki.exe"
  echo "launcher: ${LAUNCHER_EXE##*/}"
  echo "installer: ${INSTALLER##*/}"
  echo "unit tests: nax5-auth-unit, nax5-session-unit, nax5-connection-unit, nax5-telemetry-unit, rcp-input-unit, rcp-audio-unit passed"
  echo "clean launch smoke skipped: ${NAX5_SKIP_LAUNCH_SMOKE:-0}"
  echo "features: login, reserve, auto Remote Play, process log, client-reports, session hardening, first-frame replay tracking"
} > "$USER_DIR/BUILD-INFO.txt"

if find "$USER_DIR" -iname '*.cmd' | grep -q .; then
  echo "User pack must not contain .cmd launchers" >&2
  exit 1
fi

if ! compgen -G "$USER_DIR/avutil-*.dll" > /dev/null; then
  echo "Portable bundle is missing avutil-*.dll" >&2
  exit 1
fi
if ! compgen -G "$USER_DIR/avcodec-*.dll" > /dev/null; then
  echo "Portable bundle is missing avcodec-*.dll" >&2
  exit 1
fi

cd "$OUT/portable"
rm -f "$PORTABLE_ZIP" "$LEGACY_PORTABLE_ZIP"
zip -r -9 "$PORTABLE_ZIP" NAX5
unzip -tq "$PORTABLE_ZIP"
cp -f "$PORTABLE_ZIP" "$LEGACY_PORTABLE_ZIP"

SHA256="$(sha256sum "$PORTABLE_ZIP" | awk '{print $1}')"
echo "PORTABLE_ZIP=$PORTABLE_ZIP"
echo "PORTABLE_SHA256=$SHA256"
ls -l "$PORTABLE_ZIP"

bash "$ROOT/scripts/release/package-inno-launcher.sh" "$USER_DIR" "$OUT"

echo "DONE"
