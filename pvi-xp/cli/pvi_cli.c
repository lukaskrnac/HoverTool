/* ===========================================================================
   pvi_cli.c  -  command line PVI client for B&R PLC  (Windows XP compatible)

   Cita a zapisuje procesne premenne PLC B&R cez Automation Net PVI.

   Vlastnosti:
     * konzolova aplikacia, ziadny .NET
     * PviCom.dll sa NEnacita pri linkovani, ale za behu:
         -DLL najde sama: --dll / %PVI_INSTALL_FOLDER% / registry App Paths
                         / standardne cesty / PATH
         - funkcie sa tahaju cez LoadLibraryA + GetProcAddress
       => na preklad nie su potrebne ziadne hlavicky ani .lib z PVI
     * bez CRT: importuje iba KERNEL32.dll  ->  funguje na Windows XP SP3

   Priklad:
     pvi_cli.exe info
     pvi_cli.exe read  gTest.iCounter --ip 192.168.1.10
     pvi_cli.exe write gTest.iCounter 42 --ip 192.168.1.10
     pvi_cli.exe read  gTest.rTeplota --ip 192.168.1.10 --proto ina

   Preklad (MinGW / zig / MSVC):
     zig cc  -target x86-windows-gnu -O2 -nostdlib -Wl,--entry=mainCRTStartup \
             -o pvi_cli.exe pvi_cli.c
     i686-w64-mingw32-gcc -O2 -nostdlib -Wl,-e,mainCRTStartup -o pvi_cli.exe pvi_cli.c
   =========================================================================== */

/* ---- pripadne prepnutie konvencie volania (default stdcall) -------------- */
#ifdef PVI_CALL_CDECL
#  define PVIAPI __cdecl
#else
#  define PVIAPI __stdcall
#endif

typedef unsigned char      U8;
typedef unsigned short     U16;
typedef unsigned int       U32;
typedef signed   int       I32;
typedef unsigned long long U64;
typedef void *             HPVI_OBJ;

/* ===========================================================================
   1) minimalne Win32 deklaracie (iba KERNEL32, ziadny windows.h)
   =========================================================================== */
#define STD_OUTPUT_HANDLE ((U32)-11)
#define STD_INPUT_HANDLE  ((U32)-10)

__declspec(dllimport) HPVI_OBJ PVIAPI GetStdHandle(U32 nStdHandle);
__declspec(dllimport) I32      PVIAPI WriteFile(HPVI_OBJ h, const void *buf, U32 n, U32 *written, void *ov);
__declspec(dllimport) I32      PVIAPI ReadFile(HPVI_OBJ h, void *buf, U32 n, U32 *read, void *ov);
__declspec(dllimport) U32      PVIAPI GetConsoleProcessList(U32 *list, U32 count);
__declspec(dllimport) HPVI_OBJ PVIAPI LoadLibraryA(const char *name);
__declspec(dllimport) HPVI_OBJ PVIAPI GetProcAddress(HPVI_OBJ mod, const char *name);
__declspec(dllimport) U32      PVIAPI GetModuleFileNameA(HPVI_OBJ mod, char *buf, U32 n);
__declspec(dllimport) U32      PVIAPI GetEnvironmentVariableA(const char *name, char *buf, U32 n);
__declspec(dllimport) char *   PVIAPI GetCommandLineA(void);
__declspec(dllimport) void     PVIAPI Sleep(U32 ms);
typedef struct { U16 wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } T_SYSTIME;
__declspec(dllimport) void     PVIAPI GetLocalTime(T_SYSTIME *st);
__declspec(dllimport) I32      PVIAPI RegOpenKeyExA(HPVI_OBJ key, const char *sub, U32 opt, U32 sam, HPVI_OBJ *res);
__declspec(dllimport) I32      PVIAPI RegQueryValueExA(HPVI_OBJ key, const char *name, U32 *res, U32 *type, U8 *data, U32 *cb);
__declspec(dllimport) I32      PVIAPI RegCloseKey(HPVI_OBJ key);
__declspec(dllimport) void     PVIAPI ExitProcess(U32 code);

#define HKEY_LOCAL_MACHINE ((HPVI_OBJ)0x80000001)

/* ---- nahrada za compiler/CRT rutiny, ktore kompilator moze vygenerovat ---- */
void *memcpy(void *dst, const void *src, U32 n)
{
    U8 *d = (U8 *)dst; const U8 *s = (const U8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, U32 n)
{
    U8 *d = (U8 *)dst;
    while (n--) *d++ = (U8)c;
    return dst;
}

U32 strlen(const char *s)
{
    U32 n = 0;
    while (s[n]) n++;
    return n;
}

int memcmp(const void *a, const void *b, U32 n)
{
    const U8 *pa = (const U8 *)a, *pb = (const U8 *)b;
    while (n--) { if (*pa != *pb) return (int)*pa - (int)*pb; pa++; pb++; }
    return 0;
}

/* ===========================================================================
   2) nahrada za libc (bez CRT)
   =========================================================================== */
static int s_len(const char *s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }

static void mem_zero(void *d, U32 n)
{
    U8 *p = (U8 *)d; while (n--) *p++ = 0;
}

static void s_cpy(char *d, const char *s, int max)
{
    int i = 0;
    if (max <= 0) return;
    while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void s_cat(char *d, const char *s, int max)
{
    int i = s_len(d), j = 0;
    while (s && s[j] && i < max - 1) { d[i++] = s[j++]; }
    d[i] = 0;
}

static int s_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(U8)*a - (int)(U8)*b;
}

static char ch_low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int s_icmp(const char *a, const char *b)
{
    while (*a && ch_low(*a) == ch_low(*b)) { a++; b++; }
    return (int)(U8)ch_low(*a) - (int)(U8)ch_low(*b);
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* ===========================================================================
   3) vystup na konzolu (bez printf)
   =========================================================================== */
static HPVI_OBJ g_out;
static char     g_buf[1024];
static int      g_buflen;

/* "capture" rezim: out() ide do bufferu (pre monitor - porovnavanie zmien) */
static int  g_capture;
static char g_capbuf[1024];
static int  g_caplen;

static void out_flush(void)
{
    U32 written = 0;
    if (g_out == 0) g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_buflen > 0)
    {
        WriteFile(g_out, g_buf, (U32)g_buflen, &written, 0);
        g_buflen = 0;
    }
}

static void out(const char *s)
{
    while (s && *s)
    {
        if (g_capture)
        {
            if (g_caplen < (int)sizeof(g_capbuf) - 1) g_capbuf[g_caplen++] = *s;
        }
        else
        {
            if (g_buflen >= (int)sizeof(g_buf) - 1) out_flush();
            g_buf[g_buflen++] = *s;
        }
        s++;
    }
}

static void outln(const char *s) { out(s); out("\r\n"); }

/* ===========================================================================
   4) formatovanie cisel (bez libc)
   =========================================================================== */
static void u64_to_dec(U64 v, char *dst)
{
    static const U64 p10[20] = {
        10000000000000000000ULL, 1000000000000000000ULL, 100000000000000000ULL,
        10000000000000000ULL, 1000000000000000ULL, 100000000000000ULL,
        10000000000000ULL, 1000000000000ULL, 100000000000ULL, 10000000000ULL,
        1000000000ULL, 100000000ULL, 10000000ULL, 1000000ULL,
        100000ULL, 10000ULL, 1000ULL, 100ULL, 10ULL, 1ULL };
    int i = 0, started = 0, k;

    for (k = 0; k < 20; k++)
    {
        U64 p = p10[k];
        int d = 0;
        while (v >= p) { v -= p; d++; }
        if (d || started || p == 1) { dst[i++] = (char)('0' + d); started = 1; }
    }
    dst[i] = 0;
}

static void i64_to_dec(signed long long v, char *dst)
{
    if (v < 0) { dst[0] = '-'; u64_to_dec((U64)(0 - (U64)v), dst + 1); }
    else u64_to_dec((U64)v, dst);
}

static void u64_to_hex(U64 v, char *dst, int minDigits)
{
    const char *hex = "0123456789ABCDEF";
    char tmp[17];
    int n = 0, i;

    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = hex[(int)(v & 0xF)]; v >>= 4; }
    while (n < minDigits && n < 16) tmp[n++] = '0';

    for (i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    dst[n] = 0;
}

/* --- realne cislo -> text (bez sprintf). ~7 platnych cifier --------------- */
static void dbl_to_dec(double v, char *dst)
{
    char *p = dst;
    int i, neg = 0;
    double w, frac;
    static const double p10[19] = {
        1e18, 1e17, 1e16, 1e15, 1e14, 1e13, 1e12, 1e11, 1e10,
        1e9, 1e8, 1e7, 1e6, 1e5, 1e4, 1e3, 1e2, 1e1, 1.0 };

    if (v != v) { s_cpy(dst, "NaN", 8); return; }         /* NaN */
    if (v < 0) { neg = 1; v = -v; }

    /* prilis velke / male -> vedecky zapis */
    if (v >= 1e15 || (v > 0 && v < 1e-6))
    {
        int e = 0;
        if (v > 0)
        {
            while (v >= 10.0) { v /= 10.0; e++; }
            while (v < 1.0)   { v *= 10.0; e--; }
        }
        if (neg) *p++ = '-';
        /* jedna cifra + 6 desatinnych */
        *p++ = (char)('0' + (int)v);
        frac = v - (double)(int)v;
        *p++ = '.';
        for (i = 0; i < 6; i++)
        {
            frac *= 10.0;
            *p++ = (char)('0' + (int)frac);
            frac -= (double)(int)frac;
        }
        *p++ = 'e';
        if (e < 0) { *p++ = '-'; e = -e; } else *p++ = '+';
        if (e >= 100) { *p++ = (char)('0' + e / 100); e = e % 100; }
        *p++ = (char)('0' + e / 10);
        *p++ = (char)('0' + e % 10);
        *p = 0;
        return;
    }

    if (neg) *p++ = '-';

    /* celociselna cast - bez konverzie double->int64 (ta potrebuje libgcc) */
    w = 0.0;
    for (i = 0; i < 19; i++)
    {
        if (v >= p10[i])
        {
            int d = 0;
            while (v >= p10[i]) { v -= p10[i]; d++; w = 1.0; }
            *p++ = (char)('0' + d);
        }
        else if (w != 0.0) *p++ = '0';
    }
    if (w == 0.0) *p++ = '0';

    /* desatinna cast */
    frac = v;
    if (frac > 0.0000005)
    {
        int lastNonZero = 0;
        char *fs = p;
        *p++ = '.';
        frac += 0.0000005;                       /* zaokruhlenie */
        for (i = 0; i < 6; i++)
        {
            frac *= 10.0;
            *p = (char)('0' + (int)frac);
            if (*p != '0') lastNonZero = i + 1;
            p++;
            frac -= (double)(int)frac;
        }
        p = fs + 1 + lastNonZero;                /* odstran nuly na konci */
    }
    *p = 0;
}

/* --- casova peciatka HH:MM:SS.mmm (bez libc) ------------------------------ */
static void app2(char *dst, int max, U32 v)
{
    char n[8];
    u64_to_dec((U64)v, n);
    if (v < 10) s_cat(dst, "0", max);
    s_cat(dst, n, max);
}

static void timestamp(char *dst, int max)
{
    T_SYSTIME st;
    char n[8];

    GetLocalTime(&st);
    dst[0] = 0;
    app2(dst, max, st.wHour);   s_cat(dst, ":", max);
    app2(dst, max, st.wMinute); s_cat(dst, ":", max);
    app2(dst, max, st.wSecond); s_cat(dst, ".", max);
    u64_to_dec((U64)st.wMilliseconds, n);
    if (st.wMilliseconds < 100) s_cat(dst, "0", max);
    if (st.wMilliseconds < 10)  s_cat(dst, "0", max);
    s_cat(dst, n, max);
}

/* --- parsovanie ----------------------------------------------------------- */

/* cislo v tvare 123 / -5 / 0x1F / 16#FF / 8#77 / 2#1010 */
static int parse_u64(const char *s, U64 *out)
{
    U64 v = 0;
    int base = 10, neg = 0, digits = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '1' && s[1] == '6' && s[2] == '#') { base = 16; s += 3; }
    else if (s[0] == '2' && s[1] == '#') { base = 2;  s += 2; }
    else if (s[0] == '8' && s[1] == '#') { base = 8;  s += 2; }

    while (*s)
    {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (U64)base + (U64)d;
        digits++;
        s++;
    }
    if (!digits) return 0;
    *out = neg ? (U64)(0 - v) : v;
    return 1;
}

static int parse_dbl(const char *s, double *out)
{
    double v = 0.0, scale = 1.0;
    int neg = 0, digits = 0, eneg = 0, e = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    while (is_digit(*s)) { v = v * 10.0 + (double)(*s - '0'); digits++; s++; }
    if (*s == '.' || *s == ',')
    {
        s++;
        while (is_digit(*s))
        {
            scale /= 10.0;
            v += scale * (double)(*s - '0');
            digits++;
            s++;
        }
    }
    if (!digits) return 0;

    if (*s == 'e' || *s == 'E')
    {
        s++;
        if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        while (is_digit(*s)) { e = e * 10 + (*s - '0'); s++; }
        while (e-- > 0) { if (eneg) v /= 10.0; else v *= 10.0; }
    }
    *out = neg ? -v : v;
    return 1;
}

__declspec(dllimport) U32 PVIAPI GetFileAttributesA(const char *path);

/* ===========================================================================
   5) PVI / PviCom.dll - dynamicke nacitanie a volanie
   =========================================================================== */
#define PVI_HMSG_NIL     1
#define SET_PVIFUNCTION  0xfffffffc

#define OBJ_PVI     0
#define OBJ_LINE    1
#define OBJ_DEVICE  2
#define OBJ_STATION 3
#define OBJ_CPU     4
#define OBJ_MODULE  5
#define OBJ_TASK    6
#define OBJ_PVAR    7

#define ACC_VERSION       2
#define ACC_DATA         11
#define ACC_STATUS       12
#define ACC_TYPE_EXTERN  14
#define ACC_TYPE_INTERN  19
#define ACC_LIST_PVAR    36
#define ACC_INFO_LICENCE 200

#define EV_PVI_CONNECT 240
#define EV_PVI_ARRANGE 242

/* popis typu premennej (VT/VL/VN) */
typedef struct { char vt[24]; int vl; int vn; char at[8]; } TVarType;

/* dopredne deklaracie */
static int file_exists(const char *p);
static int try_dir(const char *base, const char *dll);
static int read_value(U32 linkId, TVarType *t, int timeoutMs, int raw);
static void out_err(const char *s);
static void out_errln(const char *s);

typedef I32 (PVIAPI *FN_INIT)(U32 *phPvi, I32 timeout, I32 retry, const char *param, void *res);
typedef I32 (PVIAPI *FN_DEINIT)(U32 hPvi);
typedef I32 (PVIAPI *FN_SETEV)(U32 hPvi, U32 evType, void *hMsg, U32 msgNo, long param);
typedef I32 (PVIAPI *FN_CREATE)(U32 hPvi, U32 *linkId, const char *name, U32 objType,
                                const char *descr, void *hMsg, U32 msgNo, long param,
                                const char *linkDescr);
typedef I32 (PVIAPI *FN_UNLINK)(U32 hPvi, U32 linkId);
typedef I32 (PVIAPI *FN_READ)(U32 hPvi, U32 linkId, U32 acc, void *argData, I32 argLen,
                              void *data, I32 dataLen);
typedef I32 (PVIAPI *FN_WRITE)(U32 hPvi, U32 linkId, U32 acc, void *data, I32 dataLen,
                               void *rstData, I32 rstLen);
typedef I32 (PVIAPI *FN_NEXTRESP)(U32 hPvi, U32 *pwParam, long *plParam, void **phMsg, void *hEvent);
typedef I32 (PVIAPI *FN_RESPINFO)(U32 hPvi, U32 wParam, long *pParam, U32 *pDataLen,
                                  void *pInfo, U32 infoLen);
typedef I32 (PVIAPI *FN_READRESP)(U32 hPvi, U32 wParam, void *data, I32 dataLen);

static FN_INIT      f_init;
static FN_DEINIT    f_deinit;
static FN_SETEV     f_setev;
static FN_CREATE    f_create;
static FN_UNLINK    f_unlink;
static FN_READ      f_read;
static FN_WRITE     f_write;
static FN_NEXTRESP  f_nextresp;
static FN_RESPINFO  f_respinfo;
static FN_READRESP  f_readresp;

static U32  g_hPvi;
static char g_dllpath[512];
static int  g_verbose;
static int  g_nopause;
static char g_taskbuf[128];
static char g_varbuf[256];

typedef struct { U32 linkID, nMode, nType, errCode, status; } T_RESPONSE_INFO;
typedef struct { U8 state0, state1, inv0, inv1; U32 res1; char lcName[65]; } T_LIC;

static HPVI_OBJ g_lastmod;
static int      g_api_missing;
static char     g_missing[300];

static void miss(const char *name)
{
    if (g_missing[0]) s_cat(g_missing, ", ", (int)sizeof(g_missing));
    s_cat(g_missing, name, (int)sizeof(g_missing));
}

static int resolve(HPVI_OBJ mod, const char *path)
{
    g_lastmod = mod;
    g_missing[0] = 0;

    f_init     = (FN_INIT)     GetProcAddress(mod, "PviXInitialize");
    f_deinit   = (FN_DEINIT)   GetProcAddress(mod, "PviXDeinitialize");
    f_setev    = (FN_SETEV)    GetProcAddress(mod, "PviXSetGlobEventMsg");
    f_create   = (FN_CREATE)   GetProcAddress(mod, "PviXCreate");
    f_unlink   = (FN_UNLINK)   GetProcAddress(mod, "PviXUnlink");
    f_read     = (FN_READ)     GetProcAddress(mod, "PviXRead");
    f_write    = (FN_WRITE)    GetProcAddress(mod, "PviXWrite");
    f_nextresp = (FN_NEXTRESP) GetProcAddress(mod, "PviXGetNextResponse");
    f_respinfo = (FN_RESPINFO) GetProcAddress(mod, "PviXGetResponseInfo");
    f_readresp = (FN_READRESP) GetProcAddress(mod, "PviXReadResponse");

    s_cpy(g_dllpath, path, (int)sizeof(g_dllpath));

    if (!f_init)     miss("PviXInitialize");
    if (!f_deinit)   miss("PviXDeinitialize");
    if (!f_setev)    miss("PviXSetGlobEventMsg");
    if (!f_create)   miss("PviXCreate");
    if (!f_unlink)   miss("PviXUnlink");
    if (!f_read)     miss("PviXRead");
    if (!f_write)    miss("PviXWrite");
    if (!f_nextresp) miss("PviXGetNextResponse");
    if (!f_respinfo) miss("PviXGetResponseInfo");
    if (!f_readresp) miss("PviXReadResponse");

    g_api_missing = (g_missing[0] != 0);
    return !g_api_missing;
}

/* Vypise exporty nacitanej DLL (v in-process PE), aby bolo vidiet, ake API to je. */
static void dump_exports(HPVI_OBJ mod, int maxShow)
{
    U8 *base = (U8 *)mod;
    U32 pe, opt, expRva, nNames, addrNames, i, shown = 0;
    char num[16];

    if (!base || base[0] != 'M' || base[1] != 'Z')
    {
        out_errln("  (nie je platna PE kniznica)");
        return;
    }
    pe = *(U32 *)(base + 0x3C);
    if (*(U32 *)(base + pe) != 0x00004550u)
    {
        out_errln("  (nie je platna PE kniznica)");
        return;
    }
    opt = pe + 24;
    expRva = *(U32 *)(base + opt + 96);
    if (!expRva)
    {
        out_errln("  DLL nema ziadne exporty -> toto nie je klientska kniznica PVI");
        return;
    }

    nNames    = *(U32 *)(base + expRva + 24);
    addrNames = *(U32 *)(base + expRva + 32);

    out_err("  DLL exportuje ");
    u64_to_dec((U64)nNames, num);
    out_err(num);
    out_errln(" funkcii; tie zacinajuce na 'Pvi':");

    for (i = 0; i < nNames && shown < (U32)maxShow; i++)
    {
        const char *nm = (const char *)(base + (*(U32 *)(base + addrNames + i * 4)));
        if (nm[0] == 'P' && nm[1] == 'v' && nm[2] == 'i')
        {
            out_err("    ");
            out_errln(nm);
            shown++;
        }
    }
    if (shown == 0) out_errln("    (ziadna - toto nebude PVI klientska kniznica)");
}

static int file_exists(const char *p) { return GetFileAttributesA(p) != 0xFFFFFFFFu; }

static int load_pvi(const char *forced)
{
    static const char *dlls[2] = { "PviCom.dll", "Pvi6Com.dll" };
    char buf[512], *p;
    HPVI_OBJ key;
    U32 cb, type;
    int d;

    /* 1) --dll <cesta> */
    if (forced && forced[0])
    {
        HPVI_OBJ mod = LoadLibraryA(forced);
        if (mod && resolve(mod, forced)) return 1;
        return 0;
    }

    /* 2) %PVI_INSTALL_FOLDER% */
    if (GetEnvironmentVariableA("PVI_INSTALL_FOLDER", buf, (U32)sizeof(buf)) > 0)
        for (d = 0; d < 2; d++) if (try_dir(buf, dlls[d])) return 1;

    /* 3) registry: App Paths\PviMan.exe (PVI 4) / Pvi6Man.exe (PVI 6) -> "Path" */
    {
        static const char *keys[2] = {
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\PviMan.exe",
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Pvi6Man.exe" };
        int k;
        for (k = 0; k < 2; k++)
        {
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keys[k], 0, 0x20019, &key) != 0) continue;
            cb = (U32)sizeof(buf);
            if (RegQueryValueExA(key, "Path", 0, &type, (U8 *)buf, &cb) != 0 || !buf[0])
            {
                RegCloseKey(key);
                continue;
            }
            RegCloseKey(key);

            p = buf;
            if (*p == '"') p++;
            {
                int n = s_len(p);
                while (n > 0 && (p[n - 1] == '"' || p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = 0;
                while (n > 0 && p[n - 1] != '\\') p[--n] = 0;   /* zahod meno suboru */
                if (n > 0) p[n - 1] = 0;                        /* zahod koncove '\' */
            }
            for (d = 0; d < 2; d++) if (try_dir(p, dlls[d])) return 1;
        }
    }

    /* 4) caste instalacne cesty */
    {
        static const char *bases[10] = {
            "C:\\BrAutomation\\PVI", "C:\\BrAutomation\\PVI\\V4.12",
            "C:\\BrAutomation\\PVI\\V4.11", "C:\\BrAutomation\\PVI\\V4.10",
            "C:\\BrAutomation\\PVI\\V4.3", "C:\\BrAutomation\\PVI\\V4.5",
            "C:\\Program Files\\BrAutomation\\PVI",
            "C:\\Program Files (x86)\\BrAutomation\\PVI",
            "C:\\BrAutomation\\AS40\\PVI", "C:\\BrAutomation\\AS4120\\PVI" };
        int b;
        for (b = 0; b < 10; b++)
            for (d = 0; d < 2; d++) if (try_dir(bases[b], dlls[d])) return 1;
    }

    /* 5) podla mena (PATH / aktualny priecinok) */
    for (d = 0; d < 2; d++)
    {
        HPVI_OBJ mod = LoadLibraryA(dlls[d]);
        if (mod && resolve(mod, dlls[d])) return 1;
    }
    return 0;
}

/* ---- spracovanie fronty odpovedi/eventov PVI ----------------------------- */
static int pump(int wantType, int timeoutMs)
{
    int waited = 0;
    U32 wParam = 0;
    long lParam = 0;
    void *hMsg = 0;
    T_RESPONSE_INFO ri;

    for (;;)
    {
        wParam = 0;
        if (f_nextresp(g_hPvi, &wParam, &lParam, &hMsg, 0) == 0 && wParam != 0)
        {
            U32 dataLen = 0;
            mem_zero(&ri, sizeof(ri));
            if (f_respinfo(g_hPvi, wParam, 0, &dataLen, &ri, (U32)sizeof(ri)) == 0)
            {
                if (g_verbose)
                {
                    char n[24];
                    u64_to_dec((U64)ri.nType, n);
                    out("  event/response type=");
                    out(n);
                    out(" err=");
                    u64_to_dec((U64)ri.errCode, n);
                    outln(n);
                }
                if (wantType > 0 && (int)ri.nType == wantType)
                {
                    f_readresp(g_hPvi, wParam, 0, 0);
                    return 1;
                }
            }
            f_readresp(g_hPvi, wParam, 0, 0);      /* uvolni spravu */
        }

        if (timeoutMs <= 0) return 0;
        if (waited >= timeoutMs) return 0;
        Sleep(50);
        waited += 50;
    }
}

/* ---- pomocnici na tvorbu PVI objektov ----------------------------------- */
static void descr_add(char *buf, int max, const char *key, const char *val)
{
    int i, quote = 0;

    for (i = 0; val[i]; i++)
        if (val[i] == '/' || val[i] == '.' || val[i] == ' ' || val[i] == '\\') { quote = 1; break; }

    if (buf[0]) s_cat(buf, " ", max);
    s_cat(buf, key, max);
    s_cat(buf, "=", max);
    if (quote) s_cat(buf, "\"", max);
    s_cat(buf, quote ? val : val, max);
    if (quote) s_cat(buf, "\"", max);
}

/* meno objektu = <meno rodica>/<meno> */
static void obj_name(char *dst, int max, const char *parent, const char *name)
{
    s_cpy(dst, parent, max);
    s_cat(dst, "/", max);
    s_cat(dst, name, max);
}

static I32 pvi_create(U32 objType, const char *name, const char *descr,
                      const char *linkDescr, U32 *linkId)
{
    U32 id = 0;
    I32 rc;

    if (g_verbose)
    {
        out("  create type=");
        { char n[16]; u64_to_dec((U64)objType, n); out(n); }
        out(" name=");
        out(name);
        out(" descr=\"");
        out(descr);
        outln("\"");
    }

    rc = f_create(g_hPvi, &id, name, objType, descr, (void *)PVI_HMSG_NIL,
                  SET_PVIFUNCTION, 0, linkDescr);
    if (rc == 0 && linkId) *linkId = id;
    return rc;
}

/* ---- stderr (diagnostika, aby stdout ostal ciste pre hodnotu) ------------ */
#define STD_ERROR_HANDLE ((U32)-12)

static void out_err(const char *s)
{
    static char b[512];
    static int n;
    HPVI_OBJ h;
    U32 written = 0;

    while (s && *s)
    {
        if (n >= (int)sizeof(b) - 1) { h = GetStdHandle(STD_ERROR_HANDLE); WriteFile(h, b, (U32)n, &written, 0); n = 0; }
        b[n++] = *s++;
    }
}

static void out_errln(const char *s) { out_err(s); out_err("\r\n"); }

/* ---- typ premennej ------------------------------------------------------- */

static char g_typebuf[65536];
static U8   g_databuf[65536];
static char g_listbuf[524288];

/* najdi podretazec (case-insensitive); vrati 1 ak hay obsahuje needle */
static int s_contains_icmp(const char *hay, const char *needle)
{
    int i, j;
    if (!needle || !needle[0]) return 1;
    for (i = 0; hay[i]; i++)
    {
        for (j = 0; needle[j]; j++)
            if (ch_low(hay[i + j]) != ch_low(needle[j])) break;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* najdi v typovom popise polozku "XX=hodnota" (napr. VT=i32) */
static int type_field(const char *s, const char *key, char *outp, int max)
{
    int i = 0;

    while (s[i])
    {
        if (s[i] >= 'A' && s[i] <= 'Z' && s[i + 1] >= 'A' && s[i + 1] <= 'Z' && s[i + 2] == '=')
        {
            if (s[i] == key[0] && s[i + 1] == key[1])
            {
                int j = 0;
                i += 3;
                while (s[i] && s[i] != ' ' && s[i] != '}' && j < max - 1) outp[j++] = s[i++];
                outp[j] = 0;
                return 1;
            }
        }
        i++;
    }
    return 0;
}

static int field_int(const char *s, const char *key, int def)
{
    char num[24];
    U64 v;
    if (!type_field(s, key, num, (int)sizeof(num))) return def;
    if (!parse_u64(num, &v)) return def;
    return (int)v;
}

/* precitaj typ premennej z PLC (opakuje, kym sa CPU nespoji) */
static int read_var_type(U32 linkId, TVarType *t, int timeoutMs)
{
    int waited = 0;
    char num[24];

    for (;;)
    {
        I32 rc = f_read(g_hPvi, linkId, ACC_TYPE_EXTERN, 0, 0, g_typebuf, (I32)sizeof(g_typebuf) - 1);
        if (rc == 0)
        {
            s_cpy(t->vt, "?", (int)sizeof(t->vt));
            type_field(g_typebuf, "VT", t->vt, (int)sizeof(t->vt));
            s_cpy(t->at, "", (int)sizeof(t->at));
            type_field(g_typebuf, "AT", t->at, (int)sizeof(t->at));
            t->vl = field_int(g_typebuf, "VL", 0);
            t->vn = field_int(g_typebuf, "VN", 1);
            if (t->vn < 1) t->vn = 1;

            /* skutocnu dlzku dat dava POBJ_ACC_TYPE_INTERN */
            mem_zero(g_typebuf, sizeof(g_typebuf));
            if (f_read(g_hPvi, linkId, ACC_TYPE_INTERN, 0, 0, g_typebuf,
                       (I32)sizeof(g_typebuf) - 1) == 0)
            {
                int v = field_int(g_typebuf, "VL", 0);
                if (v > 0) t->vl = v;
                v = field_int(g_typebuf, "VN", 0);
                if (v > 0) t->vn = v;
            }

            if (g_verbose)
            {
                out_err("  typ: VT=");
                out_err(t->vt);
                out_err(" VL=");
                u64_to_dec((U64)t->vl, num); out_err(num);
                out_err(" VN=");
                u64_to_dec((U64)t->vn, num); out_err(num);
                out_err(" AT=");
                out_errln(t->at);
            }
            return 1;
        }

        pump(0, 0);                       /* vycisti frontu eventov */
        if (waited >= timeoutMs) return 0;
        Sleep(100);
        waited += 100;
    }
}

static U64 get_le(const U8 *p, int n)
{
    U64 v = 0;
    int i;
    for (i = n - 1; i >= 0; i--) v = (v << 8) | (U64)p[i];
    return v;
}

static void put_le(U8 *p, int n, U64 v)
{
    int i;
    for (i = 0; i < n; i++) { p[i] = (U8)(v & 0xFF); v >>= 8; }
}

static void print_hex(const U8 *d, int len)
{
    char tmp[8];
    int i;
    for (i = 0; i < len; i++)
    {
        u64_to_hex((U64)d[i], tmp, 2);
        out(tmp);
        if ((i & 15) == 15 && i + 1 < len) out(" ");
    }
}

/* vypis jednej hodnoty daneho typu */
static void print_one(const TVarType *t, const U8 *d, int esz)
{
    char num[64];

    if (s_icmp(t->vt, "boolean") == 0)  { out(d[0] ? "TRUE" : "FALSE"); }
    else if (s_icmp(t->vt, "i8") == 0)  { i64_to_dec((signed long long)(signed char)d[0], num); out(num); }
    else if (s_icmp(t->vt, "i16") == 0) { i64_to_dec((signed long long)(short)get_le(d, 2), num); out(num); }
    else if (s_icmp(t->vt, "i32") == 0) { i64_to_dec((signed long long)(int)get_le(d, 4), num); out(num); }
    else if (s_icmp(t->vt, "i64") == 0) { i64_to_dec((signed long long)get_le(d, 8), num); out(num); }
    else if (s_icmp(t->vt, "u8") == 0)  { u64_to_dec(get_le(d, 1), num); out(num); }
    else if (s_icmp(t->vt, "u16") == 0) { u64_to_dec(get_le(d, 2), num); out(num); }
    else if (s_icmp(t->vt, "u32") == 0) { u64_to_dec(get_le(d, 4), num); out(num); }
    else if (s_icmp(t->vt, "u64") == 0) { u64_to_dec(get_le(d, 8), num); out(num); }
    else if (s_icmp(t->vt, "dt") == 0 || s_icmp(t->vt, "date") == 0 ||
             s_icmp(t->vt, "tod") == 0 || s_icmp(t->vt, "time") == 0)
    {
        u64_to_dec(get_le(d, 4), num);
        out(num);
        out(" (");
        out(t->vt);
        out(")");
    }
    else if (s_icmp(t->vt, "f32") == 0)
    {
        union { U32 u; float f; } c;
        c.u = (U32)get_le(d, 4);
        dbl_to_dec((double)c.f, num);
        out(num);
    }
    else if (s_icmp(t->vt, "f64") == 0)
    {
        union { U64 u; double d; } c;
        c.u = get_le(d, 8);
        dbl_to_dec(c.d, num);
        out(num);
    }
    else if (s_icmp(t->vt, "string") == 0)
    {
        int i;
        out("\"");
        for (i = 0; i < esz && d[i]; i++) { char c[2]; c[0] = (char)d[i]; c[1] = 0; out(c); }
        out("\"");
    }
    else if (s_icmp(t->vt, "wstring") == 0)
    {
        int i;
        out("\"");
        for (i = 0; i + 1 < esz; i += 2)
        {
            char c[2];
            U16 ch = (U16)get_le(d + i, 2);
            if (!ch) break;
            c[0] = (ch < 128) ? (char)ch : '?';
            c[1] = 0;
            out(c);
        }
        out("\"");
    }
    else print_hex(d, esz);
}

/* zapis jednej hodnoty daneho typu do buffra */
static int put_one(const TVarType *t, U8 *d, int esz, const char *text)
{
    U64 iv;
    double dv;

    if (s_icmp(t->vt, "boolean") == 0)
    {
        int b;
        if (s_icmp(text, "true") == 0 || s_cmp(text, "1") == 0) b = 1;
        else if (s_icmp(text, "false") == 0 || s_cmp(text, "0") == 0) b = 0;
        else { if (!parse_u64(text, &iv)) return 0; b = (iv != 0); }
        d[0] = (U8)b;
        return 1;
    }
    if (s_icmp(t->vt, "string") == 0)
    {
        int i = 0;
        while (text[i] && i < esz - 1) { d[i] = (U8)text[i]; i++; }
        d[i] = 0;
        return 1;
    }
    if (s_icmp(t->vt, "wstring") == 0)
    {
        int i = 0, o = 0;
        while (text[i] && o + 1 < esz) { d[o] = (U8)text[i]; d[o + 1] = 0; i++; o += 2; }
        return 1;
    }
    if (s_icmp(t->vt, "f32") == 0)
    {
        union { U32 u; float f; } c;
        if (!parse_dbl(text, &dv)) return 0;
        c.f = (float)dv;
        put_le(d, 4, (U64)c.u);
        return 1;
    }
    if (s_icmp(t->vt, "f64") == 0)
    {
        union { U64 u; double d; } c;
        if (!parse_dbl(text, &dv)) return 0;
        c.d = dv;
        put_le(d, 8, c.u);
        return 1;
    }
    /* celociselne typy (+ dt/date/tod/time ako u32) */
    if (!parse_u64(text, &iv))
    {
        if (!parse_dbl(text, &dv)) return 0;
        /* desatinne cislo do celociselneho typu: pretypujeme cez delenie 1 */
        if (dv < 0) { U64 a = (U64)(0.0 - dv); iv = (U64)(0 - a); }
        else
        {
            U64 a = 0;
            while (dv >= 1.0 && a < 0xFFFFFFFFFFFFFFFFULL) { dv -= 1.0; a++; }
            iv = a;
        }
    }

    if (s_icmp(t->vt, "i8") == 0)       put_le(d, 1, iv);
    else if (s_icmp(t->vt, "u8") == 0)  put_le(d, 1, iv);
    else if (s_icmp(t->vt, "i16") == 0) put_le(d, 2, iv);
    else if (s_icmp(t->vt, "u16") == 0) put_le(d, 2, iv);
    else if (s_icmp(t->vt, "i32") == 0) put_le(d, 4, iv);
    else if (s_icmp(t->vt, "u32") == 0) put_le(d, 4, iv);
    else if (s_icmp(t->vt, "i64") == 0) put_le(d, 8, iv);
    else if (s_icmp(t->vt, "u64") == 0) put_le(d, 8, iv);
    else if (s_icmp(t->vt, "dt") == 0 || s_icmp(t->vt, "date") == 0 ||
             s_icmp(t->vt, "tod") == 0 || s_icmp(t->vt, "time") == 0) put_le(d, 4, iv);
    else return 0;

    return 1;
}

static int write_value(U32 linkId, const char *text, int timeoutMs)
{
    TVarType t;
    static U8 buf[65536];
    int esz, len, i;
    I32 rc;

    if (!read_var_type(linkId, &t, timeoutMs))
    {
        out_errln("chyba: nepodarilo sa precitat typ premennej (PLC neodpoveda?)");
        return 0;
    }
    if (s_icmp(t.vt, "struct") == 0 || s_icmp(t.vt, "data") == 0)
    {
        out_errln("chyba: zapis struktury nie je podporovany (zapis jednotlivy member)");
        return 0;
    }

    esz = (t.vl > 0) ? t.vl : 4;
    len = esz * t.vn;
    if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
    mem_zero(buf, (U32)len);

    /* rovnaku hodnotu zapiseme do vsetkych prvkov pola (ako Pvi.py) */
    for (i = 0; i < t.vn; i++)
    {
        if (!put_one(&t, buf + i * esz, esz, text))
        {
            out_err("chyba: neda sa previest hodnota na typ ");
            out_errln(t.vt);
            return 0;
        }
    }

    rc = f_write(g_hPvi, linkId, ACC_DATA, buf, (I32)len, 0, 0);
    if (rc != 0)
    {
        char n[24];
        out_err("chyba: PviXWrite(DATA) = ");
        u64_to_dec((U64)(U32)rc, n);
        out_errln(n);
        return 0;
    }
    return 1;
}

/* ===========================================================================
   6) hlavny program
   =========================================================================== */
static void usage(void)
{
    outln("pvi_cli - konzolovy PVI klient pre PLC B&R (bezi aj na Windows XP)");
    outln("");
    outln("Pouzitie:");
    outln("  pvi_cli info");
    outln("  pvi_cli list  [--filter <text>]");
    outln("  pvi_cli read  <premenna>");
    outln("  pvi_cli write <premenna> <hodnota>");
    outln("  pvi_cli m     <premenna> [ms]      sleduj zmeny (Ctrl+C = koniec)");
    outln("");
    outln("Parametre:");
    outln("  --filter <text>   len premenne obsahujuce text (pre 'list')");
    outln("  --ip <adresa>     IP adresa PLC (default 192.168.1.10)");
    outln("  --proto ansl|ina  protokol: ansl (AR 4.08+) alebo ina (starsi AR)");
    outln("  --station <n>     INA: cielova stanica PLC  /DA=  (default 2)");
    outln("  --src <n>         INA: stanica PC          /SA=  (default 113)");
    outln("  --com <n>         INA po serii: cislo COM portu (napr. 1)");
    outln("  --baud <n>        INA po serii: prenosova rychlost (default 57600)");
    outln("  --cpu <nazov>     nazov CPU objektu v PVI (default: cpu)");
    outln("  --task <nazov>    ak je premenna lokalna v ulohe (task)");
    outln("  --dll <cesta>     vynuti konkretny PviCom.dll");
    outln("  --timeout <s>     ako dlho cakat na spojenie (default 5 s)");
    outln("  --raw             vypis dat ako HEX");
    outln("  --verbose         diagnostika na stderr");
    outln("  --nopause         nezastavuj sa na konci (pre skripty)");
    outln("  --help            tento text");
    outln("");
    outln("Hodnota:  123   -5   1.5   TRUE   text   16#FF   0x1F   2#1010");
    outln("");
    outln("Navratove kody: 0 ok, 1 parametre, 2 nenajdena DLL, 3 init,");
    outln("                4 vytvorenie objektov, 5 citanie, 6 zapis");
}

static int split_cmdline(char *s, char **argv, int maxArgs)
{
    int argc = 0, inQuotes = 0;
    char *p = s;

    while (*p && argc < maxArgs)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"') { inQuotes = 1; p++; }
        argv[argc++] = p;

        if (inQuotes)
        {
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
            inQuotes = 0;
        }
        else
        {
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    return argc;
}

static const char *license_state(U8 s)
{
    if (s == 0) return "undefined";
    if (s == 1) return "TRIAL (2 h od startu!)";
    if (s == 2) return "runtime";
    if (s == 3) return "developer";
    if (s == 4) return "LOCKED";
    return "?";
}

static int run(int argc, char **argv)
{
    const char *cmd = 0, *var = 0, *val = 0, *dll = 0;
    const char *ip = "192.168.1.10", *proto = "ansl";
    const char *cpuName = "cpu", *taskName = 0, *filter = 0;
    int station = 2, srcStation = 113, comPort = 0, baud = 57600;
    int timeout = 5, raw = 0, ina, i;
    char ifstr[160], nmRoot[64], nmLine[128], nmDev[256], nmCpu[384];
    char nmTask[448], nmVar[512], dsc[256], num[24];
    U32 idRoot = 0, idLine = 0, idDev = 0, idCpu = 0, idTask = 0, idVar = 0;
    TVarType t;
    I32 prc;

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        U64 v;

        if (s_icmp(a, "--help") == 0 || s_cmp(a, "-h") == 0)  { usage(); return 0; }
        else if (s_icmp(a, "--ip") == 0 && i + 1 < argc)      ip = argv[++i];
        else if (s_icmp(a, "--proto") == 0 && i + 1 < argc)   proto = argv[++i];
        else if (s_icmp(a, "--cpu") == 0 && i + 1 < argc)     cpuName = argv[++i];
        else if (s_icmp(a, "--task") == 0 && i + 1 < argc)    taskName = argv[++i];
        else if (s_icmp(a, "--dll") == 0 && i + 1 < argc)     dll = argv[++i];
        else if (s_icmp(a, "--filter") == 0 && i + 1 < argc)  filter = argv[++i];
        else if (s_icmp(a, "--station") == 0 && i + 1 < argc) { parse_u64(argv[++i], &v); station = (int)v; }
        else if (s_icmp(a, "--src") == 0 && i + 1 < argc)     { parse_u64(argv[++i], &v); srcStation = (int)v; }
        else if (s_icmp(a, "--com") == 0 && i + 1 < argc)     { parse_u64(argv[++i], &v); comPort = (int)v; }
        else if (s_icmp(a, "--baud") == 0 && i + 1 < argc)    { parse_u64(argv[++i], &v); baud = (int)v; }
        else if (s_icmp(a, "--timeout") == 0 && i + 1 < argc) { parse_u64(argv[++i], &v); timeout = (int)v; }
        else if (s_icmp(a, "--raw") == 0)      raw = 1;
        else if (s_icmp(a, "--verbose") == 0)  g_verbose = 1;
        else if (s_icmp(a, "--nopause") == 0)  g_nopause = 1;
        else if (a[0] == '-' && a[1] == '-')   { out_err("neznama volba: "); out_errln(a); return 1; }
        else if (!cmd) cmd = a;
        else if (!var) var = a;
        else if (!val) val = a;
    }

    if (!cmd) { usage(); return 1; }

    /* podpora zapisu "task:premenna" (rovnake ako --task) */
    if (var && !taskName)
    {
        int k, colon = -1;
        for (k = 0; var[k]; k++) if (var[k] == ':') { colon = k; break; }
        if (colon > 0)
        {
            int n = colon;
            if (n > (int)sizeof(g_taskbuf) - 1) n = (int)sizeof(g_taskbuf) - 1;
            for (k = 0; k < n; k++) g_taskbuf[k] = var[k];
            g_taskbuf[n] = 0;
            s_cpy(g_varbuf, var + colon + 1, (int)sizeof(g_varbuf));
            taskName = g_taskbuf;
            var = g_varbuf;
        }
    }

    if ((s_icmp(cmd, "read") == 0 || s_icmp(cmd, "m") == 0 || s_icmp(cmd, "mon") == 0 ||
         s_icmp(cmd, "monitor") == 0) && !var)
    { out_errln("chyba: chyba nazov premennej"); return 1; }
    if (s_icmp(cmd, "write") == 0 && (!var || !val))  { out_errln("chyba: chyba premenna alebo hodnota"); return 1; }
    if (s_icmp(cmd, "read") != 0 && s_icmp(cmd, "write") != 0 && s_icmp(cmd, "info") != 0 &&
        s_icmp(cmd, "list") != 0 && s_icmp(cmd, "m") != 0 &&
        s_icmp(cmd, "mon") != 0 && s_icmp(cmd, "monitor") != 0)
    { out_err("chyba: neznama operacia: "); out_errln(cmd); usage(); return 1; }

    if (timeout < 1) timeout = 1;
    timeout *= 1000;

    /* --- 1) PVI kniznica ------------------------------------------------ */
    if (!load_pvi(dll))
    {
        if (g_api_missing)
        {
            out_err("chyba: kniznica ");
            out_err(g_dllpath);
            out_errln(" sa nacitala, ale NEMA funkcie PviX*");
            out_err("  chybaju: ");
            out_errln(g_missing);
            dump_exports(g_lastmod, 40);
            out_errln("");
            out_errln("  -> toto je STARSIA verzia PVI (stare API: PviInitialize, PviCreate,");
            out_errln("     PviReadRequest, PviWriteRequest, ...). pvi_cli potrebuje PVI s API PviX*");
            out_errln("     (overene: PVI 4.12 a PVI 6.x - na tychto stoji aj kniznica Pvi.py).");
            out_errln("");
            out_errln("  Moznosti:");
            out_errln("   a) nainstalovat novsi PVI Development Setup (4.12 obsahuje aj INA2000):");
            out_errln("      https://www.br-automation.com/en/downloads/software/automation-studio/automation-studio-3090/pvi-development-setup/");
            out_errln("   b) pouzit .NET verziu (pvi-xp/src, build.cmd) s BR.AN.PviServices.dll, ktora");
            out_errln("      je sucastou TVOJEJ instalacie PVI - funguje aj so starsim PVI");
            out_errln("   c) ukazat na inu kniznicu:  --dll \"C:\\cesta\\k\\novejsiemu\\PviCom.dll\"");
            return 2;
        }

        out_errln("chyba: nepodarilo sa najst/nacitat PviCom.dll");
        out_errln("  - je PVI nainstalovane? (%PVI_INSTALL_FOLDER%)");
        out_errln("  - skus:  pvi_cli info --verbose --dll \"C:\\BrAutomation\\PVI\\V4.12\\Bin\\PviCom.dll\"");
        out_errln("  - kde ju hladat:  dir /s /b C:\\PviCom.dll");
        return 2;
    }
    if (g_verbose) { out_err("PVI kniznica: "); out_errln(g_dllpath); }

    /* --- 2) inicializacia PVI klienta ---------------------------------- */
    prc = f_init(&g_hPvi, timeout / 1000, 0, "", 0);
    if (prc != 0)
    {
        out_err("chyba: PviXInitialize = ");
        u64_to_dec((U64)(U32)prc, num);
        out_errln(num);
        out_errln("  -> bezi PviMan? (sluzba 'PviManager' alebo PviMan.exe)");
        return 3;
    }

    /* --- 3) globalne eventy + cakanie na 'arrange' --------------------- */
    f_setev(g_hPvi, EV_PVI_CONNECT, (void *)PVI_HMSG_NIL, SET_PVIFUNCTION, 0);
    f_setev(g_hPvi, 241,             (void *)PVI_HMSG_NIL, SET_PVIFUNCTION, 0);
    f_setev(g_hPvi, EV_PVI_ARRANGE, (void *)PVI_HMSG_NIL, SET_PVIFUNCTION, 0);
    pump(EV_PVI_ARRANGE, 3000);

    /* --- 4) objekty: root -> line -> device -> cpu [-> task] -> pvar ---- */
    ina = (s_icmp(proto, "ina") == 0);

    s_cpy(nmRoot, "@Pvi", (int)sizeof(nmRoot));
    prc = pvi_create(OBJ_PVI, nmRoot, "", "EV=ed ", &idRoot);
    if (prc != 0) goto create_failed;

    obj_name(nmLine, (int)sizeof(nmLine), nmRoot, ina ? "LNINA2" : "LNANSL");
    dsc[0] = 0;
    descr_add(dsc, (int)sizeof(dsc), "CD", ina ? "LNINA2" : "LNANSL");
    prc = pvi_create(OBJ_LINE, nmLine, dsc, "EV=ed ", &idLine);
    if (prc != 0) goto create_failed;

    /* interface (device) */
    if (comPort > 0)
    {
        s_cpy(ifstr, "/IF=com", (int)sizeof(ifstr));
        u64_to_dec((U64)comPort, num);
        s_cat(ifstr, num, (int)sizeof(ifstr));
        s_cat(ifstr, " /BAUD=", (int)sizeof(ifstr));
        u64_to_dec((U64)baud, num);
        s_cat(ifstr, num, (int)sizeof(ifstr));
    }
    else if (ina)
    {
        s_cpy(ifstr, "/IF=tcpip /SA=", (int)sizeof(ifstr));
        u64_to_dec((U64)srcStation, num);
        s_cat(ifstr, num, (int)sizeof(ifstr));
    }
    else s_cpy(ifstr, "/IF=TcpIp", (int)sizeof(ifstr));

    obj_name(nmDev, (int)sizeof(nmDev), nmLine, "TCP");
    dsc[0] = 0;
    descr_add(dsc, (int)sizeof(dsc), "CD", ifstr);
    prc = pvi_create(OBJ_DEVICE, nmDev, dsc, "EV=ed ", &idDev);
    if (prc != 0) goto create_failed;

    /* CPU */
    if (ina)
    {
        s_cpy(ifstr, "/DAIP=", (int)sizeof(ifstr));
        s_cat(ifstr, ip, (int)sizeof(ifstr));
        s_cat(ifstr, " /DA=", (int)sizeof(ifstr));
        u64_to_dec((U64)station, num);
        s_cat(ifstr, num, (int)sizeof(ifstr));
    }
    else
    {
        s_cpy(ifstr, "/IP=", (int)sizeof(ifstr));
        s_cat(ifstr, ip, (int)sizeof(ifstr));
    }

    obj_name(nmCpu, (int)sizeof(nmCpu), nmDev, cpuName);
    dsc[0] = 0;
    descr_add(dsc, (int)sizeof(dsc), "CD", ifstr);
    prc = pvi_create(OBJ_CPU, nmCpu, dsc, "EV=ep ", &idCpu);
    if (prc != 0) goto create_failed;

    if (g_verbose) { out_err("CPU: "); out_errln(ifstr); }

    /* volitelne: task (lokalne premenne) */
    if (taskName)
    {
        obj_name(nmTask, (int)sizeof(nmTask), nmCpu, taskName);
        dsc[0] = 0;
        descr_add(dsc, (int)sizeof(dsc), "CD", taskName);
        prc = pvi_create(OBJ_TASK, nmTask, dsc, "EV=ed ", &idTask);
        if (prc != 0) goto create_failed;
    }

    /* --- 5) premenna (registracia sa moze opakovat, kym sa CPU spoji) --- */
    if (var)
    {
        int waited = 0;

        obj_name(nmVar, (int)sizeof(nmVar), taskName ? nmTask : nmCpu, var);
        dsc[0] = 0;
        descr_add(dsc, (int)sizeof(dsc), "CD", var);
        descr_add(dsc, (int)sizeof(dsc), "RF", "0");

        for (;;)
        {
            prc = pvi_create(OBJ_PVAR, nmVar, dsc, "EV=ed ", &idVar);
            if (prc == 0) break;

            pump(0, 0);
            if (waited >= timeout) goto create_failed;
            Sleep(200);
            waited += 200;
        }
    }

    /* --- 6) operacia ---------------------------------------------------- */
    if (s_icmp(cmd, "info") == 0)
    {
        static T_LIC lic;
        char ver[128];

        mem_zero(&lic, (U32)sizeof(lic));
        if (f_read(g_hPvi, idRoot, ACC_INFO_LICENCE, 0, 0, &lic, (I32)sizeof(lic)) == 0)
        {
            out("licencia     : ");
            outln(license_state(lic.state0));
            out("licencne meno: ");
            outln(lic.lcName);
        }
        else out_errln("licenciu sa nepodarilo precitat");

        mem_zero(ver, (U32)sizeof(ver));
        if (f_read(g_hPvi, idRoot, ACC_VERSION, 0, 0, ver, (I32)sizeof(ver) - 1) == 0)
        {
            out("PVI verzia   : ");
            outln(ver);
        }
        out("kniznica     : ");
        outln(g_dllpath);
    }
    else if (s_icmp(cmd, "read") == 0)
    {
        if (!read_value(idVar, &t, timeout, raw)) goto read_failed;
        out("\r\n");
    }
    else if (s_icmp(cmd, "m") == 0 || s_icmp(cmd, "mon") == 0 || s_icmp(cmd, "monitor") == 0)
    {
        int iv = 200;
        U64 ivv;
        char last[1024];
        char stamp[32];

        if (val && parse_u64(val, &ivv) && ivv >= 10 && ivv <= 600000) iv = (int)ivv;
        last[0] = 0;

        if (g_verbose)
        {
            out_err("sledujem ");
            out_err(var);
            out_err(" kazdych ");
            u64_to_dec((U64)iv, num);
            out_err(num);
            out_errln(" ms (Ctrl+C = koniec)");
        }

        for (;;)
        {
            int ok;
            g_caplen = 0;
            g_capbuf[0] = 0;
            g_capture = 1;
            ok = read_value(idVar, &t, timeout, raw);
            g_capture = 0;
            if (!ok) goto read_failed;

            if (s_cmp(g_capbuf, last) != 0)          /* vypis len pri zmene */
            {
                timestamp(stamp, (int)sizeof(stamp));
                out(stamp);
                out("  ");
                out(var);
                out(" = ");
                out(g_capbuf);
                out("\r\n");
                out_flush();
                s_cpy(last, g_capbuf, (int)sizeof(last));
            }
            Sleep((U32)iv);
        }
    }
    else if (s_icmp(cmd, "list") == 0)
    {
        int waited = 0, n = 0, k;
        char name[256];

        for (;;)
        {
            mem_zero(g_listbuf, (U32)sizeof(g_listbuf));
            prc = f_read(g_hPvi, idCpu, ACC_LIST_PVAR, 0, 0, g_listbuf, (I32)sizeof(g_listbuf) - 1);
            if (prc == 0) break;
            pump(0, 0);
            if (waited >= timeout) goto list_failed;
            Sleep(200);
            waited += 200;
        }

        /* zoznam premennych je oddeleny znakom TAB -> kazda na vlastny riadok */
        for (k = 0;; k++)
        {
            char c = g_listbuf[k];

            if (c == '\t' || c == 0)
            {
                name[n] = 0;
                if (n > 0 && s_contains_icmp(name, filter))
                {
                    out(name);
                    out("\r\n");
                }
                n = 0;
                if (c == 0) break;
            }
            else if (n < (int)sizeof(name) - 1) name[n++] = c;
        }
    }
    else /* write */
    {
        if (!write_value(idVar, val, timeout)) goto write_failed;
        if (g_verbose)
        {
            out_err("zapisane: ");
            out_err(var);
            out_err(" = ");
            out_errln(val);
        }
    }

    /* --- 7) upratanie --------------------------------------------------- */
    if (idVar)  f_unlink(g_hPvi, idVar);
    if (idTask) f_unlink(g_hPvi, idTask);
    if (idCpu)  f_unlink(g_hPvi, idCpu);
    if (idDev)  f_unlink(g_hPvi, idDev);
    if (idLine) f_unlink(g_hPvi, idLine);
    if (idRoot) f_unlink(g_hPvi, idRoot);
    f_deinit(g_hPvi);
    return 0;

create_failed:
    out_err("chyba: nepodarilo sa vytvorit PVI objekt = ");
    u64_to_dec((U64)(U32)prc, num);
    out_errln(num);
    out_errln("  -> skontroluj IP/protokol; pre starsi AR pouzi --proto ina");
    if (idVar)  f_unlink(g_hPvi, idVar);
    if (idTask) f_unlink(g_hPvi, idTask);
    if (idCpu)  f_unlink(g_hPvi, idCpu);
    if (idDev)  f_unlink(g_hPvi, idDev);
    if (idLine) f_unlink(g_hPvi, idLine);
    if (idRoot) f_unlink(g_hPvi, idRoot);
    f_deinit(g_hPvi);
    return 4;

list_failed:
    out_errln("chyba: nepodarilo sa precitat zoznam premennych");
    out_errln("  -> PLC neodpoveda, alebo AR nepovoluje vycet premennych");
    if (idVar)  f_unlink(g_hPvi, idVar);
    if (idTask) f_unlink(g_hPvi, idTask);
    if (idCpu)  f_unlink(g_hPvi, idCpu);
    if (idDev)  f_unlink(g_hPvi, idDev);
    if (idLine) f_unlink(g_hPvi, idLine);
    if (idRoot) f_unlink(g_hPvi, idRoot);
    f_deinit(g_hPvi);
    return 5;

read_failed:
    if (idVar)  f_unlink(g_hPvi, idVar);
    if (idTask) f_unlink(g_hPvi, idTask);
    if (idCpu)  f_unlink(g_hPvi, idCpu);
    if (idDev)  f_unlink(g_hPvi, idDev);
    if (idLine) f_unlink(g_hPvi, idLine);
    if (idRoot) f_unlink(g_hPvi, idRoot);
    f_deinit(g_hPvi);
    return 5;

write_failed:
    if (idVar)  f_unlink(g_hPvi, idVar);
    if (idTask) f_unlink(g_hPvi, idTask);
    if (idCpu)  f_unlink(g_hPvi, idCpu);
    if (idDev)  f_unlink(g_hPvi, idDev);
    if (idLine) f_unlink(g_hPvi, idLine);
    if (idRoot) f_unlink(g_hPvi, idRoot);
    f_deinit(g_hPvi);
    return 6;
}

/* ===========================================================================
   7) vstupny bod (bez CRT)
   =========================================================================== */

/* Ak bol program spusteny dvojklikom, je v konzole sam -> okno by hned zmizlo.
   Preto pri takomto spusteni pockame na Enter (rovnako ako "pause" v .bat). */
static void pause_if_needed(int rc)
{
    U32 pids[4];
    U32 n;
    char num[16];
    char ch;
    U32 got = 0;
    HPVI_OBJ hIn;

    if (g_nopause) return;

    n = GetConsoleProcessList(pids, 4);
    if (n != 1) return;            /* spustene z cmd.exe -> pauza netreba */

    out("\r\nnavratovy kod: ");
    u64_to_dec((U64)(U32)rc, num);
    out(num);
    out("\r\nStlac Enter pre ukoncenie...");
    out_flush();

    hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != 0 && hIn != (HPVI_OBJ)-1)
    {
        ReadFile(hIn, &ch, 1, &got, 0);
    }
}

void mainCRTStartup(void)
{
    static char cmdline[2048];
    static char *argv[64];
    int argc, rc;

    s_cpy(cmdline, GetCommandLineA(), (int)sizeof(cmdline));
    argc = split_cmdline(cmdline, argv, 64);
    rc = run(argc, argv);

    pause_if_needed(rc);
    out_flush();
    ExitProcess((U32)rc);
}


static int read_value(U32 linkId, TVarType *t, int timeoutMs, int raw)
{
    int esz, len, i;
    I32 rc;

    if (!read_var_type(linkId, t, timeoutMs))
    {
        out_errln("chyba: nepodarilo sa precitat typ premennej (PLC neodpoveda?)");
        return 0;
    }

    esz = (t->vl > 0) ? t->vl : 4;
    len = esz * t->vn;
    if (len > (int)sizeof(g_databuf)) len = (int)sizeof(g_databuf);

    mem_zero(g_databuf, (U32)len);
    rc = f_read(g_hPvi, linkId, ACC_DATA, 0, 0, g_databuf, (I32)len);
    if (rc != 0)
    {
        char n[24];
        out_err("chyba: PviXRead(DATA) = ");
        u64_to_dec((U64)(U32)rc, n);
        out_errln(n);
        return 0;
    }

    if (raw || s_icmp(t->vt, "struct") == 0 || s_icmp(t->vt, "data") == 0)
    {
        print_hex(g_databuf, len);
        return 1;
    }

    if (t->vn <= 1)
    {
        print_one(t, g_databuf, esz);
        return 1;
    }

    for (i = 0; i < t->vn; i++)
    {
        char n[24];
        out("[");
        u64_to_dec((U64)i, n);
        out(n);
        out("]=");
        print_one(t, g_databuf + i * esz, esz);
        if (i + 1 < t->vn) out(" ");
    }
    return 1;
}



/* skus <base>\<dll>, <base>\bin\<dll>, <base>\Bin\<dll>, ... */
static int try_dir(const char *base, const char *dll)
{
    static const char *sub[5] = { "", "\\bin", "\\Bin", "\\pvi", "\\PVI" };
    char path[512];
    HPVI_OBJ mod;
    int i;

    for (i = 0; i < 5; i++)
    {
        s_cpy(path, base, (int)sizeof(path));
        s_cat(path, sub[i], (int)sizeof(path));
        if (path[s_len(path) - 1] != '\\') s_cat(path, "\\", (int)sizeof(path));
        s_cat(path, dll, (int)sizeof(path));
        if (!file_exists(path)) continue;

        mod = LoadLibraryA(path);
        if (!mod) continue;
        if (resolve(mod, path)) return 1;
    }
    return 0;
}
