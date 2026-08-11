#!/bin/bash
# 一键构建并打包 AutoDeskflow macOS 绿色版（自包含 .app.zip）
#
# 产物：dist/AutoDeskFlow-mac-$(uname -m).zip，解压即用，不依赖 Homebrew Qt
#
# 用法：
#   ./dist-mac.sh              # 构建 + 打包 + 校验
#   ./dist-mac.sh --no-build   # 跳过构建，直接用 build-release/bin 里现成的 .app
#   ./dist-mac.sh --inspect    # 只审查已打好的 zip，不重新打包
#
# 签名：默认 ad-hoc，发布包里不带任何人的个人证书。
#
# Apple Silicon 上「完全不签名」不是一个选项 —— arm64 二进制没有签名会被内核
# 直接 SIGKILL（实测 rc=137）。ad-hoc 是最低档，不需要证书、纯本地生成。
#
# 代价是 Finder 扩展（DeskflowPaste.appex）注册不上：pluginkit 要求 appex 有带
# entitlements 的真实签名 + TeamID。装机时跑 `./deploy-mac.sh --from-zip`
# 用本机证书重签即可，顺带把 TCC 权限绑到本机身份上。
#
# 要在打包阶段就用真证书：
#   CERT=auto ./dist-mac.sh                     # 自动探测本机证书
#   CERT=<证书 hash 或身份名> ./dist-mac.sh     # 指定

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO/build-release"
MACDEPLOYQT="${MACDEPLOYQT:-/opt/homebrew/opt/qt/bin/macdeployqt}"
QT6_DIR="${QT6_DIR:-/opt/homebrew/opt/qt/lib/cmake/Qt6}"
QTSVG_FRAMEWORK="${QTSVG_FRAMEWORK:-/opt/homebrew/opt/qtsvg/lib/QtSvg.framework}"

APP_NAME="AutoDeskflow.app"
SRC_APP="$BUILD_DIR/bin/$APP_NAME"
STAGE="$BUILD_DIR/portable"
APP="$STAGE/$APP_NAME"
OUT_DIR="$REPO/dist"
OUT_ZIP="$OUT_DIR/AutoDeskFlow-mac-$(uname -m).zip"
ENTITLEMENTS="$REPO/src/apps/deskflow-paste-ext/DeskflowPaste.entitlements"

step() { printf '\n\033[36m>>> %s\033[0m\n' "$1"; }
ok()   { printf '\033[32m✓ %s\033[0m\n' "$1"; }
warn() { printf '\033[33m! %s\033[0m\n' "$1"; }

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
#   DEVELOPMENT_TEAM=XXXXXXXXXX ./dist-mac.sh    # 换 team
#   CERT=<证书 hash 或身份名> ./dist-mac.sh      # 直接指定
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

# 列出 bundle 内所有 Mach-O 文件
list_macho() {
  find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q 'Mach-O'; then printf '%s\n' "$f"; fi
  done
}

# 手工部署 macdeployqt 漏掉的框架（Homebrew 把 Qt 拆成多个 formula，-libpath 也搜不到）
# $1 = 源 .framework 路径, $2 = 框架名
deploy_extra_framework() {
  local src="$1" name="$2"
  local dst="$APP/Contents/Frameworks/$name.framework"
  local bin="$dst/Versions/A/$name"
  [ -d "$src" ] || { warn "找不到 $src，跳过 $name"; return 0; }
  rm -rf "$dst"
  ditto "$src" "$dst"
  chmod -R u+w "$dst"
  # Headers / .prl / 旧签名都是构建期产物，绿色版不需要
  rm -rf "$dst/Headers" "$dst/Versions/A/Headers" "$dst/Versions/A/_CodeSignature"
  find "$dst" -name '*.prl' -delete
  install_name_tool -id "@executable_path/../Frameworks/$name.framework/Versions/A/$name" "$bin"
  # 把绝对路径依赖改写成 bundle 内相对路径
  otool -L "$bin" | tail -n +2 | awk '{print $1}' \
    | grep -E '^/opt/homebrew|^/usr/local/' | while read -r dep; do
    local db; db=$(basename "$dep")
    if [ -e "$APP/Contents/Frameworks/$db.framework/Versions/A/$db" ]; then
      install_name_tool -change "$dep" "@executable_path/../Frameworks/$db.framework/Versions/A/$db" "$bin"
    elif [ -e "$APP/Contents/Frameworks/$db" ]; then
      install_name_tool -change "$dep" "@loader_path/../../../$db" "$bin"
    else
      warn "  $name 依赖 $db 未内嵌"
    fi
  done
  ok "  已部署 $name.framework"
}

# 迭代删除没有任何 Mach-O 引用的 Frameworks 条目
prune_unused_frameworks() {
  local pass=1
  while :; do
    local refs removed=0
    refs=$(list_macho | while IFS= read -r f; do
      local self; self=$(basename "$f")
      otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | while read -r dep; do
        local b; b=$(basename "$dep")
        [ "$b" = "$self" ] || printf '%s\n' "$b"
      done
    done | sort -u)

    for entry in "$APP/Contents/Frameworks"/*; do
      [ -e "$entry" ] || continue
      local base name; base=$(basename "$entry")
      case "$base" in
        *.framework) name="${base%.framework}" ;;
        *)           name="$base" ;;
      esac
      if ! printf '%s\n' "$refs" | grep -qx "$name"; then
        echo "  剪除未引用: $base ($(du -sh "$entry" | cut -f1))"
        rm -rf "$entry"
        removed=1
      fi
    done
    [ "$removed" -eq 0 ] && break
    pass=$((pass + 1))
    [ "$pass" -gt 10 ] && break
  done
}

inspect_zip() {
  step "包内容审查：$OUT_ZIP"
  [ -f "$OUT_ZIP" ] || { warn "zip 不存在"; return 1; }
  echo "大小: $(du -h "$OUT_ZIP" | cut -f1)"
  echo
  echo "--- 顶层条目（应只有 $APP_NAME）---"
  unzip -Z1 "$OUT_ZIP" | awk -F/ '{print $1}' | sort -u
  echo
  echo "--- 多余数据检查 ---"
  local bad
  bad=$(unzip -Z1 "$OUT_ZIP" \
        | grep -iE '\.DS_Store$|\.dSYM/|__MACOSX|/\._|\.prl$|Headers/|\.o$|\.a$|CMakeFiles|\.git/|\.log$' || true)
  if [ -n "$bad" ]; then
    warn "发现可疑条目："; echo "$bad" | head -30
  else
    ok "无 __MACOSX / ._AppleDouble / .DS_Store / dSYM / Headers / .prl / 构建中间产物"
  fi
  echo
  echo "--- Contents 顶层 ---"
  unzip -Z1 "$OUT_ZIP" | grep -oE "^$APP_NAME/Contents/[^/]+" | sort -u
  echo
  echo "--- 可执行文件 ---"
  unzip -Z1 "$OUT_ZIP" | grep -E "^$APP_NAME/Contents/MacOS/[^/]+$" | sort -u
  echo
  echo "--- PlugIns ---"
  unzip -Z1 "$OUT_ZIP" | grep -oE "Contents/PlugIns/[^/]+" | sort -u
}

if [ "${1:-}" = "--inspect" ]; then
  inspect_zip
  exit 0
fi

# 默认 ad-hoc；CERT=auto 时才去钥匙串里找真证书
CERT="${CERT:--}"
if [ "$CERT" = "auto" ]; then
  if IDENTITY="$(detect_codesign_identity)"; then
    # 传给 codesign / CMake 一律用 hash：上游 add_custom_command 里的
    # codesign --sign ${APPLE_CODESIGN_DEV} 没加引号，身份名里的空格和括号会被拆散
    CERT="$(printf '%s' "$IDENTITY" | cut -f1)"
    echo "签名身份: $(printf '%s' "$IDENTITY" | cut -f2)  (team $(printf '%s' "$IDENTITY" | cut -f3))"
  else
    CERT="-"
    warn "钥匙串里没有可用证书，退回 ad-hoc"
  fi
fi
if [ "$CERT" = "-" ]; then
  echo "签名身份: ad-hoc（发布包不带个人证书；装机时用 ./deploy-mac.sh --from-zip 重签）"
fi

if [ "${1:-}" != "--no-build" ]; then
  step "0/8 构建 Release"
  cmake -S "$REPO" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DQt6_DIR="$QT6_DIR" \
    -DAPPLE_CODESIGN_DEV="$CERT" \
    -DBUILD_TESTS=OFF > /dev/null
  cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)"
  ok "构建完成"
fi

[ -d "$SRC_APP" ] || { echo "找不到 $SRC_APP，请先构建（去掉 --no-build）" >&2; exit 1; }

step "1/8 准备 staging 目录"
rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$SRC_APP" "$APP"
ok "已复制 $APP_NAME"

step "2/8 暂存 Finder 扩展（避免 macdeployqt 误处理 .appex）"
APPEX_SRC="$APP/Contents/PlugIns/DeskflowPaste.appex"
APPEX_TMP="$STAGE/_appex/DeskflowPaste.appex"
if [ -d "$APPEX_SRC" ]; then
  mkdir -p "$STAGE/_appex"
  mv "$APPEX_SRC" "$APPEX_TMP"
  ok "已移出 DeskflowPaste.appex"
else
  warn "未找到 DeskflowPaste.appex（Finder 扩展将不包含在绿色版中）"
fi

step "3/8 macdeployqt 自包含化"
# -executable 必须显式指定：Contents/MacOS/autodeskflow-core 是第二个可执行文件，
# macdeployqt 默认只重写 CFBundleExecutable（AutoDeskflow）的 Qt 链接路径，
# 漏掉 core 的话，绿色版在没装 Homebrew Qt 的机器上会起不来
"$MACDEPLOYQT" "$APP" -verbose=1 \
  -executable="$APP/Contents/MacOS/autodeskflow-core" \
  2>&1 | grep -viE "^Log: (Deploy|Copy)|^ERROR" | tail -20 || true
ok "Qt 框架与插件已嵌入"

step "4/8 补齐 QtSvg 并剪除用不到的组件"
# 整套界面图标是 SVG，靠 PlugIns/iconengines/libqsvgicon.dylib 渲染，它依赖 QtSvg。
# Homebrew 的 qtsvg 是独立 formula，macdeployqt（含 -libpath）解析不到，只能手工部署，
# 否则界面图标会全空。
deploy_extra_framework "$QTSVG_FRAMEWORK" "QtSvg"
SVGICON="$APP/Contents/PlugIns/iconengines/libqsvgicon.dylib"
if [ -f "$SVGICON" ]; then
  install_name_tool -change "@rpath/QtSvg.framework/Versions/A/QtSvg" \
    "@loader_path/../../Frameworks/QtSvg.framework/Versions/A/QtSvg" "$SVGICON"
  ok "  libqsvgicon.dylib 已指向内嵌 QtSvg"
fi
# 虚拟键盘插件会连带拖入 QtQuick/QtQml 全家桶（约 12 MB），本应用不需要
rm -f "$APP/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
rmdir "$APP/Contents/PlugIns/platforminputcontexts" 2>/dev/null || true
# PDF 图像格式插件依赖 QtPdf（Homebrew 独立 formula，未部署），本应用不读 PDF
rm -f "$APP/Contents/PlugIns/imageformats/libqpdf.dylib"
rm -rf "$APP/Contents/Resources/qml"
prune_unused_frameworks
ok "Frameworks 现为 $(du -sh "$APP/Contents/Frameworks" | cut -f1)"

step "5/8 放回 Finder 扩展"
if [ -d "$APPEX_TMP" ]; then
  mkdir -p "$APP/Contents/PlugIns"
  mv "$APPEX_TMP" "$APP/Contents/PlugIns/DeskflowPaste.appex"
  rmdir "$STAGE/_appex" 2>/dev/null || true
  ok "DeskflowPaste.appex 已放回 Contents/PlugIns/"
fi

step "6/8 清理多余数据"
find "$APP" -name '.DS_Store' -delete
find "$APP" -maxdepth 4 -name '*.dSYM' -exec rm -rf {} + 2>/dev/null || true
find "$APP" -name '*.prl' -delete
ok "已清理 .DS_Store / dSYM / .prl"

step "7/8 代码签名（由内到外）"
codesign --force --deep --sign "$CERT" --timestamp=none "$APP" 2>&1 | tail -3
# appex 需要带 entitlements 单独重签，FinderSync 沙箱注册才认
if [ -d "$APP/Contents/PlugIns/DeskflowPaste.appex" ] && [ "$CERT" != "-" ]; then
  codesign --force --sign "$CERT" --entitlements "$ENTITLEMENTS" --timestamp=none \
    "$APP/Contents/PlugIns/DeskflowPaste.appex" 2>&1 | tail -2
fi
# 重签顶层，重新封印 PlugIns
codesign --force --sign "$CERT" --timestamp=none "$APP" 2>&1 | tail -2
# 清掉扩展属性（bundle 签名在 _CodeSignature/ 与 Mach-O 内，不依赖 xattr）
xattr -cr "$APP" 2>/dev/null || true
codesign --verify --deep --strict "$APP" && ok "签名校验通过"

step "8/8 打包 zip"
mkdir -p "$OUT_DIR"
rm -f "$OUT_ZIP"
# 用 info-zip 而非 ditto -c -k：
#   -y 保留符号链接（framework 的 Versions/Current 等）
#   -X 不写 AppleDouble；macOS 会自动给文件打上受保护的 com.apple.provenance
#      （xattr -c 删不掉），ditto 会把它序列化成 __MACOSX/._* 垃圾
( cd "$STAGE" && zip -r -y -X -q "$OUT_ZIP" "$APP_NAME" )
ok "$OUT_ZIP ($(du -h "$OUT_ZIP" | cut -f1))"

step "解包回验"
VERIFY_DIR="$STAGE/_verify"
rm -rf "$VERIFY_DIR" && mkdir -p "$VERIFY_DIR"
unzip -q "$OUT_ZIP" -d "$VERIFY_DIR"
codesign --verify --deep --strict "$VERIFY_DIR/$APP_NAME" && ok "zip 内 .app 签名有效"

echo "--- 自包含检查：不应引用 /opt/homebrew 或 /usr/local ---"
leaks=$(find "$VERIFY_DIR/$APP_NAME" -type f -perm +111 | while IFS= read -r f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' \
    | grep -E '^/opt/homebrew|^/usr/local/(lib|opt)' | sed "s|^|  $(basename "$f") -> |"
done | sort -u || true)
if [ -n "$leaks" ]; then
  warn "仍有外部依赖："; echo "$leaks"
else
  ok "全部依赖已内嵌（只剩 /usr/lib 与 /System 系统库）"
fi

echo "--- @rpath 可解析性检查 ---"
unresolved=$(find "$VERIFY_DIR/$APP_NAME" -type f -perm +111 | while IFS= read -r f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@rpath/' | while read -r dep; do
    [ -e "$VERIFY_DIR/$APP_NAME/Contents/Frameworks/${dep#@rpath/}" ] \
      || echo "  $(basename "$f") -> $dep"
  done
done | sort -u || true)
if [ -n "$unresolved" ]; then
  warn "存在无法解析的 @rpath 依赖："; echo "$unresolved"
else
  ok "所有 @rpath 依赖均可解析"
fi

echo "--- 启动自检 ---"
"$VERIFY_DIR/$APP_NAME/Contents/MacOS/autodeskflow-core" --version | head -1
rm -rf "$VERIFY_DIR"

inspect_zip
