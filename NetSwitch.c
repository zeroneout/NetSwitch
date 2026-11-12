/* ============================================================================
 *  NetSwitch.c  -  一键切换「本地网络(有线)」与「无线网络(Wi-Fi)」
 *
 *  纯 Win32 / C，面向 Windows 7 及以上：
 *    - 不依赖 UCRT / api-ms-win-crt-*（自带 memcpy/memset，-nostdlib 链接）
 *    - 不依赖 manifest / SxS / comctl32 v6（全部自绘，裸 user32 控件）
 *    - 子系统版本固定 6.01，Win7 加载器可直接运行
 *
 *  功能：
 *    - 自动枚举物理有线网卡与无线网卡（过滤虚拟网卡）
 *    - 一键启用目标网卡、禁用另一类网卡（可关闭）
 *    - 显示每块网卡状态、IPv4 地址、当前默认路由出口
 *    - 非管理员启动时提示并支持一键提权重启
 *
 *  编译（MinGW-w64，任意版本，含 UCRT 版）：
 *    gcc -O2 -s -mwindows -ffreestanding -fno-builtin -fno-stack-protector \
 *        -nostdlib -nostartfiles -nodefaultlibs -Wl,-e,AppEntry \
 *        -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,1 \
 *        -o NetSwitch.exe NetSwitch.c \
 *        -lkernel32 -luser32 -lgdi32 -ladvapi32 -lshell32 -lsetupapi -liphlpapi -lcfgmgr32
 *
 *  编译（MSVC 2015+，/MT 静态 CRT，同样兼容 Win7）：
 *    cl /O2 /MT /DUNICODE /D_UNICODE NetSwitch.c /link /SUBSYSTEM:WINDOWS \
 *       setupapi.lib iphlpapi.lib cfgmgr32.lib shell32.lib user32.lib gdi32.lib advapi32.lib
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shellapi.h>

#ifndef GET_X_LPARAM_
#define GET_X_LPARAM_(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM_(lp) ((int)(short)HIWORD(lp))
#endif

#ifndef CM_PROB_DISABLED
#define CM_PROB_DISABLED 22
#endif

/* ==========================================================================
 *  一、极简运行时（不链 CRT）
 * ==========================================================================*/
typedef unsigned long long ns_usize;

#ifndef _MSC_VER
void *memcpy(void *dst, const void *src, ns_usize n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, ns_usize n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
void *memset(void *dst, int c, ns_usize n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
int memcmp(const void *a, const void *b, ns_usize n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
#endif

static void *xalloc(ns_usize n)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
}
static void xfree(void *p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

/* ==========================================================================
 *  二、常量
 * ==========================================================================*/
#define APP_TITLE       L"网络切换工具"
#define APP_CLASS       L"NetSwitchWndClass"

#define NIC_ETH         1
#define NIC_WIFI        2

#define MAX_NIC         32
#define GW_LEN          64

/* 控件 ID */
#define IDC_BTN_ETH     1001
#define IDC_BTN_WIFI    1002
#define IDC_BTN_REFRESH 1003
#define IDC_CHK_DIS     1004

/* 颜色（浅色主题） */
#define C_BG            RGB(255, 255, 255)
#define C_LINE          RGB(229, 231, 235)
#define C_TITLE         RGB(17, 24, 39)
#define C_TEXT          RGB(55, 65, 81)
#define C_MUTED         RGB(107, 114, 128)
#define C_CARD          RGB(249, 250, 251)
#define C_BORDER        RGB(229, 231, 235)

#define C_ETH           RGB(37, 99, 235)
#define C_ETH_DOWN      RGB(29, 78, 216)
#define C_WIFI          RGB(5, 150, 105)
#define C_WIFI_DOWN     RGB(4, 120, 87)
#define C_OFF           RGB(203, 213, 225)

#define C_GREEN         RGB(22, 163, 74)
#define C_GREEN_BG      RGB(220, 252, 231)
#define C_RED           RGB(220, 38, 38)
#define C_RED_BG        RGB(254, 226, 226)
#define C_ORANGE        RGB(217, 119, 6)
#define C_ORANGE_BG     RGB(254, 243, 199)
#define C_GRAY_BG       RGB(241, 245, 249)

#define C_BTNLIGHT      RGB(243, 244, 246)
#define C_BTNLIGHT_DOWN RGB(229, 231, 235)

/* ==========================================================================
 *  三、全局状态
 * ==========================================================================*/
typedef struct {
    WCHAR name[160];        /* 网卡友好名 */
    WCHAR guid[GW_LEN];     /* NetCfgInstanceId */
    int   type;             /* NIC_ETH / NIC_WIFI */
    int   disabled;         /* 设备被禁用 */
    int   up;               /* 媒体已连接 */
    DWORD ifindex;          /* IP 接口索引 */
    DWORD ipv4;             /* 首选 IPv4（网络字节序的整数值，已转为主机序） */
    int   has_ip;
} NICINFO;

static NICINFO  g_nic[MAX_NIC];
static int      g_nicCount;
/* 待处理的网卡 GUID 列表（静态，避免在栈上开 4KB 数组触发栈探测例程） */
static WCHAR    g_pending[MAX_NIC][GW_LEN];

static HWND     g_hwnd;
static HWND     g_btnEth, g_btnWifi, g_btnRefresh, g_chkDis;
static HFONT    g_fTitle, g_fSub, g_fCardTitle, g_fName, g_fBadge;
static HFONT    g_fBtn, g_fSmall, g_fCheck;

static int      g_dpi = 96;
static BOOL     g_admin = FALSE;
static BOOL     g_disableOther = TRUE;
static BOOL     g_busy = FALSE;        /* 切换进行中：屏蔽重入 */
static BOOL     g_scanDone = FALSE;    /* 首次枚举是否已完成 */
static int      g_ethBest = -1, g_wifiBest = -1;
static DWORD    g_defRouteIf = 0;

static WCHAR    g_status[300];
static COLORREF g_statusColor = C_MUTED;

#define SC(n) MulDiv((n), g_dpi, 96)

static const GUID kNetClassGuid =
    { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

/* ==========================================================================
 *  四、字符串工具
 * ==========================================================================*/
static WCHAR UpCase(WCHAR c)
{
    if (c >= L'a' && c <= L'z') return (WCHAR)(c - 32);
    return c;
}

static BOOL StrHasI(const WCHAR *hay, const WCHAR *needle)
{
    int i, j;
    if (!hay || !needle || !*needle) return FALSE;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            WCHAR a = hay[i + j], b = needle[j];
            if (!a) return FALSE;
            if (UpCase(a) != UpCase(b)) break;
        }
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}

static const WCHAR *kVirtualKeys[] = {
    L"wan miniport", L"virtual", L"vmware", L"virtualbox", L"hyper-v", L"vethernet",
    L"loopback", L"tap-win", L"tap-", L"tunnel", L"teredo", L"isatap", L"6to4",
    L"npcap", L"bluetooth", L"蓝牙", L"wi-fi direct", L"pseudo", L"ras async",
    L"miniport", L"vpn", L"anyconnect", L"openvpn", L"sangfor", L"easyconnect",
    L"桥接", L"虚拟", L"回环", L"隧道"
};
static const WCHAR *kWifiKeys[] = {
    L"wireless", L"wi-fi", L"wifi", L"802.11", L"wlan", L"无线", L"无线网"
};

static BOOL IsVirtualName(const WCHAR *name)
{
    int i;
    for (i = 0; i < (int)(sizeof(kVirtualKeys) / sizeof(kVirtualKeys[0])); i++)
        if (StrHasI(name, kVirtualKeys[i])) return TRUE;
    return FALSE;
}

static BOOL LooksWifi(const WCHAR *name)
{
    int i;
    for (i = 0; i < (int)(sizeof(kWifiKeys) / sizeof(kWifiKeys[0])); i++)
        if (StrHasI(name, kWifiKeys[i])) return TRUE;
    return FALSE;
}

static void IpToStr(DWORD ip, WCHAR *out)
{
    wsprintfW(out, L"%u.%u.%u.%u",
              (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
              (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF));
}

/* ==========================================================================
 *  五、设备枚举（SetupAPI）
 * ==========================================================================*/
static BOOL DevPropStr(HDEVINFO h, SP_DEVINFO_DATA *d, DWORD prop, WCHAR *out, DWORD cch)
{
    DWORD type = 0, size = 0;
    if (!out || cch == 0) return FALSE;
    out[0] = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(h, d, prop, &type, (PBYTE)out,
                                           cch * sizeof(WCHAR), &size))
        return FALSE;
    out[cch - 1] = 0;
    return TRUE;
}

/* 设备驱动键里关心的几项配置：一次打开、批量读取（原来每次读一个值都要
 * 单独 SetupDiOpenDevRegKey，一块网卡要开两次键，多网卡机器上开销明显） */
typedef struct {
    WCHAR guid[GW_LEN];
    DWORD pmt;      /* PhysicalMediaType；0xFFFFFFFF 表示驱动未上报 */
    int   iftype;   /* *IfType；-1 表示未上报 */
} DEVCONF;

static int StrToIntW(const WCHAR *s)
{
    int v = 0, neg = 0, any = 0;
    if (!s) return -1;
    while (*s == L' ') s++;
    if (*s == L'-') { neg = 1; s++; }
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (int)(*s - L'0'); s++; any = 1; }
    if (!any) return -1;
    return neg ? -v : v;
}

/* 读整型值，兼容 REG_DWORD 与 REG_SZ 两种写法 */
static int RegInt(HKEY k, const WCHAR *name, int def)
{
    DWORD type = 0, v = 0, sz = sizeof(v);
    WCHAR s[32];

    if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS &&
        type == REG_DWORD)
        return (int)v;

    sz = sizeof(s);
    type = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)s, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        s[31] = 0;
        return StrToIntW(s);
    }
    return def;
}

static BOOL ReadDevConf(HDEVINFO h, SP_DEVINFO_DATA *d, DEVCONF *c)
{
    HKEY k;
    DWORD type = 0, sz;
    BOOL ok = FALSE;

    ZeroMemory(c, sizeof(*c));
    c->pmt = 0xFFFFFFFFu;
    c->iftype = -1;

    k = SetupDiOpenDevRegKey(h, d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (k == INVALID_HANDLE_VALUE) return FALSE;

    sz = sizeof(c->guid);
    if (RegQueryValueExW(k, L"NetCfgInstanceId", NULL, &type,
                         (LPBYTE)c->guid, &sz) == ERROR_SUCCESS) {
        c->guid[GW_LEN - 1] = 0;
        ok = (c->guid[0] != 0);
    }
    if (ok) {
        c->pmt = (DWORD)RegInt(k, L"PhysicalMediaType", -1);
        if (c->pmt == 0xFFFFFFFFu || c->pmt == 0)   /* 没上报或“未指定”时看 *IfType */
            c->iftype = RegInt(k, L"*IfType", -1);
    }
    RegCloseKey(k);
    return ok;
}

static BOOL DevDrvStr(HDEVINFO h, SP_DEVINFO_DATA *d, const WCHAR *val, WCHAR *out, DWORD cch)
{
    HKEY k = SetupDiOpenDevRegKey(h, d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    DWORD sz = cch * sizeof(WCHAR), type = 0;
    BOOL ok = FALSE;
    if (k != INVALID_HANDLE_VALUE) {
        if (RegQueryValueExW(k, val, NULL, &type, (LPBYTE)out, &sz) == ERROR_SUCCESS) {
            out[cch - 1] = 0;
            ok = TRUE;
        }
        RegCloseKey(k);
    }
    return ok;
}

/* 采集 IP 层状态：GUID -> 运行状态 / 接口索引 / IPv4 */
typedef struct {
    WCHAR guid[GW_LEN];
    DWORD ifindex;
    int   up;
    DWORD ipv4;
    int   has_ip;
} IPSTATE;
static IPSTATE g_ips[MAX_NIC];
static int     g_ipCount;

static void CollectIpState(void)
{
    ULONG sz = 0;
    IP_ADAPTER_ADDRESSES *head, *a;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;

    g_ipCount = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &sz) != ERROR_BUFFER_OVERFLOW)
        return;
    head = (IP_ADAPTER_ADDRESSES *)xalloc(sz);
    if (!head) return;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, head, &sz) == NO_ERROR) {
        for (a = head; a && g_ipCount < MAX_NIC; a = a->Next) {
            IPSTATE *s = &g_ips[g_ipCount];
            IP_ADAPTER_UNICAST_ADDRESS *ua;

            ZeroMemory(s, sizeof(*s));   /* 数组按槽复用，必须先清干净 */
            if (MultiByteToWideChar(CP_ACP, 0, a->AdapterName, -1,
                                    s->guid, GW_LEN) <= 0)
                continue;
            s->ifindex = a->IfIndex;
            s->up = (a->OperStatus == IfOperStatusUp);

            for (ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                if (ua->Address.lpSockaddr &&
                    ua->Address.lpSockaddr->sa_family == AF_INET) {
                    const BYTE *b = (const BYTE *)&((SOCKADDR_IN *)
                        ua->Address.lpSockaddr)->sin_addr.S_un.S_addr;
                    DWORD ip = ((DWORD)b[0] << 24) | ((DWORD)b[1] << 16) |
                               ((DWORD)b[2] << 8) | (DWORD)b[3];
                    if (ip == 0) continue;
                    if (!s->has_ip || ((ip >> 24) != 169 && (s->ipv4 >> 24) == 169)) {
                        s->ipv4 = ip;
                        s->has_ip = 1;
                    }
                }
            }
            g_ipCount++;
        }
    }
    xfree(head);
}

static IPSTATE *FindIpState(const WCHAR *guid)
{
    int i;
    for (i = 0; i < g_ipCount; i++)
        if (lstrcmpiW(g_ips[i].guid, guid) == 0) return &g_ips[i];
    return NULL;
}

/* 当前默认路由（0.0.0.0/0）使用的接口索引 */
static DWORD GetDefaultRouteIf(void)
{
    DWORD sz = 0, best = 0, bestMetric = 0xFFFFFFFF, i;
    PMIB_IPFORWARDTABLE t;

    if (GetIpForwardTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER) return 0;
    t = (PMIB_IPFORWARDTABLE)xalloc(sz);
    if (!t) return 0;
    if (GetIpForwardTable(t, &sz, FALSE) == NO_ERROR) {
        for (i = 0; i < t->dwNumEntries; i++) {
            if (t->table[i].dwForwardDest == 0 && t->table[i].dwForwardMask == 0) {
                if (t->table[i].dwForwardMetric1 < bestMetric) {
                    bestMetric = t->table[i].dwForwardMetric1;
                    best = t->table[i].dwForwardIfIndex;
                }
            }
        }
    }
    xfree(t);
    return best;
}

static void EnumerateNICs(void)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i;
    WCHAR name[160];

    g_nicCount = 0;
    CollectIpState();
    g_defRouteIf = GetDefaultRouteIf();

    h = SetupDiGetClassDevsW(&kNetClassGuid, NULL, NULL, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return;

    d.cbSize = sizeof(d);
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        NICINFO *n;
        DEVCONF cfg;
        ULONG status = 0, problem = 0;
        IPSTATE *ip;

        if (g_nicCount >= MAX_NIC) break;

        /* 先只看名字：虚拟网卡直接跳过，省掉后面的注册表访问 */
        name[0] = 0;
        if (!DevPropStr(h, &d, SPDRP_FRIENDLYNAME, name, 160) || !name[0]) {
            if (!DevPropStr(h, &d, SPDRP_DEVICEDESC, name, 160) || !name[0])
                lstrcpynW(name, L"未知网卡", 160);
        }
        if (IsVirtualName(name)) continue;

        if (!ReadDevConf(h, &d, &cfg) || !cfg.guid[0]) continue;

        n = &g_nic[g_nicCount];
        ZeroMemory(n, sizeof(*n));   /* 数组按槽复用：不清零会把上一轮的
                                        disabled / up / ip 残留下来 */

        lstrcpynW(n->name, name, 160);
        lstrcpynW(n->guid, cfg.guid, GW_LEN);

        /* 物理介质类型：9 = 802.11，14 = 802.3；
         * 驱动没上报时退回 *IfType（71 = IEEE80211，6 = Ethernet） */
        if (cfg.pmt == 9)               n->type = NIC_WIFI;
        else if (cfg.pmt == 14)         n->type = NIC_ETH;
        else if (cfg.iftype == 71)      n->type = NIC_WIFI;
        else if (cfg.iftype == 6)       n->type = NIC_ETH;
        else if (LooksWifi(name))       n->type = NIC_WIFI;
        else                            n->type = NIC_ETH;

        if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS &&
            problem == CM_PROB_DISABLED)
            n->disabled = 1;

        ip = FindIpState(cfg.guid);
        if (ip) {
            n->up = ip->up;
            n->ifindex = ip->ifindex;
            n->ipv4 = ip->ipv4;
            n->has_ip = ip->has_ip;
        }
        g_nicCount++;
    }
    SetupDiDestroyDeviceInfoList(h);
}

static int FindBest(int type)
{
    int i, best = -1, bestScore = -1;
    for (i = 0; i < g_nicCount; i++) {
        int score;
        if (g_nic[i].type != type) continue;
        if (g_nic[i].up)            score = 3;
        else if (!g_nic[i].disabled) score = 2;
        else                        score = 1;
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}

/* ==========================================================================
 *  六、启用 / 禁用网卡
 * ==========================================================================*/

/* 等待期间保持消息泵运转，避免窗口出现“未响应”。
 * 因为会分发消息，调用方需用 g_busy 做重入保护。 */
static void PumpWait(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;

    for (;;) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                PostQuitMessage((int)m.wParam);
                return;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if ((int)(GetTickCount() - end) >= 0) break;
        Sleep(8);
    }
}

static BOOL ApplyChange(HDEVINFO h, SP_DEVINFO_DATA *d, BOOL enable, DWORD scope)
{
    SP_PROPCHANGE_PARAMS p;
    ZeroMemory(&p, sizeof(p));
    p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    p.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
    p.Scope = scope;
    p.HwProfile = 0;

    if (!SetupDiSetClassInstallParamsW(h, d, &p.ClassInstallHeader, sizeof(p)))
        return FALSE;
    return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, h, d) ? TRUE : FALSE;
}

static BOOL NicIsDisabledDevnode(SP_DEVINFO_DATA *d)
{
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, d->DevInst, 0) != CR_SUCCESS)
        return FALSE;
    return (problem == CM_PROB_DISABLED);
}

/* 一次枚举设备树，对 guids 里的所有目标应用状态；返回实际生效的数量。
 * 多网卡时避免“每块网卡各扫一遍设备树”。 */
static int ApplyMany(WCHAR guids[][GW_LEN], int count, BOOL enable)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i;
    int done = 0;

    if (count <= 0) return 0;
    h = SetupDiGetClassDevsW(&kNetClassGuid, NULL, NULL, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 0;

    d.cbSize = sizeof(d);
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        WCHAR g[GW_LEN];
        int k;

        if (!DevDrvStr(h, &d, L"NetCfgInstanceId", g, GW_LEN) || !g[0]) continue;

        for (k = 0; k < count; k++) {
            int t;

            if (lstrcmpiW(g, guids[k]) != 0) continue;

            if (NicIsDisabledDevnode(&d) == !enable) { done++; break; }

            if (!ApplyChange(h, &d, enable, DICS_FLAG_GLOBAL))
                ApplyChange(h, &d, enable, DICS_FLAG_CONFIGSPECIFIC);

            /* 复核：通常第一次就已是目标状态，最多等到 6*80ms */
            for (t = 0; t < 6; t++) {
                if (NicIsDisabledDevnode(&d) == !enable) { done++; break; }
                PumpWait(80);
            }
            break;
        }
        if (!g_hwnd) break;   /* 窗口已被关闭，别再把剩下的网卡也动了 */
    }
    SetupDiDestroyDeviceInfoList(h);
    return done;
}

/* 按 GUID 设置单块网卡的状态 */
static BOOL SetNicState(const WCHAR *guid, BOOL enable)
{
    WCHAR one[1][GW_LEN];

    if (!guid) return FALSE;
    lstrcpynW(one[0], guid, GW_LEN);
    return ApplyMany(one, 1, enable) > 0;
}

/* ==========================================================================
 *  七、权限
 * ==========================================================================*/
static BOOL IsElevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION el;
    DWORD cb = 0;
    BOOL r = FALSE;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb))
            r = el.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }
    return r;
}

static void RelaunchAsAdmin(HWND hwnd)
{
    WCHAR path[MAX_PATH];
    SHELLEXECUTEINFOW sei;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        DestroyWindow(hwnd);
        return;
    }
    MessageBoxW(hwnd,
                L"未能以管理员身份启动，程序将在只读模式下运行。\n"
                L"可以右键程序 → 以管理员身份运行。",
                APP_TITLE, MB_ICONWARNING);
}

/* ==========================================================================
 *  八、绘制
 * ==========================================================================*/
/* 双缓冲位图常驻复用：重绘时不再反复 Create/Destroy GDI 对象 */
static HDC     g_memDC;
static HBITMAP g_memBmp;
static HGDIOBJ g_memOld;
static int     g_memW, g_memH;

static HDC GetBackBuf(HDC hdc, int w, int h)
{
    HBITMAP nb;

    if (!g_memDC) {
        g_memDC = CreateCompatibleDC(hdc);
        if (!g_memDC) return NULL;
    }
    if (g_memBmp && g_memW == w && g_memH == h) return g_memDC;

    nb = CreateCompatibleBitmap(hdc, w, h);
    if (!nb) return NULL;
    if (!g_memOld) g_memOld = SelectObject(g_memDC, nb);
    else           SelectObject(g_memDC, nb);
    if (g_memBmp) DeleteObject(g_memBmp);
    g_memBmp = nb;
    g_memW = w;
    g_memH = h;
    return g_memDC;
}

static void FreeBackBuf(void)
{
    if (g_memDC) {
        if (g_memOld) SelectObject(g_memDC, g_memOld);
        if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = NULL; }
        DeleteDC(g_memDC);
        g_memDC = NULL;
    }
    g_memOld = NULL;
}

static void RoundFill(HDC dc, const RECT *r, int rad, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pn);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pn);
}

static void DrawStr(HDC dc, const WCHAR *s, int x, int y, int w, int h,
                    HFONT f, COLORREF c, UINT fmt)
{
    RECT r;
    HGDIOBJ old;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static int TextWidth(HDC dc, HFONT f, const WCHAR *s)
{
    SIZE sz = { 0, 0 };
    HGDIOBJ old = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static void DrawBadge(HDC dc, int right, int top, const WCHAR *txt,
                      COLORREF fg, COLORREF bg)
{
    int padX = SC(9), h = SC(22);
    int w = TextWidth(dc, g_fBadge, txt) + padX * 2;
    RECT r;
    r.top = top; r.bottom = top + h;
    r.right = right; r.left = right - w;
    RoundFill(dc, &r, SC(11), bg, bg);
    DrawStr(dc, txt, r.left, r.top, w, h, g_fBadge, fg,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawCard(HDC dc, int x, int y, int w, int h, int isEth, int idx)
{
    RECT card, inner;
    const WCHAR *title = isEth ? L"本地网络" : L"无线网络";
    const WCHAR *sub   = isEth ? L"有线以太网" : L"Wi-Fi 无线";
    COLORREF accent = isEth ? C_ETH : C_WIFI;
    COLORREF badgeFg, badgeBg;
    const WCHAR *badgeTxt;
    int badgeRight, i;

    card.left = x; card.top = y; card.right = x + w; card.bottom = y + h;
    RoundFill(dc, &card, SC(12), C_CARD, C_BORDER);

    /* 左侧色条 */
    inner.left = x + 1; inner.top = y + SC(14);
    inner.right = x + SC(4); inner.bottom = y + h - SC(14);
    RoundFill(dc, &inner, SC(3), accent, accent);

    DrawStr(dc, title, x + SC(18), y + SC(16), w - SC(120), SC(22),
            g_fCardTitle, C_TITLE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawStr(dc, sub, x + SC(18) + TextWidth(dc, g_fCardTitle, title) + SC(10),
            y + SC(16), SC(90), SC(22), g_fSmall, C_MUTED,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 状态徽章 */
    if (idx < 0) {
        badgeTxt = g_scanDone ? L"未找到" : L"扫描中";
        badgeFg  = g_scanDone ? C_RED : C_MUTED;
        badgeBg  = g_scanDone ? C_RED_BG : C_GRAY_BG;
    } else if (g_nic[idx].disabled) {
        badgeTxt = L"已禁用";
        badgeFg = C_MUTED; badgeBg = C_GRAY_BG;
    } else if (g_nic[idx].up) {
        badgeTxt = L"已连接";
        badgeFg = C_GREEN; badgeBg = C_GREEN_BG;
    } else {
        badgeTxt = L"未连接";
        badgeFg = C_ORANGE; badgeBg = C_ORANGE_BG;
    }
    badgeRight = x + w - SC(16);
    DrawBadge(dc, badgeRight, y + SC(14), badgeTxt, badgeFg, badgeBg);

    /* 网卡名 */
    if (idx >= 0) {
        DrawStr(dc, g_nic[idx].name, x + SC(18), y + SC(48), w - SC(36), SC(24),
                g_fName, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        const WCHAR *ph = !g_scanDone ? L"正在扫描…"
                        : (isEth ? L"未检测到有线网卡" : L"未检测到无线网卡");
        DrawStr(dc, ph, x + SC(18), y + SC(48), w - SC(36), SC(24),
                g_fName, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* 详情行 */
    if (idx >= 0) {
        WCHAR line[200];
        line[0] = 0;
        if (g_nic[idx].disabled) {
            lstrcpynW(line, L"设备已禁用，点击下方按钮启用", 200);
        } else if (g_nic[idx].has_ip) {
            WCHAR ip[40];
            IpToStr(g_nic[idx].ipv4, ip);
            wsprintfW(line, L"IPv4  %s", ip);
        } else if (g_nic[idx].up) {
            lstrcpynW(line, L"已连接，正在获取地址…", 200);
        } else {
            lstrcpynW(line, isEth ? L"网线未连接" : L"未连接到无线网络", 200);
        }
        DrawStr(dc, line, x + SC(18), y + SC(78), w - SC(36), SC(20),
                g_fSmall, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        /* 当前出口标记 */
        if (!g_nic[idx].disabled && g_nic[idx].ifindex &&
            g_nic[idx].ifindex == g_defRouteIf) {
            WCHAR tag[64];
            RECT tr;
            int tw;
            lstrcpynW(tag, L"当前上网出口", 64);
            tw = TextWidth(dc, g_fSmall, tag) + SC(16);
            tr.left = x + SC(18); tr.top = y + SC(102);
            tr.right = tr.left + tw; tr.bottom = tr.top + SC(20);
            RoundFill(dc, &tr, SC(10), isEth ? RGB(219,234,254) : RGB(209,250,229),
                      isEth ? RGB(219,234,254) : RGB(209,250,229));
            DrawStr(dc, tag, tr.left, tr.top, tw, SC(20), g_fSmall,
                    isEth ? C_ETH : C_WIFI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    (void)i;
}

static void PaintMain(HDC dc, const RECT *rc)
{
    int W = rc->right;
    int pad = SC(24), gap = SC(16);
    int cardW = (W - pad * 2 - gap) / 2;
    int cardY = SC(88), cardH = SC(140);
    RECT line;

    /* 顶部标题 */
    DrawStr(dc, L"网络切换", pad, SC(20), SC(300), SC(32), g_fTitle, C_TITLE,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawStr(dc, L"一键切换本地有线网络与无线网络", pad, SC(52), SC(360), SC(20),
            g_fSub, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 权限标识（放在「刷新」按钮左侧） */
    {
        const WCHAR *t = g_admin ? L"管理员" : L"普通权限";
        COLORREF c = g_admin ? C_GREEN : C_RED;
        int tw = TextWidth(dc, g_fSmall, t);
        int x = W - pad - SC(76) - SC(16) - tw;
        DrawStr(dc, t, x, SC(29), tw + 2, SC(20), g_fSmall, c,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        {   /* 小圆点 */
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, x - SC(12), SC(35), x - SC(4), SC(43));
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(b);
        }
    }

    line.left = pad; line.right = W - pad; line.top = SC(78); line.bottom = SC(79);
    {
        HBRUSH b = CreateSolidBrush(C_LINE);
        FillRect(dc, &line, b);
        DeleteObject(b);
    }

    DrawCard(dc, pad, cardY, cardW, cardH, 1, g_ethBest);
    DrawCard(dc, pad + cardW + gap, cardY, cardW, cardH, 0, g_wifiBest);

    /* 状态文本（复选框下方） */
    if (g_status[0]) {
        DrawStr(dc, g_status, pad, SC(352), W - pad * 2, SC(50), g_fSmall,
                g_statusColor, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

/* ==========================================================================
 *  九、业务逻辑
 * ==========================================================================*/
static void SetStatus(const WCHAR *s, COLORREF c)
{
    lstrcpynW(g_status, s, 300);
    g_statusColor = c;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void RefreshAll(void)
{
    EnumerateNICs();
    g_ethBest  = FindBest(NIC_ETH);
    g_wifiBest = FindBest(NIC_WIFI);

    if (g_btnEth)     EnableWindow(g_btnEth, g_ethBest >= 0);
    if (g_btnWifi)    EnableWindow(g_btnWifi, g_wifiBest >= 0);
    if (g_hwnd)       InvalidateRect(g_hwnd, NULL, FALSE);
}

static void SwitchTo(int type, HWND hwnd)
{
    WCHAR msg[300], ip[40];
    int target, i, disabledCount = 0;
    BOOL enabledNow = FALSE;

    if (g_busy) return;

    if (!g_admin) {
        SetStatus(L"缺少管理员权限，无法启用/禁用网卡。请点击右上角提示或以管理员身份重新运行。",
                  C_RED);
        MessageBoxW(hwnd,
                    L"启用或禁用网卡需要管理员权限。\n\n"
                    L"请关闭程序后右键 → 以管理员身份运行。",
                    APP_TITLE, MB_ICONINFORMATION);
        return;
    }

    if (!g_scanDone) {
        SetStatus(L"正在扫描网卡，请稍候…", C_MUTED);
        return;
    }

    /* 直接采用界面当前展示的数据，避免切换前再做一次完整枚举 */
    target = (type == NIC_ETH) ? g_ethBest : g_wifiBest;
    if (target < 0) {
        SetStatus(type == NIC_ETH ? L"未检测到可用的有线网卡。" : L"未检测到可用的无线网卡。",
                  C_RED);
        return;
    }

    g_busy = TRUE;

    SetStatus(L"正在切换，请稍候…", C_MUTED);
    UpdateWindow(hwnd);

    /* 1) 先启用目标网卡，保证切换过程尽短 */
    if (g_nic[target].disabled) {
        SetStatus(L"正在启用目标网卡…", C_MUTED);
        UpdateWindow(hwnd);
        if (!SetNicState(g_nic[target].guid, TRUE)) {
            SetStatus(L"启用目标网卡失败：请确认已授予管理员权限，或检查该网卡驱动状态。",
                      C_RED);
            goto done;
        }
        enabledNow = TRUE;
    } else if (g_disableOther && !g_nic[target].up) {
        /* 目标网卡本来就启用但未连通：切换后可能直接断网，先确认 */
        int i2, willDisable = 0;
        for (i2 = 0; i2 < g_nicCount; i2++)
            if (g_nic[i2].type != type && !g_nic[i2].disabled) willDisable = 1;

        if (willDisable) {
            const WCHAR *label = (type == NIC_ETH) ? L"本地网络" : L"无线网络";
            const WCHAR *why   = (type == NIC_ETH) ? L"未连通（网线未插好或链路未通）"
                                                   : L"未连接（尚未关联到无线网络）";
            WCHAR q[400];
            wsprintfW(q, L"「%s」当前%s。\n\n"
                         L"继续切换会禁用另一块网卡，可能导致短暂断网。是否继续？",
                      label, why);
            if (MessageBoxW(hwnd, q, APP_TITLE, MB_YESNO | MB_ICONWARNING) != IDYES) {
                SetStatus(L"已取消切换。", C_MUTED);
                goto done;
            }
        }
    }

    /* 2) 再禁用另一类网卡（一次枚举批量处理） */
    if (g_disableOther) {
        int gc = 0;

        for (i = 0; i < g_nicCount; i++) {
            if (g_nic[i].type == type || g_nic[i].disabled) continue;
            lstrcpynW(g_pending[gc++], g_nic[i].guid, GW_LEN);
        }
        disabledCount = ApplyMany(g_pending, gc, FALSE);
    }

    PumpWait(350);
    RefreshAll();

    /* 3) 结果提示 */
    {
        int t2 = (type == NIC_ETH) ? g_ethBest : g_wifiBest;
        const WCHAR *label = (type == NIC_ETH) ? L"本地网络" : L"无线网络";
        WCHAR name[160];
        lstrcpynW(name, (t2 >= 0) ? g_nic[t2].name : L"", 160);

        if (t2 >= 0 && g_nic[t2].has_ip) {
            IpToStr(g_nic[t2].ipv4, ip);
            if (disabledCount > 0)
                wsprintfW(msg, L"已切换到「%s」%s（IPv4 %s），并禁用另一块网卡。",
                          label, name, ip);
            else if (enabledNow)
                wsprintfW(msg, L"已启用「%s」%s（IPv4 %s）。", label, name, ip);
            else
                wsprintfW(msg, L"当前已在「%s」%s（IPv4 %s），无需切换。",
                          label, name, ip);
            SetStatus(msg, C_GREEN);
        } else {
            if (t2 >= 0 && g_nic[t2].disabled)
                SetStatus(L"切换失败：目标网卡仍处于禁用状态，请检查驱动或权限。", C_RED);
            else {
                wsprintfW(msg, L"已启用「%s」%s，正在等待网络就绪…", label, name);
                SetStatus(msg, C_ORANGE);
            }
        }
    }

done:
    g_busy = FALSE;
}

/* ==========================================================================
 *  十、控件自绘
 * ==========================================================================*/
static void DrawButton(HDC dc, const DRAWITEMSTRUCT *di, COLORREF base,
                       COLORREF down, COLORREF off, BOOL lightText)
{
    RECT r = di->rcItem;
    BOOL sel = (di->itemState & ODS_SELECTED) != 0;
    BOOL dis = (di->itemState & ODS_DISABLED) != 0;
    COLORREF fill = dis ? off : (sel ? down : base);
    WCHAR txt[128];

    RoundFill(dc, &r, SC(10), fill, fill);
    GetWindowTextW(di->hwndItem, txt, 128);
    DrawStr(dc, txt, r.left, r.top, r.right - r.left, r.bottom - r.top,
            g_fBtn, lightText ? RGB(255, 255, 255) : C_TEXT,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawCheck(HDC dc, const DRAWITEMSTRUCT *di)
{
    RECT r = di->rcItem;
    int box = SC(18), top = r.top + (r.bottom - r.top - box) / 2;
    RECT b;
    COLORREF fg, bg;

    {   /* 先铺背景，避免露出按钮默认灰底 */
        HBRUSH bk = CreateSolidBrush(C_BG);
        FillRect(dc, &r, bk);
        DeleteObject(bk);
    }

    b.left = r.left; b.top = top; b.right = r.left + box; b.bottom = top + box;

    if (g_disableOther) {
        fg = C_ETH;
        bg = C_ETH;
    } else {
        fg = RGB(156, 163, 175);
        bg = RGB(255, 255, 255);
    }
    RoundFill(dc, &b, SC(5), bg, fg);

    if (g_disableOther) {
        POINT pt[3];
        HPEN pn = CreatePen(PS_SOLID, SC(2), RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pn);
        pt[0].x = b.left + SC(4);  pt[0].y = b.top + SC(9);
        pt[1].x = b.left + SC(7);  pt[1].y = b.top + SC(12);
        pt[2].x = b.left + SC(13); pt[2].y = b.top + SC(5);
        Polyline(dc, pt, 3);
        SelectObject(dc, op);
        DeleteObject(pn);
    }

    DrawStr(dc, L"切换时自动禁用另一块网卡", r.left + box + SC(9), r.top,
            r.right - r.left - box - SC(9), r.bottom - r.top,
            g_fCheck, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/* ==========================================================================
 *  十一、窗口
 * ==========================================================================*/
static void CreateFonts(void)
{
    const WCHAR *face = L"微软雅黑";

#define MKF(px, weight) CreateFontW(-SC(px), 0, 0, 0, (weight), 0, 0, 0, \
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, \
        DEFAULT_PITCH | FF_DONTCARE, face)

    g_fTitle     = MKF(24, FW_SEMIBOLD);
    g_fSub       = MKF(13, FW_NORMAL);
    g_fCardTitle = MKF(15, FW_SEMIBOLD);
    g_fName      = MKF(15, FW_NORMAL);
    g_fBadge     = MKF(12, FW_MEDIUM);
    g_fBtn       = MKF(16, FW_SEMIBOLD);
    g_fSmall     = MKF(12, FW_NORMAL);
    g_fCheck     = MKF(13, FW_NORMAL);
#undef MKF
}

static void DestroyFonts(void)
{
    HFONT *fs[] = { &g_fTitle, &g_fSub, &g_fCardTitle, &g_fName,
                    &g_fBadge, &g_fBtn, &g_fSmall, &g_fCheck };
    int i;
    for (i = 0; i < (int)(sizeof(fs) / sizeof(fs[0])); i++) {
        if (*fs[i]) { DeleteObject(*fs[i]); *fs[i] = NULL; }
    }
}

static void CalcLayout(HWND hwnd, RECT *rRef, RECT *rEth, RECT *rWifi, RECT *rChk)
{
    RECT rc;
    int W, pad, gap, cardW, btnY, btnH, chkY;

    GetClientRect(hwnd, &rc);
    W = rc.right;
    pad = SC(24);
    gap = SC(16);
    cardW = (W - pad * 2 - gap) / 2;
    btnY = SC(244);
    btnH = SC(64);
    chkY = SC(318);

    SetRect(rRef,  W - pad - SC(76), SC(24), W - pad, SC(54));
    SetRect(rEth,  pad, btnY, pad + cardW, btnY + btnH);
    SetRect(rWifi, pad + cardW + gap, btnY, pad + cardW + gap + cardW, btnY + btnH);
    SetRect(rChk,  pad, chkY, pad + SC(320), chkY + SC(24));
}

static void LayoutChildren(HWND hwnd)
{
    RECT a, b, c, d;

    CalcLayout(hwnd, &a, &b, &c, &d);
    MoveWindow(g_btnRefresh, a.left, a.top, a.right - a.left, a.bottom - a.top, TRUE);
    MoveWindow(g_btnEth,     b.left, b.top, b.right - b.left, b.bottom - b.top, TRUE);
    MoveWindow(g_btnWifi,    c.left, c.top, c.right - c.left, c.bottom - c.top, TRUE);
    MoveWindow(g_chkDis,     d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
}

static void CreateChildren(HWND hwnd)
{
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    RECT rRef, rEth, rWifi, rChk;

    /* 一次算准位置直接创建，省掉“先建后 MoveWindow”的一轮重排 */
    CalcLayout(hwnd, &rRef, &rEth, &rWifi, &rChk);

    g_btnEth = CreateWindowExW(0, L"BUTTON", L"切换到本地网络",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        rEth.left, rEth.top, rEth.right - rEth.left, rEth.bottom - rEth.top,
        hwnd, (HMENU)(INT_PTR)IDC_BTN_ETH, hi, NULL);

    g_btnWifi = CreateWindowExW(0, L"BUTTON", L"切换到无线网络",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        rWifi.left, rWifi.top, rWifi.right - rWifi.left, rWifi.bottom - rWifi.top,
        hwnd, (HMENU)(INT_PTR)IDC_BTN_WIFI, hi, NULL);

    g_btnRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        rRef.left, rRef.top, rRef.right - rRef.left, rRef.bottom - rRef.top,
        hwnd, (HMENU)(INT_PTR)IDC_BTN_REFRESH, hi, NULL);

    g_chkDis = CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        rChk.left, rChk.top, rChk.right - rChk.left, rChk.bottom - rChk.top,
        hwnd, (HMENU)(INT_PTR)IDC_CHK_DIS, hi, NULL);

    /* 扫描完成前先禁掉切换按钮，避免拿到空数据就点 */
    EnableWindow(g_btnEth, FALSE);
    EnableWindow(g_btnWifi, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_admin = IsElevated();
        CreateFonts();
        CreateChildren(hwnd);

        /* 网卡枚举放到窗口显示之后再做：启动时不再卡在枚举上 */
        g_scanDone = FALSE;
        SetStatus(L"正在扫描网卡…", C_MUTED);
        PostMessageW(hwnd, WM_APP + 2, 0, 0);
        if (!g_admin) PostMessageW(hwnd, WM_APP + 1, 0, 0);
        return 0;

    case WM_APP + 2:   /* 首次扫描完成 */
        RefreshAll();
        g_scanDone = TRUE;
        if (g_admin)
            SetStatus(L"就绪。点击下方按钮即可切换网络。", C_MUTED);
        else
            SetStatus(L"当前为普通权限，只能查看状态。点击「普通权限」标记可提权重启。", C_RED);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP + 1:
        if (!g_admin) {
            int r = MessageBoxW(hwnd,
                L"启用 / 禁用网卡需要管理员权限。\n\n是否以管理员身份重新启动本程序？",
                APP_TITLE, MB_YESNO | MB_ICONQUESTION);
            if (r == IDYES) RelaunchAsAdmin(hwnd);
        }
        return 0;

    case WM_SIZE:
        if (g_btnEth) LayoutChildren(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HDC mem;

        GetClientRect(hwnd, &rc);
        mem = GetBackBuf(hdc, rc.right, rc.bottom);
        if (mem) {
            HBRUSH bg = CreateSolidBrush(C_BG);
            FillRect(mem, &rc, bg);
            DeleteObject(bg);

            PaintMain(mem, &rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        switch (di->CtlID) {
        case IDC_BTN_ETH:
            DrawButton(di->hDC, di, C_ETH, C_ETH_DOWN, C_OFF, TRUE);
            return TRUE;
        case IDC_BTN_WIFI:
            DrawButton(di->hDC, di, C_WIFI, C_WIFI_DOWN, C_OFF, TRUE);
            return TRUE;
        case IDC_BTN_REFRESH:
            DrawButton(di->hDC, di, C_BTNLIGHT, C_BTNLIGHT_DOWN, RGB(250, 250, 250), FALSE);
            return TRUE;
        case IDC_CHK_DIS:
            DrawCheck(di->hDC, di);
            return TRUE;
        }
        return FALSE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (g_busy) return 0;              /* 切换进行中，忽略重复点击 */
        if (id == IDC_BTN_ETH)      { SwitchTo(NIC_ETH, hwnd);  return 0; }
        if (id == IDC_BTN_WIFI)     { SwitchTo(NIC_WIFI, hwnd); return 0; }
        if (id == IDC_BTN_REFRESH)  {
            RefreshAll();
            g_scanDone = TRUE;
            SetStatus(L"已刷新网卡状态。", C_MUTED);
            return 0;
        }
        if (id == IDC_CHK_DIS) {
            g_disableOther = !g_disableOther;
            InvalidateRect(g_chkDis, NULL, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
        /* 点击右上角权限标记 → 提权重启 */
        if (!g_admin) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (GET_X_LPARAM_(lp) > rc.right - SC(160) && GET_Y_LPARAM_(lp) < SC(60)) {
                SendMessageW(hwnd, WM_APP + 1, 0, 0);
                return 0;
            }
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && !g_admin) {
            POINT pt;
            RECT rc;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            GetClientRect(hwnd, &rc);
            if (pt.x > rc.right - SC(160) && pt.y < SC(60) && pt.y > SC(10)) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        FreeBackBuf();
        DestroyFonts();
        g_hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  十二、入口
 * ==========================================================================*/
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    RECT wr;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int ww, wh, sw, sh;
    HDC sdc;

    (void)hPrev; (void)cmd; (void)show;

    SetProcessDPIAware();

    /* 先把 DPI 与窗口尺寸算准，CreateWindow 一次到位；
     * 不再“先建窗口、再在 WM_CREATE 里 SetWindowPos 重排”
     * （那次重排会触发 WM_SIZE、四个自绘控件重新定位与重绘） */
    sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (g_dpi <= 0) g_dpi = 96;

    SetRect(&wr, 0, 0, SC(600), SC(416));
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    ww = wr.right - wr.left;
    wh = wr.bottom - wr.top;
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_CLASS;

    if (!RegisterClassExW(&wc)) return 0;

    hwnd = CreateWindowExW(0, APP_CLASS, APP_TITLE, style,
                           (sw - ww) / 2, (sh - wh) / 2, ww, wh,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

#ifndef _MSC_VER
void WINAPI AppEntry(void)
{
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL,
                               GetCommandLineW(), SW_SHOWDEFAULT));
}
#endif
