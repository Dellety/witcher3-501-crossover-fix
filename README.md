# The Witcher 3 Remastered 5.01 — CrossOver Black Screen Fix
# 巫师 3 重制版 5.01 — CrossOver 黑屏修复

Fixes the black screen / hang on launch of **The Witcher 3: Wild Hunt — Remastered (patch 5.01)** on Macs running **CrossOver (D3DMetal backend)**. The original community fix ([tholtman1-del/witcher3-crossover-fix](https://github.com/tholtman1-del/witcher3-crossover-fix)) only covers 5.00b and stops working on 5.01; this patch rebuilds the hooking mechanism and additionally fixes four issues that 5.01 exposed.

修复《巫师 3:狂猎 重制版》(**5.01 补丁**)在 Mac 的 **CrossOver(D3DMetal 后端)**下启动黑屏/卡死的问题。原社区补丁([tholtman1-del/witcher3-crossover-fix](https://github.com/tholtman1-del/witcher3-crossover-fix))只支持 5.00b,在 5.01 上失效;本补丁重做了挂钩方式,并修复了 5.01 暴露出的四层问题。

---

## Tested environment / 测试环境

| Item / 项目 | Value / 值 |
|---|---|
| Mac | MacBook Pro (Mac17,9), **Apple M5 Pro**, 48 GB RAM |
| macOS | **27.0.1** (Build 26A434), Apple Silicon (arm64) |
| CrossOver | **26.3** (D3DMetal / DXMT graphics backend, bottle "Steam") |
| Game | The Witcher 3: Wild Hunt — Remastered, **patch 5.01**, Steam (GOG should work too, untested) |
| Result / 结果 | Game runs and is playable / 游戏可正常启动并游玩 |
| Not working / 不可用 | In-game FSR is a dummy placeholder (keep it off); in-game DLSS cannot be unlocked via CrossOver's MetalFX layer (verified, see Known limitations) / 游戏内 FSR 为占位(保持关闭);DLSS 无法通过 CrossOver 的 MetalFX 层解锁(已实测,见"已知限制") |

---

## English

### Requirements

- CrossOver 26.x with the **D3DMetal** graphics backend (default for new bottles)
- The Witcher 3 Remastered updated to **5.01**, not running during installation
- Apple Silicon Mac

### Install

1. Double-click `install.command` (or run `./install.command` in Terminal);
2. It automatically searches all CrossOver bottles, Steam libraries and external drives for the game.

What it does:

| Action | Why |
|---|---|
| Replace `bin/x64_dx12/amd_fidelityfx_loader_dx12.dll` with our FFX proxy (the real loader is kept as `..._orig.dll`) | See "What it fixes" #2–4 |
| Add `bin/x64_dx12/d3d12.dll` (our d3d12 proxy) | See "What it fixes" #1 + device vtable hooking |
| Create symlink `bin/x64 -> x64_dx12` | CrossOver redirects the launch path to the non-existent `bin\x64` (the DX11 path that 5.x no longer ships) |
| Add bottle registry override `HKCU\Software\Wine\DllOverrides` → `d3d12 = native,builtin` | **Critical**: CrossOver forces the game onto the builtin d3d12; without this override the proxy DLL in the game folder is silently ignored |
| (untouched) `D3DM_ENABLE_METALFX` in the bottle config | DLSS/MetalFX experiment flag, left alone — see Known limitations |

### Uninstall

Double-click `uninstall.command`: restores the real FFX loader, removes both proxy DLLs, the symlink and the registry override.

### What it fixes (technical summary)

The 5.01 "black screen" is actually a **crash ~6 s into startup, then the game's own crash handler deadlocks**: two threads enter dbghelp to write a minidump at the same time, dbghelp is not thread-safe, everything hangs at ~100 % CPU. What used to be blamed on shader compilation is dbghelp burning CPU. The crash itself has four layers:

1. **Wine's `D3D12CreateDevice` returns `S_FALSE` for capability probes** (ppDevice == NULL) — Windows only ever returns S_OK or a failure code. The game misreads the odd "success" and later dereferences a NULL device (page fault at `witcher3+0x1cd4544`).
   → the d3d12 proxy normalizes probes to S_OK, and fixes up out-param `S_FALSE` + NULL device via QueryInterface.
2. **FFX `ffxQuery(GET_VERSIONS)` returns ERROR on Apple GPUs** (provider vendor check).
   → the FFX proxy answers locally when the real query fails.
3. **The game walks the version array with do-while semantics**: even with `count == 0` it unconditionally reads `versionIds[0]` (verified in the disassembly at `witcher3+0x1cd4544`: the zero-count path skips the allocation but still loads through the NULL array). Windows FFX always reports ≥ 1 version, so this game bug never fires there.
   → answer with **exactly one stub version**, never zero.
4. **FFX context creation failures are also unchecked.**
   → hand back a no-op dummy context for upscale/framegen contexts (FSR effectively off).

Plus, the upstream patch's core feature is kept and strengthened: stream-output pipelines (GS without PS / SO entries) crash Apple's D3DMetal shader converter, so they must be rejected. 5.01 builds its poison pipeline *before* the first FFX call, so hooking at FFX time is too late — the d3d12 proxy patches the device vtable the instant `D3D12CreateDevice` returns. The vtable hook is **single-owner** (only the d3d12 proxy hooks; discovered the hard way: two proxies racing to hook the same slots can chain into a cycle → hundreds of thousands of pipeline creations → hang), and it bails out instead of overwriting if a foreign hook is detected.

Log excerpt from a healthy boot: `streamout entries=3 → SKIPPED`.

### Known limitations

- **In-game FSR is a dummy placeholder.** The game requires "at least one FFX version exists" to boot (see fix #3), so the option always appears, but selecting it does nothing real. Keep it off.
- **In-game DLSS cannot be unlocked** via CrossOver's MetalFX layer (`D3DM_ENABLE_METALFX=1`): tested — the game runs fine with it enabled (after the hook-race fix), but the DLSS option stays grey. Pursue image quality with the game's own AA/sharpening settings.
- Rejecting the poison pipeline means one in-game visual effect may be missing (same as the upstream patch).

### Troubleshooting

**Still a black screen after install** → check the registry override actually exists (most common failure point):
```
CX_BOTTLE=Steam /Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine \
  --bottle Steam reg add "HKCU\Software\Wine\DllOverrides" /v d3d12 /t REG_SZ /d "native,builtin" /f
```
(replace `Steam` with your bottle name)

**Verify the proxies are active** → each launch rewrites `drive_c/users/crossover/ffxproxy.log` and `d3d12proxy.log` inside the bottle. A healthy boot shows `hooked D3D12 device` and dozens of paired `CreateGraphicsPipelineState ... hr=0x00000000` lines within a minute. Warning sign: tens of thousands of pipeline creations with no `hr=` lines (hook cycle) — if you ever see that, please open an issue.

**After a game update or Steam "verify integrity of files"** → run `install.command` again (the real loader gets restored by Steam and needs to be re-shelved as `_orig.dll`).

### Repository layout & source

```
├── README.md            this file
├── install.command      installer
├── uninstall.command    uninstaller
├── LICENSE              MIT (with upstream attribution)
├── dlls/
│   ├── d3d12.dll                     d3d12 proxy (build artifact)
│   └── amd_fidelityfx_loader_dx12.dll FFX proxy (build artifact)
└── src/
    ├── d3d12proxy.c     d3d12 proxy source (single file, no dependencies)
    ├── ffxproxy.c       FFX proxy source (single file, based on the upstream 5.00b version)
    └── build.sh         build script
```

Build it yourself (requires [zig](https://ziglang.org), `brew install zig`):
```
./src/build.sh
```
Both proxies perform no network access; logs go to the bottle's user directory. `FFXPROXY_DUMP=1` dumps shader DXIL for debugging.

### Credits & license

- Upstream 5.00b patch: [tholtman1-del/witcher3-crossover-fix](https://github.com/tholtman1-del/witcher3-crossover-fix) (MIT) — the FFX forwarding design, DPI-awareness note, stream-output rejection logic and the installer's game-search logic are derived from that project.
- The 5.01 modifications are published under MIT as well, see `LICENSE`.
- Not affiliated with CD PROJEKT RED, CodeWeavers or Apple.

---

## 简体中文

### 环境要求

- CrossOver 26.x,图形后端为 **D3DMetal**(新建瓶的默认值)
- 《巫师 3 重制版》已更新到 **5.01**,安装补丁时游戏未在运行
- Apple Silicon 的 Mac

### 安装

1. 双击 `install.command`(或在终端执行 `./install.command`);
2. 脚本会自动搜索所有 CrossOver 瓶、Steam 库和外接盘里的游戏。

安装内容:

| 动作 | 原因 |
|---|---|
| 用本补丁的 FFX 代理替换 `bin/x64_dx12/amd_fidelityfx_loader_dx12.dll`(真实 loader 保留为 `..._orig.dll`) | 见"修了什么" #2–4 |
| 新增 `bin/x64_dx12/d3d12.dll`(本补丁的 d3d12 代理) | 见"修了什么" #1 + 设备虚表挂钩 |
| 创建符号链接 `bin/x64 -> x64_dx12` | CrossOver 会把启动路径重定向到不存在的 `bin\x64`(5.x 已不再附带的 DX11 版路径) |
| 瓶注册表写入 `HKCU\Software\Wine\DllOverrides` → `d3d12 = native,builtin` | **关键**:CrossOver 默认强制游戏用 builtin d3d12,不写这条,游戏目录里的代理 DLL 会被静默忽略 |
| (不修改)瓶配置里的 `D3DM_ENABLE_METALFX` | DLSS/MetalFX 实验开关,默认不动,见"已知限制" |

### 卸载

双击 `uninstall.command`:恢复真实 FFX loader、删除两个代理 DLL、移除符号链接和注册表覆盖。

### 修了什么(技术摘要)

5.01 的"黑屏"其实是**启动约 6 秒后崩溃,然后游戏自带的 crash handler 死锁**:两个线程同时进 dbghelp 写 minidump,而 dbghelp 非线程安全,于是全员挂在 ~100% CPU 上。以前被当成"着色器编译卡死"的,其实是 dbghelp 在烧 CPU。崩溃本身有四层:

1. **Wine 的 `D3D12CreateDevice` 对能力探测(ppDevice == NULL)返回 `S_FALSE`** —— Windows 上只会返回 S_OK 或失败码。游戏把这种"成功又不对劲"的返回值当成坏路径,随后对 NULL 设备解引用(页错误,`witcher3+0x1cd4544`)。
   → d3d12 代理把探测调用规范成 S_OK;out-param 形式的 S_FALSE + NULL 设备用 QueryInterface 补救。
2. **FFX 的 `ffxQuery(GET_VERSIONS)` 在 Apple GPU 上返回 ERROR**(provider 的供应商检查)。
   → FFX 代理在真实查询失败时本地应答。
3. **游戏遍历版本数组用的是 do-while 语义**:即使 `count == 0`,也会无条件读 `versionIds[0]`(反汇编验证于 `witcher3+0x1cd4544`:count 为 0 的路径跳过了内存分配,但仍然从 NULL 数组读取)。Windows 上 FFX 永远报告 ≥1 个版本,所以这个游戏 bug 在 Windows 上从不发作。
   → 应答"**恰好 1 个** stub 版本",绝不能答 0 个。
4. **FFX 上下文创建失败同样没被检查。**
   → 对升频/帧生成的上下文创建失败时,返回一个空转的 dummy 上下文(等效 FSR 关闭)。

另外,原补丁的核心功能被保留并加强:stream-output 管线(带几何着色器、无像素着色器/带 SO 条目)会让 Apple 的 D3DMetal 着色器编译器崩溃,必须拒绝。5.01 在**第一次 FFX 调用之前**就构建这条毒管线,所以在 FFX 时机挂钩太晚——d3d12 代理本身就是 d3d12.dll,在 `D3D12CreateDevice` 返回的瞬间就把设备虚表挂上。虚表钩子是**单点所有**(只有 d3d12 代理挂钩;这个教训是踩出来的:两个代理赛跑抢同一些槽位可能连成环 → 十几万条管线创建 → 卡死),并且发现槽位被外来代码改写时会放弃而不是覆盖。

健康启动的日志特征:`streamout entries=3 → SKIPPED`。

### 已知限制

- **游戏内 FSR 选项是占位的。** 游戏需要"至少存在一个 FFX 版本"才能启动(见修复 #3),所以选项永远会出现,但选了没有真实升频。请保持关闭。
- **游戏内 DLSS 无法解锁**:实测 CrossOver 的 MetalFX 层(`D3DM_ENABLE_METALFX=1`)——开启后游戏运行正常(在钩子赛跑修复之后),但 DLSS 选项仍是灰色。追求画质请用游戏自带的抗锯齿/锐化设置。
- 拒绝毒管线意味着游戏内某个视觉特效可能缺失(与原补丁相同)。

### 排错

**装完还是黑屏** → 确认注册表覆盖真的写进去了(最常见的失效点):
```
CX_BOTTLE=Steam /Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine \
  --bottle Steam reg add "HKCU\Software\Wine\DllOverrides" /v d3d12 /t REG_SZ /d "native,builtin" /f
```
(把 `Steam` 换成你的瓶名。)

**确认代理在跑** → 每次启动都会重写瓶内 `drive_c/users/crossover/ffxproxy.log` 和 `d3d12proxy.log`。健康启动会在一分钟内出现 `hooked D3D12 device` 和几十条成对的 `CreateGraphicsPipelineState ... hr=0x00000000`。危险信号:几万条管线创建但没有 `hr=` 行(钩子成环)——如果看到这种日志请开 issue。

**游戏更新或 Steam"验证文件完整性"之后** → 重跑一次 `install.command`(Steam 会还原真实 loader,需要重新保留为 `_orig.dll`)。

### 目录结构与源码

```
├── README.md            本文件
├── install.command      安装脚本
├── uninstall.command    卸载脚本
├── LICENSE              MIT(含上游致谢)
├── dlls/
│   ├── d3d12.dll                     d3d12 代理(构建产物)
│   └── amd_fidelityfx_loader_dx12.dll FFX 代理(构建产物)
└── src/
    ├── d3d12proxy.c     d3d12 代理源码(单文件,无依赖)
    ├── ffxproxy.c       FFX 代理源码(单文件,基于上游 5.00b 版修改)
    └── build.sh         构建脚本
```

自己构建(需要 [zig](https://ziglang.org),`brew install zig`):
```
./src/build.sh
```
两个代理不访问网络;日志写在瓶内用户目录;`FFXPROXY_DUMP=1` 可转储着色器 DXIL 供调试。

### 致谢与许可

- 上游 5.00b 补丁:[tholtman1-del/witcher3-crossover-fix](https://github.com/tholtman1-del/witcher3-crossover-fix)(MIT)——FFX 转发设计、DPI 声明、毒管线拒绝逻辑与安装脚本的游戏搜索逻辑源自该项目。
- 本补丁的 5.01 修改部分同样以 MIT 发布,见 `LICENSE`。
- 与 CD PROJEKT RED、CodeWeavers、Apple 均无关联。
