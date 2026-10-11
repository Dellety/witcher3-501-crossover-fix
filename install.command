#!/bin/zsh
# The Witcher 3 Remastered 5.01 black-screen fix for CrossOver (D3DMetal).
# Usage: double-click, or run: ./install.command ["/path/to/The Witcher 3"]
# The path may be the game folder, its bin or bin/x64_dx12 folder, or witcher3.exe.
set -e
here=${0:A:h}
dll_dir="$here/dlls"
ffx_dll="$dll_dir/amd_fidelityfx_loader_dx12.dll"
d3d12_dll="$dll_dir/d3d12.dll"
[ -f "$ffx_dll" ] && [ -f "$d3d12_dll" ] || { echo "dlls/ 缺少文件,请保持补丁目录完整。"; exit 1; }
bottles="$HOME/Library/Application Support/CrossOver/Bottles"
wine_bin="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
[ -x "$wine_bin" ] || wine_bin="$HOME/Applications/CrossOver/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"

# Turn whatever the user gave us into the bin/x64_dx12 folder, or print nothing.
resolve_game_dir() {
  local p="$1"
  while [[ "$p" == *" " && "$p" != *"\\ " ]]; do p="${p% }"; done   # Terminal adds a trailing space on drag-and-drop
  p="${p%\"}"; p="${p#\"}"; p="${p%\'}"; p="${p#\'}"      # strip quotes
  p="${p//\\ / }"                                          # un-escape drag-and-drop spaces
  p="${p%/}"
  [ -f "$p" ] && p="${p:h}"
  for c in "$p" "$p/x64_dx12" "$p/bin/x64_dx12"; do
    [ -f "$c/witcher3.exe" ] && [ "${c:t}" = "x64_dx12" ] && { echo "${c:A}"; return; }
  done
  return 0
}

win_to_mac() {
  local bottle="$1" win="$2"
  local letter="${win[1]:l}" rest="${win[3,-1]//\\\\//}"
  rest="${rest//\\//}"
  local dev="$bottle/dosdevices/$letter:"
  [ -e "$dev" ] && echo "${dev:A}$rest"
  return 0
}

found=()
bottle_of=()   # parallel array: bottle directory (may be empty)
add() { local r; r=$(resolve_game_dir "$1"); [ -n "$r" ] && found+=("$r"); return 0; }

if [ -n "$1" ]; then
  add "$1"
  [ ${#found[@]} -gt 0 ] || { echo "在 $1 下没有找到 bin/x64_dx12/witcher3.exe"; exit 1; }
else
  echo "搜索《巫师 3》..."
  for b in "$bottles"/*(N/); do
    # 1. 瓶内 C 盘
    for exe in "${(@f)$(find "$b/drive_c" -path "*/bin/x64_dx12/witcher3.exe" 2>/dev/null)}"; do
      add "$exe"; bottle_of+=("$b")
    done
    # 2. 瓶映射的其他盘符(跳过 y:=家目录 和 z:=整盘)
    for dev in "$b"/dosdevices/[a-xA-X]:(N@); do
      [ "${dev:t}" = "c:" ] && continue
      case "${dev:A}" in /|"$HOME"|/Users|/System*) continue ;; esac
      for exe in "${(@f)$(find -L "$dev/" -maxdepth 9 -path "*/bin/x64_dx12/witcher3.exe" 2>/dev/null)}"; do
        add "$exe"; bottle_of+=("$b")
      done
    done
    # 3. Steam 库目录
    for vdf in "$b"/drive_c/Program\ Files*/Steam/{steamapps,config}/libraryfolders.vdf(N); do
      for lib in "${(@f)$(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' "$vdf")}"; do
        mac=$(win_to_mac "$b" "$lib")
        [ -n "$mac" ] && [ -d "$mac/steamapps/common" ] || continue
        for exe in "$mac"/steamapps/common/*/bin/x64_dx12/witcher3.exe(N); do add "$exe"; bottle_of+=("$b"); done
      done
    done
  done
  # 4. 外接盘
  for vol in /Volumes/*(N/); do
    [ "${vol:A}" = "/" ] && continue
    for exe in "${(@f)$(find "$vol" -maxdepth 9 -path "*/bin/x64_dx12/witcher3.exe" 2>/dev/null)}"; do
      add "$exe"; bottle_of+=("")
    done
  done
fi

found=(${(u)found:#})
if [ ${#found[@]} -eq 0 ]; then
  echo
  echo "没有自动找到游戏。把《巫师 3》文件夹(包含 bin 和 content 的那层)拖进本窗口后回车:"
  read -r answer
  add "$answer"
  [ ${#found[@]} -gt 0 ] || { echo "该路径下没有 bin/x64_dx12/witcher3.exe"; exit 1; }
fi

for d in "${found[@]}"; do
  echo
  echo "补丁目标: $d"
  # --- ffx 代理:保留真实 loader 为 _orig.dll ---
  if ! grep -q "ffxproxy" "$d/amd_fidelityfx_loader_dx12.dll" 2>/dev/null; then
    mv -f "$d/amd_fidelityfx_loader_dx12.dll" "$d/amd_fidelityfx_loader_dx12_orig.dll"
    echo "  真实 FFX loader 已保留为 amd_fidelityfx_loader_dx12_orig.dll"
  fi
  cp "$ffx_dll" "$d/amd_fidelityfx_loader_dx12.dll"
  # --- d3d12 代理 ---
  cp "$d3d12_dll" "$d/d3d12.dll"
  # --- bin/x64 符号链接(CrossOver 会把启动路径重定向到不存在的 bin\x64)---
  bin="${d:h}"
  if [ -L "$bin/x64" ]; then
    echo "  bin/x64 链接已存在"
  elif [ ! -e "$bin/x64" ]; then
    ln -s x64_dx12 "$bin/x64" && echo "  已创建 bin/x64 -> x64_dx12"
  else
    echo "  警告: bin/x64 已存在且不是本补丁创建的,保持原样"
  fi
  echo "  DLL 已安装"
done

# --- 注册表覆盖:让游戏加载游戏目录里的 d3d12.dll(CrossOver 默认强制 builtin)---
echo
if [ -x "$wine_bin" ]; then
  typeset -a seen_bottles
  for b in "${bottle_of[@]}"; do
    [ -z "$b" ] && continue
    [[ " ${seen_bottles[*]} " == *" $b "* ]] && continue
    seen_bottles+=("$b")
    name="${b:t}"
    echo "写入瓶 \"$name\" 的 d3d12=native,builtin 覆盖..."
    CX_BOTTLE="$name" "$wine_bin" --bottle "$name" reg add "HKCU\\Software\\Wine\\DllOverrides" /v d3d12 /t REG_SZ /d "native,builtin" /f >/dev/null 2>&1 \
      && echo "  完成" || echo "  失败(可手动执行,见 README 排错一节)"
  done
  if [ ${#seen_bottles[@]:-0} -eq 0 ]; then
    echo "游戏在瓶外(外接盘等)。请手动对你使用的瓶执行(见 README):"
    echo "  CX_BOTTLE=<瓶名> \"$wine_bin\" --bottle <瓶名> reg add 'HKCU\\Software\\Wine\\DllOverrides' /v d3d12 /t REG_SZ /d native,builtin /f"
  fi
else
  echo "未找到 CrossOver 的 wine 命令,请按 README 手动写入注册表覆盖。"
fi

# --- 可选:DLSS(MetalFX)说明 ---
echo
echo "提示:Mac 上的 DLSS 依赖 CrossOver 的 MetalFX 替换层,默认未开启(部分环境下不稳定)。"
echo "想尝试 DLSS 可执行:"
echo "  sed -i '' 's/D3DM_ENABLE_METALFX\" = \"0/D3DM_ENABLE_METALFX\" = \"1/' \"<瓶路径>/cxbottle.conf\""
echo "然后重启 CrossOver 瓶。若出现异常,把该值改回 0 即可。"

echo
echo "安装完成,正常启动游戏即可。"
echo "提醒:游戏内 FSR 选项是占位用的,请保持关闭。"
