#!/bin/bash
# 一键构建、部署、注册 AutoDeskflow（macOS）
set -e

CERT="07C2F6CAC8AE4C01B93F9C13C843CD8E473F941A"
APP_DST="/Applications/AutoDeskflow.app"
PLUGINS_DIR="$APP_DST/Contents/PlugIns"
XCODE_PROJ_DIR="/tmp/DeskflowPasteXcode"

echo "=== 停止现有进程 ==="
pkill -x AutoDeskflow || true
pkill -x autodeskflow-core || true
pkill -f DeskflowPaste || true
sleep 1

echo "=== 编译主 App (Release) ==="
cmake --build build-release --parallel "$(sysctl -n hw.ncpu)"

echo "=== 部署主 App ==="
# 先完整删除再复制，避免嵌套
rm -rf "$APP_DST"
cp -R build-release/bin/AutoDeskflow.app "$APP_DST"

echo "=== 构建 & 注册 Finder 扩展 ==="
# xcodebuild 直接构建到 PlugIns 目录，自动执行 RegisterExecutionPolicyException
# clean 步骤会报错（不在 DerivedData 内）但 CodeSign+Register 会正常执行
cd "$XCODE_PROJ_DIR"
xcodebuild \
  -project DeskflowPaste.xcodeproj \
  -scheme DeskflowPaste \
  -configuration Release \
  -derivedDataPath /tmp/DeskflowPasteBuild \
  DEVELOPMENT_TEAM=6H5RT96MM3 \
  CODE_SIGN_STYLE=Automatic \
  CONFIGURATION_BUILD_DIR="$PLUGINS_DIR" \
  SKIP_INSTALL=YES \
  clean build 2>&1 | grep -E "error:|RegisterExecution|CodeSign|BUILD SUCCEEDED|BUILD FAILED" || true

cd - > /dev/null

echo "=== 签名主 App ==="
codesign --force --sign "$CERT" "$APP_DST"

echo "=== 启动 ==="
open "$APP_DST"

sleep 3
echo "=== 扩展注册状态 ==="
pluginkit -m -p com.apple.FinderSync

echo "✅ 部署完成"
