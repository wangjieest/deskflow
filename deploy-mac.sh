#!/bin/bash
# 一键部署、签名、注册 AutoDeskflow（macOS）
#
# 用法：
#   ./deploy-mac.sh                 # 从源码构建 → 装到 /Applications → 本机签名 → 注册扩展
#   ./deploy-mac.sh --from-zip      # 从 dist/AutoDeskFlow-mac-<arch>.zip 部署（不编译）
#   ./deploy-mac.sh --from-zip <路径.zip>
#   ./deploy-mac.sh --from-app <路径.app>
#
# --from-zip / --from-app 是给绿色版准备的：dist-mac.sh 出的包只带 ad-hoc 签名
# （不含任何人的个人证书），装机时在这里用本机证书重签。重签之后 Finder 扩展
# 才能被 pluginkit 注册，TCC 权限（辅助功能/输入监控）也绑到本机身份上。
set -e

MODE="build"
SOURCE_PATH=""
case "${1:-}" in
  --from-zip) MODE="zip"; SOURCE_PATH="${2:-}" ;;
  --from-app) MODE="app"; SOURCE_PATH="${2:-}"
              [ -n "$SOURCE_PATH" ] || { echo "--from-app 需要指定 .app 路径" >&2; exit 1; } ;;
  "")         ;;
  *)          echo "未知参数: $1" >&2; exit 1 ;;
esac

# 自动探测本机代码签名身份。
#
# 两个坑：
#  1. 不能写死证书 hash —— 证书每年轮换（脚本里原先那个 07C2F6CAC8AE… 已失效）。
#  2. 身份名括号里的值不是 TeamID，是开发者个人 ID
#     （"Apple Development: jeffry wang (X9N5Y2HD59)" 的 team 其实是 6H5RT96MM3）。
#     真 TeamID 得从证书 subject 的 OU 字段取。
#
# 按 team 挑而不是"取第一个"：钥匙串里常有同事的 Apple Development 证书，
# 签错人不只是署名问题 —— TCC 权限（辅助功能/输入监控）绑签名身份，
# 换了 team 等于换了个 app，得去系统设置重新授权。
#
# 覆盖方式：
#   DEVELOPMENT_TEAM=XXXXXXXXXX ./deploy-mac.sh    # 换 team
#   CERT=<证书 hash 或身份名> ./deploy-mac.sh      # 直接指定
PREFERRED_TEAM="${DEVELOPMENT_TEAM:-6H5RT96MM3}"

# 每行输出 "hash<TAB>身份名<TAB>TeamID"
codesign_identities() {
  security find-identity -v -p codesigning 2>/dev/null \
    | sed -nE 's/^ *[0-9]+\) *([0-9A-F]+) *"(.*)"$/\1	\2/p' | sort -u \
    | while IFS=$'\t' read -r hash name; do
        team=$(security find-certificate -c "$name" -p 2>/dev/null \
               | openssl x509 -noout -subject 2>/dev/null \
               | tr ',/' '\n\n' | sed -nE 's/.*OU *= *([A-Z0-9]+).*/\1/p' | head -1)
        printf '%s\t%s\t%s\n' "$hash" "$name" "$team"
      done
}

# 成功时输出 "hash<TAB>身份名<TAB>TeamID"
detect_codesign_identity() {
  local all line pattern
  all=$(codesign_identities)
  for pattern in "Developer ID Application:" "Apple Development:"; do
    line=$(printf '%s\n' "$all" | grep -F "$pattern" \
           | awk -F'\t' -v t="$PREFERRED_TEAM" '$3 == t { print; exit }')
    [ -n "$line" ] && { printf '%s\n' "$line"; return 0; }
  done
  for pattern in "Developer ID Application:" "Apple Development:"; do
    line=$(printf '%s\n' "$all" | grep -F "$pattern" | head -1)
    if [ -n "$line" ]; then
      echo "! 钥匙串里没有 team $PREFERRED_TEAM 的证书，退用 $(printf '%s' "$line" | cut -f2)" >&2
      echo "! TCC 权限（辅助功能/输入监控）需要重新授权" >&2
      printf '%s\n' "$line"; return 0
    fi
  done
  return 1
}

IDENTITY="$(detect_codesign_identity)" || {
  echo "找不到可用的代码签名身份，请检查钥匙串（security find-identity -v -p codesigning）" >&2
  exit 1
}
CERT_NAME="$(printf '%s' "$IDENTITY" | cut -f2)"
# 传给 codesign / CMake 一律用 hash：上游 add_custom_command 里的
# codesign --sign ${APPLE_CODESIGN_DEV} 没加引号，身份名里的空格和括号会被拆散
CERT="${CERT:-$(printf '%s' "$IDENTITY" | cut -f1)}"
TEAM_ID="${TEAM_ID:-$(printf '%s' "$IDENTITY" | cut -f3)}"
echo "签名身份: $CERT_NAME  (team $TEAM_ID)"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO"
APP_DST="/Applications/AutoDeskflow.app"
PLUGINS_DIR="$APP_DST/Contents/PlugIns"
XCODE_PROJ_DIR="$REPO/src/apps/deskflow-paste-ext"

echo "=== 停止现有进程 ==="
pkill -x AutoDeskflow || true
pkill -x autodeskflow-core || true
pkill -f DeskflowPaste || true
sleep 1

if [ "$MODE" = "build" ]; then
  echo "=== 编译主 App (Release) ==="
  # Re-run cmake if needed (e.g. new dependencies)
  cmake -S. -Bbuild-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DQt6_DIR=/opt/homebrew/opt/qt/lib/cmake/Qt6 \
    -DAPPLE_CODESIGN_DEV="$CERT" \
    -DBUILD_TESTS=OFF \
    > /dev/null
  cmake --build build-release --parallel "$(sysctl -n hw.ncpu)"
  SRC_APP="build-release/bin/AutoDeskflow.app"
elif [ "$MODE" = "zip" ]; then
  ZIP="${SOURCE_PATH:-$REPO/dist/AutoDeskFlow-mac-$(uname -m).zip}"
  [ -f "$ZIP" ] || { echo "找不到 $ZIP（先跑 ./dist-mac.sh）" >&2; exit 1; }
  echo "=== 解包 $ZIP ==="
  UNPACK_DIR="$(mktemp -d)"
  trap 'rm -rf "$UNPACK_DIR"' EXIT
  unzip -q "$ZIP" -d "$UNPACK_DIR"
  SRC_APP="$UNPACK_DIR/AutoDeskflow.app"
else
  SRC_APP="$SOURCE_PATH"
  [ -d "$SRC_APP" ] || { echo "找不到 $SRC_APP" >&2; exit 1; }
fi

echo "=== 部署主 App ==="
# 先完整删除再复制，避免嵌套
rm -rf "$APP_DST"
ditto "$SRC_APP" "$APP_DST"
# 下载来的 zip 带 quarantine，不清掉 Gatekeeper 会拦
xattr -dr com.apple.quarantine "$APP_DST" 2>/dev/null || true

if [ "$MODE" = "build" ]; then
  echo "=== 构建 & 注册 Finder 扩展 ==="
  # xcodebuild 直接构建到 PlugIns 目录，自动执行 RegisterExecutionPolicyException
  # clean 步骤会报错（不在 DerivedData 内）但 CodeSign+Register 会正常执行
  cd "$XCODE_PROJ_DIR"
  xcodebuild \
    -project DeskflowPaste.xcodeproj \
    -scheme DeskflowPaste \
    -configuration Release \
    -derivedDataPath /tmp/DeskflowPasteBuild \
    DEVELOPMENT_TEAM="$TEAM_ID" \
    CODE_SIGN_STYLE=Automatic \
    CONFIGURATION_BUILD_DIR="$PLUGINS_DIR" \
    SKIP_INSTALL=YES \
    clean build 2>&1 | grep -E "error:|RegisterExecution|CodeSign|BUILD SUCCEEDED|BUILD FAILED" || true
  cd - > /dev/null
fi

echo "=== 用本机证书重签（由内到外）==="
# 绿色版进来时是 ad-hoc 签名，整包都得换成本机身份，只签顶层不够
codesign --force --deep --sign "$CERT" --timestamp=none "$APP_DST"
# appex 必须带 entitlements 单独重签，否则 pluginkit 不认
APPEX="$PLUGINS_DIR/DeskflowPaste.appex"
if [ -d "$APPEX" ]; then
  codesign --force --sign "$CERT" --timestamp=none \
    --entitlements "$XCODE_PROJ_DIR/DeskflowPaste.entitlements" "$APPEX"
fi
# 重签顶层，重新封印 PlugIns
codesign --force --sign "$CERT" --timestamp=none "$APP_DST"
codesign --verify --deep --strict "$APP_DST" && echo "✓ 签名校验通过"

echo "=== 启动 ==="
open "$APP_DST"

sleep 3
echo "=== 扩展注册状态 ==="
pluginkit -m -p com.apple.FinderSync

echo "✅ 部署完成"
