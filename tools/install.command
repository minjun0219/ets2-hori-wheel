#!/bin/bash
# 더블클릭 설치: 이 파일 옆의 hori_apex.so 를 현재 사용자의 ETS2 plugins 폴더에 복사한다.
# (Finder 에서 .command 를 더블클릭하면 터미널이 열리며 실행된다)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/hori_apex.so"
STEAM="$HOME/Library/Application Support/Steam"
APP_REL="steamapps/common/Euro Truck Simulator 2/Euro Truck Simulator 2.app"

pause() { echo; read -n 1 -s -r -p "아무 키나 누르면 창을 닫습니다…"; echo; }
fail() { echo "❌ $1"; pause; exit 1; }

echo "HORI Racing Wheel Apex 플러그인 설치"
echo "────────────────────────────────────"
[ -f "$SRC" ] || fail "hori_apex.so 를 찾지 못했습니다: $SRC"

# 기본 라이브러리 + Steam 라이브러리 폴더(libraryfolders.vdf)에서 ETS2 를 찾는다
APP=""
for lib in "$STEAM" $(grep -o '"path"[[:space:]]*"[^"]*"' "$STEAM/steamapps/libraryfolders.vdf" 2>/dev/null | sed 's/.*"\(.*\)"$/\1/' | tr '\n' ' '); do
  [ -d "$lib/$APP_REL" ] && { APP="$lib/$APP_REL"; break; }
done
[ -n "$APP" ] || fail "Euro Truck Simulator 2 설치를 찾지 못했습니다. Steam 에서 먼저 설치해 주세요."

if pgrep -xq eurotrucks2; then
  echo "⚠️  게임이 실행 중입니다. 설치 뒤 게임을 완전히 껐다가 다시 켜야 새 플러그인이 적용됩니다."
fi

DEST="$APP/Contents/MacOS/plugins"
mkdir -p "$DEST" || fail "plugins 폴더를 만들 수 없습니다: $DEST"
cp -f "$SRC" "$DEST/hori_apex.so" || fail "복사에 실패했습니다."
xattr -c "$DEST/hori_apex.so" 2>/dev/null

if cmp -s "$SRC" "$DEST/hori_apex.so"; then
  echo "✅ 설치했습니다"
  echo "   → $DEST/hori_apex.so"
  echo "   버전: $(stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$SRC") · $(shasum -a 256 "$SRC" | cut -c1-12)"
  echo
  echo "게임을 켜고 옵션 → 조작에서 \"HORI Racing Wheel Apex SDK\" 를 고르세요."
else
  fail "복사한 파일이 원본과 다릅니다."
fi
pause
