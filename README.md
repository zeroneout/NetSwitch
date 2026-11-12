# NetSwitch — 一键切换本地网络 / 无线网络

一个纯 C 写的 Win32 小工具：扫描本机的有线网卡和无线网卡，点一下就把目标网卡启用、把另一块禁用，实现「有线 ↔ 无线」的一键切换。

## 界面

- 左右两张卡片分别显示**本地网络（有线）**和**无线网络（Wi-Fi）**的网卡名、连接状态、IPv4 地址
- 绿色「已连接 / 已禁用 / 未连接 / 未找到」状态标记
- 「当前上网出口」标签：标出默认路由实际走的那块网卡
- 两个大按钮：**切换到本地网络** / **切换到无线网络**
- 复选框：切换时是否自动禁用另一块网卡（默认勾选）
- 右上角显示当前权限，「刷新」可重新扫描网卡

## Windows 7 兼容性（重点）

本程序刻意绕开了 Win7 上最常见的三类启动失败：

| 风险 | 处理方式 |
|---|---|
| `ucrtbase.dll` / `api-ms-win-crt-*.dll` 缺失（UCRT 版工具链的默认产物） | 不用 C 运行时：自带 `memcpy/memset/memmove/memcmp`，`-nostdlib` 直接链系统 DLL |
| PE 子系统版本过高导致「不是有效的 Win32 应用程序」 | 固定 `MajorSubsystemVersion=6, Minor=1`（= Win7） |
| manifest / SxS 依赖（comctl32 v6 等） | 不使用 comctl32、不嵌入 manifest，按钮与复选框全部自绘（纯 `user32` + `gdi32`） |

最终 exe 的导入表只有 7 个系统 DLL，Win7 全部自带：

```
ADVAPI32.dll  GDI32.dll  IPHLPAPI.DLL  KERNEL32.dll  SETUPAPI.dll  SHELL32.dll  USER32.dll
```

> 只输出 64 位版本需要 x64 工具链。若要在 32 位 Win7 上运行，把编译命令里的 `gcc` 换成 `i686-w64-mingw32-gcc`（`windres` 同理）重新编译即可，源码无需改动。

## 使用

1. 双击运行；如果不是管理员，程序会询问是否提权重启（切换网卡必须有管理员权限）。
2. 点「切换到本地网络」或「切换到无线网络」。
3. 状态栏会给出结果，例如：`已切换到「无线网络」WLAN（IPv4 192.168.1.23），并禁用另一块网卡。`

安全设计：

- 目标网卡**启用失败**时立即中止，不会把另一块网卡也禁掉（避免直接断网）。
- 目标网卡**尚未连通**（网线没插好 / 没连上 Wi-Fi）且要禁用另一块时，先弹确认框。
- 蓝牙 PAN、VMware/VirtualBox、Hyper-V、TAP、VPN 等虚拟或非物理网卡会被自动过滤，不会误操作。

## 编译

MinGW-w64（UCRT 或 MSVCRT 版本均可）：

```bat
build.bat
```

或手动：

```bash
windres NetSwitch.rc -O coff -o NetSwitch.res.o
gcc -O2 -s -mwindows -ffreestanding -fno-builtin -fno-stack-protector \
    -nostdlib -nostartfiles -nodefaultlibs -Wl,-e,AppEntry \
    -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,1 \
    -o NetSwitch.exe NetSwitch.c NetSwitch.res.o \
    -lkernel32 -luser32 -lgdi32 -ladvapi32 -lshell32 -lsetupapi -liphlpapi -lcfgmgr32
```

MSVC 2015+ 也能编（源码用 `_MSC_VER` 条件编译跳过自带运行时）：

```bat
cl /O2 /MT /DUNICODE /D_UNICODE NetSwitch.c /link /SUBSYSTEM:WINDOWS ^
   setupapi.lib iphlpapi.lib cfgmgr32.lib shell32.lib user32.lib gdi32.lib advapi32.lib
```

## 原理

- **枚举网卡**：`SetupDiGetClassDevsW(GUID_DEVCLASS_NET)` 遍历网络设备类，读驱动键里的 `NetCfgInstanceId`（GUID）与 `PhysicalMediaType`（9 = 802.11 无线，14 = 802.3 有线）区分类型；名称关键字做兜底判断。
- **启用 / 禁用**：`SetupDiSetClassInstallParamsW` + `SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)`，等价于设备管理器里的「禁用/启用设备」，立即生效，不弹黑框、不依赖 `netsh`。
- **状态与 IP**：`GetAdaptersAddresses` 取 `OperStatus` 与首选 IPv4，`GetIpForwardTable` 找默认路由的接口索引。
- **禁用判定**：`CM_Get_DevNode_Status` 的 `problem == CM_PROB_DISABLED(22)`。

## 性能细节

- **窗口先出来，网卡后面扫**：`WM_CREATE` 只建控件，网卡枚举用 `PostMessage` 挪到窗口显示之后执行。冷启动到窗口可见约 **30 ms**（同机空白窗口基线约 28 ms），期间卡片显示「扫描中…」。
- **每块网卡只开一次注册表键**：`NetCfgInstanceId`、`PhysicalMediaType`、`*IfType` 一次读完；虚拟网卡先按名字过滤掉，根本不碰注册表。
- **批量改状态**：切换时只枚举一遍设备树，对所有目标网卡一起处理，不再「每块网卡各扫一遍」。
- **切换时不假死**：等驱动生效改用消息泵自旋（`PumpWait`），期间窗口照常重绘；配合 `g_busy` 标记屏蔽重复点击。
- **双缓冲位图常驻复用**，重绘不再反复创建 GDI 对象。

## 文件

| 文件 | 说明 |
|---|---|
| `NetSwitch.c` | 全部源码（约 1370 行，单文件） |
| `NetSwitch.rc` | 图标 + 版本信息资源 |
| `NetSwitch.ico` | 程序图标（16–128 px） |
| `build.bat` | 一键编译脚本 |
| `NetSwitch.exe` | 编译好的 64 位程序（126 KB，无任何运行时依赖） |
