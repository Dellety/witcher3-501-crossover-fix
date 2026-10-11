#!/bin/zsh
# 构建两个代理 DLL(需要 zig:brew install zig)
set -e
here=${0:A:h}
cd "$here"
zig cc -target x86_64-windows-gnu -shared -O2 -o ../dlls/d3d12.dll d3d12proxy.c
zig cc -target x86_64-windows-gnu -shared -O2 -o ../dlls/amd_fidelityfx_loader_dx12.dll ffxproxy.c
echo "构建完成:"
ls -la ../dlls/
