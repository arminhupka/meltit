#!/usr/bin/env bash
# Buduje MELTIT (Standalone + VST3) na macOS (universal: arm64 + x86_64)
# i kopiuje gotowe pliki do dist/mac.
# Uzycie:  ./build-mac.sh [--skip-tests] [--clean]
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
ROOT="$PWD"
BUILD_DIR="$ROOT/build/macos"
OUT="$ROOT/dist/mac"

SKIP_TESTS=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --skip-tests) SKIP_TESTS=1 ;;
    --clean) CLEAN=1 ;;
    *) echo "Nieznany argument: $arg" >&2; exit 1 ;;
  esac
done

command -v cmake >/dev/null || { echo "Nie znaleziono cmake (wymagany 3.22+). brew install cmake" >&2; exit 1; }
command -v xcodebuild >/dev/null || { echo "Brak Xcode / Command Line Tools: xcode-select --install" >&2; exit 1; }

[ "$CLEAN" = 1 ] && rm -rf "$BUILD_DIR"

echo "==> Konfiguracja"
cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

JOBS="$(sysctl -n hw.ncpu)"
echo "==> Budowanie (Release)"
cmake --build "$BUILD_DIR" --config Release --parallel "$JOBS" \
  --target TapeMultiFx_Standalone TapeMultiFx_VST3 TapeMultiFxTransportTest TapeMultiFxEditorTest

if [ "$SKIP_TESTS" = 0 ]; then
  echo "==> Testy"
  ctest --test-dir "$BUILD_DIR" -C Release --output-on-failure
fi

ART="$BUILD_DIR/TapeMultiFx_artefacts/Release"
APP="$ART/Standalone/MELTIT.app"
VST3="$ART/VST3/Tape Multi-FX.vst3"
[ -d "$APP" ]  || { echo "Brak po buildzie: $APP" >&2; exit 1; }
[ -d "$VST3" ] || { echo "Brak po buildzie: $VST3" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/Standalone" "$OUT/VST3"
# ditto zachowuje atrybuty i symlinki bundli
ditto "$APP" "$OUT/Standalone/MELTIT.app"
ditto "$VST3" "$OUT/VST3/Tape Multi-FX.vst3"
cp Assets/Fonts/*.txt "$OUT/" 2>/dev/null || true

# Podpis ad-hoc (lokalne uruchamianie; do dystrybucji uzyj Developer ID + notaryzacji)
codesign --force --deep --sign - "$OUT/Standalone/MELTIT.app"
codesign --force --deep --sign - "$OUT/VST3/Tape Multi-FX.vst3"

echo
echo "Gotowe: $OUT"
find "$OUT" -maxdepth 2 | sed "s|^$ROOT/|  |"
