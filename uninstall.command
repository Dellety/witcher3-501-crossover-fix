#!/bin/zsh
# 卸载巫师 3 5.01 CrossOver 修复补丁,恢复打补丁前的状态。
set -e
here=${0:A:h}
bottles="$HOME/Library/Application Support/CrossOver/Bottles"
wine_bin="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
[ -x "$wine_bin" ] || wine_bin="$HOME/Applications/CrossOver/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"

resolve_game_dir() {
  local p="${1%/}"
  p="${p%\"}"; p="${p#\"}"
  [ -f "$p" ] && p="${p:h}"
  for c in "$p" "$p/x64_dx12" "$p/bin/x64_dx12"; do
    [ -f "$c/witcher3.exe" ] && [ "${c:t}" = "x64_dx12" ] && { echo "${c:A}"; return; }
  done
  return 0
}

found=()
if [ -n "$1" ]; then
  r=$(resolve_game_dir "$1"); [ -n "$r" ] && found+=("$r")
else
  for b in "$bottles"/*(N/); do
    for exe in "${(@f)$(find "$b/drive_c" -path "*/bin/x64_dx12/witcher3.exe" 2>/dev/null)}"; do
      r=$(resolve_game_dir "$exe"); [ -n "$r" ] && found+=("$r")
    done
  done
  for vol in /Volumes/*(N/); do
    [ "${vol:A}" = "/" ] && continue
    for exe in "${(@f)$(find "$vol" -maxdepth 9 -path "*/bin/x64_dx12/witcher3.exe" 2>/dev/null)}"; do
      r=$(resolve_game_dir "$exe"); [ -n "$r" ] && found+=("$r")
    done
  done
fi
found=(${(u)found:#})
[ ${#found[@]} -gt 0 ] || { echo "没有找到已安装的游戏。"; exit 1; }

for d in "${found[@]}"; do
  echo "清理: $d"
  rm -f "$d/d3d12.dll"
  if [ -f "$d/amd_fidelityfx_loader_dx12_orig.dll" ]; then
    rm -f "$d/amd_fidelityfx_loader_dx12.dll"
    mv "$d/amd_fidelityfx_loader_dx12_orig.dll" "$d/amd_fidelityfx_loader_dx12.dll"
    echo "  已恢复真实 FFX loader"
  else
    echo "  没有 _orig.dll(真实 loader 已丢失?),跳过恢复 —— 建议用 Steam 校验文件修复"
  fi
  bin="${d:h}"
  if [ -L "$bin/x64" ] && [ "$(readlink "$bin/x64")" = "x64_dx12" ]; then
    rm "$bin/x64"
    echo "  已移除 bin/x64 链接"
  fi
done

if [ -x "$wine_bin" ]; then
  for b in "$bottles"/*(N/); do
    grep -q '"d3d12"="native,builtin"' "$b/user.reg" 2>/dev/null || continue
    name="${b:t}"
    CX_BOTTLE="$name" "$wine_bin" --bottle "$name" reg delete "HKCU\\Software\\Wine\\DllOverrides" /v d3d12 /f >/dev/null 2>&1 \
      && echo "已移除瓶 \"$name\" 的 d3d12 覆盖" || true
  done
fi
echo
echo "卸载完成。注意:D3DM_ENABLE_METALFX=1 被保留(那是 CrossOver 自身的显示功能,不影响游戏启动)。"
