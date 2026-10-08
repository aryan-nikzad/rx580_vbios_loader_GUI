/* config.c - settings: built-in defaults < \EFI\vbios_loader\vbios_loader.cfg (key=value lines) < NVRAM overrides.
 * The loader NEVER writes the .cfg file; the settings page only writes the NVRAM override variable. */
#include "loader.h"

static EFI_GUID gCfgGuid = {0x5b6c2a7e,0x9d34,0x4f1a,{0x8e,0x21,0x7a,0x0c,0x3d,0x44,0xb1,0x90}};
#define CFG_VAR L"VbiosLdrCfg"
#define PIN_VAR L"VbiosLdrPins"

INT32 g_set[S_COUNT]; UINT8 g_origin[S_COUNT];
static INT32 g_fileval[S_COUNT]; static BOOLEAN g_fileset[S_COUNT];
CHAR16 g_cfg_romdir[128]; static UINT8 g_romdir_origin;
PIN g_pins[MAX_CARDS]; UINTN g_npins;
BOOLEAN g_cfg_file_found; CHAR16 g_cfg_path[160];

const SETDEF g_setdef[S_COUNT] = {
 [S_UI_MODE]      = { (CHAR8*)"ui_mode",         1, 0, 2, UI_AUTO, (CHAR8*)"auto|gui|text", L"Interface",            L"gui = graphical (needs a display with GOP), text = classic console, auto = gui if possible" },
 [S_BOOT_MODE]    = { (CHAR8*)"boot_mode",       1, 0, 1, BM_AUTO, (CHAR8*)"auto|menu",     L"Start behaviour",       L"auto = start by itself after the timeout, menu = wait for you on the status page" },
 [S_MENU_TIMEOUT] = { (CHAR8*)"menu_timeout",    2, 1, 30, 1,      NULL,                    L"Start delay (s)",       L"Seconds the status page waits before AUTO starts; any key cancels the countdown and leaves the menu open" },
 [S_SKIP_INIT]    = { (CHAR8*)"skip_initialized",0, 0, 1, 1,       NULL,                    L"Skip initialised cards",L"Leave cards alone that are already initialised and trained (shown green)" },
 [S_RESET_MODE]   = { (CHAR8*)"reset_mode",      1, 0, 3, RS_GPU,  (CHAR8*)"gpu|pcie|reboot|none", L"Reset after failed ROM", L"gpu = AMD config reset of that card, pcie = PCIe secondary-bus reset, reboot = full cold reboot then continue with next ROM, none = just try next ROM" },
 [S_SMART_ORDER]  = { (CHAR8*)"smart_order",     0, 0, 1, 1,       NULL,                    L"Smart ROM order",       L"Try ROMs whose file name matches the card's brand first, then the rest by name" },
 [S_PIN_FALLBACK] = { (CHAR8*)"pin_fallback",    0, 0, 1, 1,       NULL,                    L"Pinned ROM fallback",   L"If a ROM pinned to a card fails, continue with the other ROMs" },
 [S_RETRY_FAILED] = { (CHAR8*)"retry_failed",    0, 0, 1, 0,       NULL,                    L"Retry failed cards",    L"Retry cards whose ROM list was exhausted on every boot (off = press R on the status page)" },
 [S_FINISH_SECS]  = { (CHAR8*)"finish_secs",     2, 0, 120, 8,     NULL,                    L"Continue boot after failure (s)",L"Seconds the finished status page stays after a failed card before the next boot attempt" },
 [S_BOOT_DELAY]   = { (CHAR8*)"boot_delay",      2, 1, 30,  3,     NULL,                    L"Pause before OS boot (s)",L"Seconds to wait before OS boot (1..30; default 3); any key cancels the countdown and leaves the loader menu open" },
 [S_HOLD_ON_FAIL] = { (CHAR8*)"hold_on_fail",    0, 0, 1, 1,       NULL,                    L"Hold if a card failed", L"Wait for a key instead of continuing when a card could not be initialised" },
 [S_LOOP_MS]      = { (CHAR8*)"loop_ms",         2, 50, 5000, 300, NULL,                    L"Stuck-poll limit (ms)", L"A register poll that does not finish in this time is skipped (AtomBIOS engine)" },
 [S_ENGINE]       = { (CHAR8*)"engine",          1, 0, 1, ENG_ATOM,(CHAR8*)"atom|gop",      L"Init engine",           L"atom = built-in interpreter (recommended), gop = firmware GOP driver (can hang the PC)" },
 [S_GOP_TIMEOUT]  = { (CHAR8*)"gop_timeout",     2, 3, 120, 12,    NULL,                    L"GOP engine timeout (s)",L"Only for engine=gop: seconds before the whole PC is reset" },
 [S_VFCT_ONLY]    = { (CHAR8*)"vfct_only",       0, 0, 1, 0,       NULL,                    L"VFCT only",             L"Only publish the ROM to the OS, do not initialise the card" },
 [S_POSTDUMP]     = { (CHAR8*)"post_dump",       0, 0, 1, 0,       NULL,                    L"Dump regs after init",  L"Write a register snapshot after init (diagnostic, can freeze the PC)" },
 [S_TRACE]        = { (CHAR8*)"trace",           0, 0, 1, 1,       NULL,                    L"Trace file",            L"Write loader_trace.txt (flushed per line) next to the loader" },
 [S_UI_SCALE]     = { (CHAR8*)"ui_scale",        2, 0, 3, 0,       NULL,                    L"GUI text scale",        L"0 = automatic, 1..3 = fixed magnification of the GUI font" },
 [S_VFCT_MODE]   = { (CHAR8*)"vfct_mode",       1, 0, 3, VM_AUTO, (CHAR8*)"auto|acpi|xsdt|off", L"VFCT install method", L"How the ROMs reach Linux: auto = ACPI protocol, falls back to patching the XSDT; acpi = protocol only; xsdt = patch XSDT directly (if the firmware freezes in the ACPI call); off = none" },
 [S_DEMO]         = { (CHAR8*)"demo",            0, 0, 1, 0,       NULL,                    L"Demo cards",            L"Show fake cards to preview the interface (no hardware is touched)" },
};

void cfg_defaults(void)
{
  for (UINTN i = 0; i < S_COUNT; i++) { g_set[i] = g_setdef[i].def; g_origin[i] = OR_DEFAULT; g_fileset[i] = FALSE; }
  g_cfg_romdir[0] = 0; g_romdir_origin = OR_DEFAULT; g_npins = 0; ZeroMem(g_pins, sizeof g_pins);
}

/* ---- text helpers ---- */
static BOOLEAN is_ws(CHAR8 c) { return c == ' ' || c == '\t' || c == '\r'; }
static CHAR8 lc(CHAR8 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static BOOLEAN eqi(const CHAR8 *a, UINTN al, const CHAR8 *b)
{
  UINTN i = 0; for (; i < al && b[i]; i++) if (lc(a[i]) != lc(b[i])) return FALSE;
  return i == al && !b[i];
}
static INTN hexv(CHAR8 c) { if (c >= '0' && c <= '9') return c - '0'; c = lc(c); if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; }

/* "03:00.0" or "0000:03:00.0" at s[0..n) -> bus/dev/fn; returns chars consumed or 0 */
static UINTN parse_bdf(const CHAR8 *s, UINTN n, UINTN *bus, UINTN *dev, UINTN *fn)
{
  UINTN v[4], nv = 0, i = 0;
  for (;;) {
    UINTN x = 0; INTN h;
    if (i >= n || hexv(s[i]) < 0 || nv >= 4) return 0;
    while (i < n && (h = hexv(s[i])) >= 0) { x = x * 16 + (UINTN)h; i++; }
    v[nv++] = x;
    if (i < n && s[i] == ':') { i++; continue; }
    if (i < n && s[i] == '.') {
      i++; x = 0; if (i >= n || hexv(s[i]) < 0 || nv >= 4) return 0;
      while (i < n && (h = hexv(s[i])) >= 0) { x = x * 16 + (UINTN)h; i++; }
      v[nv++] = x; break;
    }
    return 0;
  }
  if (nv == 3) { *bus = v[0]; *dev = v[1]; *fn = v[2]; return i; }
  if (nv == 4) { *bus = v[1]; *dev = v[2]; *fn = v[3]; return i; }
  return 0;
}

PIN *pin_find(UINTN bus, UINTN dev, UINTN fn, BOOLEAN create)
{
  for (UINTN i = 0; i < g_npins; i++) if (g_pins[i].bus == bus && g_pins[i].dev == dev && g_pins[i].fn == fn) return &g_pins[i];
  if (!create || g_npins >= MAX_CARDS) return NULL;
  PIN *p = &g_pins[g_npins++]; ZeroMem(p, sizeof *p); p->bus = (UINT8)bus; p->dev = (UINT8)dev; p->fn = (UINT8)fn; return p;
}

static void to16(CHAR16 *d, UINTN dn, const CHAR8 *s, UINTN n) { UINTN i = 0; for (; i < n && i + 1 < dn; i++) d[i] = (CHAR8)s[i] < 0x80 ? (CHAR16)s[i] : L'?'; d[i] = 0; }

static BOOLEAN parse_bool(const CHAR8 *v, UINTN n, INT32 *out)
{
  if (eqi(v, n, "1") || eqi(v, n, "on") || eqi(v, n, "yes") || eqi(v, n, "true")) { *out = 1; return TRUE; }
  if (eqi(v, n, "0") || eqi(v, n, "off") || eqi(v, n, "no") || eqi(v, n, "false")) { *out = 0; return TRUE; }
  return FALSE;
}
static BOOLEAN parse_enum(const CHAR8 *choices, const CHAR8 *v, UINTN n, INT32 *out)
{
  UINTN idx = 0; const CHAR8 *p = choices;
  for (;;) {
    const CHAR8 *e = p; while (*e && *e != '|') e++;
    if ((UINTN)(e - p) == n && eqi(v, n, p)) { *out = (INT32)idx; return TRUE; }
    if (!*e) return FALSE;
    idx++; p = e + 1;
  }
}

static void apply_pin(UINTN bus, UINTN dev, UINTN fn, const CHAR8 *field, UINTN fl, const CHAR8 *v, UINTN vn, UINT8 origin)
{
  PIN *p = pin_find(bus, dev, fn, TRUE); if (!p) return;
  if (eqi(field, fl, "rom")) { if (eqi(v, vn, "auto") || !vn) p->rom[0] = 0; else to16(p->rom, 72, v, vn); }
  else if (eqi(field, fl, "skip")) { INT32 b; if (parse_bool(v, vn, &b)) p->skip = (BOOLEAN)b; }
  else if (eqi(field, fl, "force")) { INT32 b; if (parse_bool(v, vn, &b)) p->force = (BOOLEAN)b; }
  else if (eqi(field, fl, "name")) to16(p->name, 24, v, vn);
  else return;
  p->origin = origin;
}

static void apply_kv(const CHAR8 *k, UINTN kn, const CHAR8 *v, UINTN vn, UINT8 origin)
{
  if (eqi(k, kn, "rom_dir")) { to16(g_cfg_romdir, 128, v, vn); g_romdir_origin = origin; return; }
  if (kn > 5 && lc(k[0]) == 'c' && lc(k[1]) == 'a' && lc(k[2]) == 'r' && lc(k[3]) == 'd' && k[4] == '.') {
    UINTN b, d, f, used = parse_bdf(k + 5, kn - 5, &b, &d, &f);
    if (used && 5 + used < kn && k[5 + used] == '.') apply_pin(b, d, f, k + 6 + used, kn - 6 - used, v, vn, origin);
    return;
  }
  for (UINTN i = 0; i < S_COUNT; i++) {
    const SETDEF *sd = &g_setdef[i]; UINTN l = 0; while (sd->key[l]) l++;
    if (!eqi(k, kn, sd->key) || l != kn) continue;
    INT32 val = sd->def; BOOLEAN ok = FALSE;
    if (sd->type == 0) ok = parse_bool(v, vn, &val);
    else if (sd->type == 1) ok = parse_enum(sd->choices, v, vn, &val);
    else { INT32 x = 0; BOOLEAN neg = FALSE, any = FALSE; UINTN j = 0; if (j < vn && v[j] == '-') { neg = TRUE; j++; }
           for (; j < vn && v[j] >= '0' && v[j] <= '9'; j++) { x = x * 10 + (v[j] - '0'); any = TRUE; }
           if (any && j == vn) { val = neg ? -x : x; ok = TRUE; } }
    if (!ok) return;
    if (val < sd->lo) val = sd->lo; if (val > sd->hi) val = sd->hi;
    g_set[i] = val; g_origin[i] = origin; if (origin == OR_FILE) { g_fileval[i] = val; g_fileset[i] = TRUE; }
    return;
  }
}

void cfg_parse(const CHAR8 *buf, UINTN len, UINT8 origin)
{
  UINTN i = 0;
  while (i < len) {
    UINTN s = i; while (i < len && buf[i] != '\n') i++;
    UINTN e = i; if (i < len) i++;
    while (s < e && is_ws(buf[s])) s++;
    if (s >= e || buf[s] == '#' || buf[s] == ';') continue;
    UINTN eq = s; while (eq < e && buf[eq] != '=') eq++;
    if (eq >= e) continue;
    UINTN ke = eq; while (ke > s && is_ws(buf[ke-1])) ke--;
    UINTN vs = eq + 1; while (vs < e && is_ws(buf[vs])) vs++;
    UINTN ve = e;
    for (UINTN j = vs + 1; j < e; j++) if (buf[j] == '#' && is_ws(buf[j-1])) { ve = j; break; }   /* inline " # comment" */
    while (ve > vs && is_ws(buf[ve-1])) ve--;
    if (ke > s) apply_kv(buf + s, ke - s, buf + vs, ve - vs, origin);
  }
}

/* ---- config file ---- */
void cfg_load_file(EFI_FILE_HANDLE root, const CHAR16 *owndir)
{
  CHAR16 *paths[3]; UINTN np = 0;
  g_cfg_file_found = FALSE; g_cfg_path[0] = 0;
  if (!root) return;
  paths[np++] = PoolPrint(L"%s\\%s", owndir && owndir[0] ? owndir : L"", CFG_FILENAME);
  paths[np++] = PoolPrint(L"\\EFI\\%s\\%s", LOADER_DIRNAME, CFG_FILENAME);
  paths[np++] = PoolPrint(L"\\%s", CFG_FILENAME);
  for (UINTN i = 0; i < np && !g_cfg_file_found; i++) {
    EFI_FILE_HANDLE f;
    if (EFI_ERROR(FW(root->Open, root, &f, paths[i], EFI_FILE_MODE_READ, 0))) continue;
    EFI_FILE_INFO *fi = LibFileInfo(f); UINTN sz = fi ? (UINTN)fi->FileSize : 0; if (fi) FreePool(fi);
    if (sz && sz <= 64 * 1024) {
      CHAR8 *b = AllocatePool(sz);
      if (b && !EFI_ERROR(FW(f->Read, f, &sz, b))) { cfg_parse(b, sz, OR_FILE); g_cfg_file_found = TRUE; StrCpy(g_cfg_path, paths[i]); }
      if (b) FreePool(b);
    } else if (!sz) { g_cfg_file_found = TRUE; StrCpy(g_cfg_path, paths[i]); }
    FW(f->Close, f);
  }
  for (UINTN i = 0; i < np; i++) FreePool(paths[i]);
}

/* ---- NVRAM overrides: same text format as the file ---- */
void cfg_load_nvram(void)
{
  CHAR8 buf[2048]; UINTN sz = sizeof buf; UINT32 at;
  if (!EFI_ERROR(FW(RT->GetVariable, CFG_VAR, &gCfgGuid, &at, &sz, buf))) cfg_parse(buf, sz, OR_NVRAM);
}

static UINTN put(CHAR8 *o, UINTN n, const CHAR8 *s) { while (*s && n < 2000) o[n++] = *s++; return n; }
static UINTN put16(CHAR8 *o, UINTN n, const CHAR16 *s) { while (*s && n < 2000) o[n++] = *s < 0x80 ? (CHAR8)*s : '?', s++; return n; }

void cfg_save_nvram(void)
{
  CHAR8 buf[2048]; UINTN n = 0;
  for (UINTN i = 0; i < S_COUNT; i++) {
    if (g_origin[i] != OR_NVRAM) continue;
    CHAR8 num[16]; CHAR16 *vs = cfg_value_str(i);
    n = put(buf, n, g_setdef[i].key); n = put(buf, n, "="); (void)num; n = put16(buf, n, vs); n = put(buf, n, "\n");
  }
  for (UINTN i = 0; i < g_npins; i++) {
    PIN *p = &g_pins[i]; if (p->origin != OR_NVRAM) continue;
    CHAR16 pre[48]; SPrint(pre, sizeof pre, L"card.%02x:%02x.%x.", p->bus, p->dev, p->fn);
    n = put16(buf, n, pre); n = put(buf, n, "rom="); n = put16(buf, n, p->rom[0] ? p->rom : L"auto"); n = put(buf, n, "\n");
    n = put16(buf, n, pre); n = put(buf, n, p->skip ? "skip=1\n" : "skip=0\n");
    n = put16(buf, n, pre); n = put(buf, n, p->force ? "force=1\n" : "force=0\n");
    if (p->name[0]) { n = put16(buf, n, pre); n = put(buf, n, "name="); n = put16(buf, n, p->name); n = put(buf, n, "\n"); }
  }
  if (g_romdir_origin == OR_NVRAM && g_cfg_romdir[0]) { n = put(buf, n, "rom_dir="); n = put16(buf, n, g_cfg_romdir); n = put(buf, n, "\n"); }
  FW(RT->SetVariable, CFG_VAR, &gCfgGuid, n ? (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS) : 0, n, buf);
}
void pin_commit(void) { cfg_save_nvram(); }

void cfg_reset_nvram(void)
{
  FW(RT->SetVariable, CFG_VAR, &gCfgGuid, 0, 0, NULL);
  for (UINTN i = 0; i < S_COUNT; i++) if (g_origin[i] == OR_NVRAM) { g_set[i] = g_fileset[i] ? g_fileval[i] : g_setdef[i].def; g_origin[i] = g_fileset[i] ? OR_FILE : OR_DEFAULT; }
  for (UINTN i = 0; i < g_npins; i++) if (g_pins[i].origin == OR_NVRAM) g_pins[i].origin = OR_DEFAULT;
}

CHAR16 *cfg_value_str(UINTN id)
{
  static CHAR16 b[4][40]; static UINTN k; CHAR16 *o = b[k++ & 3]; const SETDEF *sd = &g_setdef[id];
  if (sd->type == 0) { StrCpy(o, g_set[id] ? L"on" : L"off"); return o; }
  if (sd->type == 2) { SPrint(o, 80, L"%d", g_set[id]); return o; }
  const CHAR8 *p = sd->choices; for (INT32 i = 0; i < g_set[id] && *p; i++) { while (*p && *p != '|') p++; if (*p) p++; }
  UINTN j = 0; while (*p && *p != '|' && j < 38) o[j++] = *p++; o[j] = 0; return o;
}

void cfg_step(UINTN id, INTN dir)
{
  const SETDEF *sd = &g_setdef[id]; INT32 v = g_set[id];
  INT32 step = (sd->type == 2 && sd->hi - sd->lo >= 1000) ? 50 : 1;
  v += (INT32)dir * step;
  if (sd->type == 2) { if (v < sd->lo) v = sd->lo; if (v > sd->hi) v = sd->hi; }
  else { if (v > sd->hi) v = sd->lo; if (v < sd->lo) v = sd->hi; }
  g_set[id] = v; g_origin[id] = OR_NVRAM; cfg_save_nvram();
}
void cfg_clear_override(UINTN id)
{
  if (g_origin[id] != OR_NVRAM) return;
  g_set[id] = g_fileset[id] ? g_fileval[id] : g_setdef[id].def; g_origin[id] = g_fileset[id] ? OR_FILE : OR_DEFAULT; cfg_save_nvram();
}
