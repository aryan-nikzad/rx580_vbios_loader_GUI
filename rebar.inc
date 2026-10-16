/* rebar.inc - included by vbios_loader.c (uses its statics: g_mmio, c_rr, mmio_w, own_path, g_vol_root, g_root).
 *
 * Two features, both for cards whose SPI chip is dead (no hardware straps loaded -> no Resizable BAR capability):
 *
 *  1. rebar_probe(): config-space-only (safe, same reads amdgpu does) report of the PCIe extended capability
 *     chain, the Resizable BAR capability and the BAR/subsystem registers. Logged, and saved as
 *     cfg_<bdf>_<tag>.bin (4 KB raw config space) when trace=on. Compare a healthy card with a dead card.
 *
 *  2. rebar_patch_apply(): after a card was brought up by ASIC_Init (never for untouched/green cards), apply the
 *     lines of  rebar_patch.txt  (next to the loader) that match the card's PCI address:
 *
 *       BB:DD.F  mmio  REG        VALUE [MASK]            dword MMIO register index (BAR5), read-modify-write
 *       BB:DD.F  ind   IDX DATA ADDR VALUE [MASK]         indirect: write ADDR to MMIO reg IDX, rmw MMIO reg DATA
 *       BB:DD.F  cfg   OFFSET     VALUE [MASK]            PCI config dword (offset 0..0xFFC), read-modify-write
 *
 *     All numbers are hex (0x optional). '#' starts a comment. MASK defaults to FFFFFFFF.
 *     Example:   0d:00.0 cfg 0x204 0x00000D00 0x00003F00
 */

static UINT32 cfg_rd32(EFI_PCI_IO_PROTOCOL *p, UINTN off)
{
  UINT32 v = 0xFFFFFFFFu;
  FW(p->Pci.Read, p, EfiPciIoWidthUint32, off, 1, &v);
  return v;
}
static void cfg_wr32(EFI_PCI_IO_PROTOCOL *p, UINTN off, UINT32 v) { FW(p->Pci.Write, p, EfiPciIoWidthUint32, off, 1, &v); }

static void rb_save(const CHAR16 *name, const void *data, UINTN len)
{
  EFI_FILE_HANDLE root = g_vol_root ? g_vol_root : g_root, f;
  if (!root) return;
  CHAR16 *path = own_path(name);
  EFI_STATUS s = FW(root->Open, root, &f, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  FreePool(path);
  if (EFI_ERROR(s)) return;
  UINTN sz = len; FW(f->Write, f, &sz, (VOID *)data); FW(f->Flush, f); FW(f->Close, f);
}

/* tag: L"pre" (before init), L"post" (after all cards), L"dump" ... */
static void rebar_probe(CARD *c, const CHAR16 *tag, BOOLEAN save)
{
  if (!c || c->demo || !c->pio) return;
  UINT32 *buf = AllocatePool(4096);
  if (!buf) return;
  for (UINTN i = 0; i < 1024; i++) buf[i] = cfg_rd32(c->pio, i * 4);
  lg(L"  [CFG %s] %02x:%02x.%x  BAR0=%08x BAR2=%08x BAR5=%08x  subsys=%08x\n", tag, (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn,
     buf[4], buf[6], buf[9], buf[0x2C / 4]);
  BOOLEAN have_rebar = FALSE; UINTN off = 0x100;
  if (buf[0x100 / 4] == 0xFFFFFFFFu || buf[0x100 / 4] == 0)
    lg(L"  [CFG %s]   no PCIe extended capabilities readable at 0x100 (extended config space unavailable or empty)\n", tag);
  for (UINTN guard = 0; guard < 48 && off >= 0x100 && off < 0x1000; guard++) {
    UINT32 h = buf[off / 4];
    if (h == 0 || h == 0xFFFFFFFFu) break;
    UINT32 id = h & 0xFFFF, ver = (h >> 16) & 0xF; UINTN next = (h >> 20) & 0xFFC;
    lg(L"  [CFG %s]   ext cap @%03x id=%04x v%d%s\n", tag, (UINT32)off, id, ver, id == 0x15 ? L"  <- Resizable BAR" : L"");
    if (id == 0x15 && off + 12 <= 0x1000) {
      have_rebar = TRUE;
      UINT32 ctrl0 = buf[(off + 8) / 4]; UINTN nb = (ctrl0 >> 5) & 7; if (nb < 1) nb = 1; if (nb > 6) nb = 6;
      for (UINTN k = 0; k < nb && off + 4 + 8 * k + 8 <= 0x1000; k++) {
        UINT32 cap = buf[(off + 4 + 8 * k) / 4], ctl = buf[(off + 8 + 8 * k) / 4];
        lg(L"  [CFG %s]     ReBAR entry %d: cap=%08x ctrl=%08x  BAR idx %d, current size 2^%d MB, supported-size mask %05x%s\n", tag, (UINT32)k, cap, ctl,
           ctl & 7, (ctl >> 8) & 0x3F, (cap >> 4) & 0xFFFFF, ((cap >> 4) & (1u << 13)) ? L" (8 GB supported)" : L"");
      }
    }
    if (next == off) break;
    off = next;
  }
  lg(L"  [CFG %s]   Resizable BAR capability: %s\n", tag, have_rebar ? L"PRESENT" : L"ABSENT");
  TRACE(L"CFG %s %02x:%02x.%x rebar=%d bar0=%08x", tag, (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn, (int)have_rebar, buf[4]);
  if (save) {
    CHAR16 nm[64]; SPrint(nm, sizeof nm, L"cfg_%02x%02x%x_%s.bin", (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn, tag);
    rb_save(nm, buf, 4096);
  }
  FreePool(buf);
}

/* Read the GPU's config-space mirror through BAR5. Unlike EFI_PCI_IO, this path
 * can expose the extended-capability bytes on boards where direct config reads
 * stop at 0xFF. This is diagnostic only: reporting a capability is not the same
 * as creating one, and these reads do not change BAR allocation. Call only while
 * g_mmio still refers to this card (inside run_cand, after ASIC_Init). */
static void rebar_mirror_report(CARD *c, const CHAR16 *tag)
{
  if (!c || c->demo || !c->mmio || !g_mmio) return;
  UINT32 *buf = AllocatePool(4096);
  if (!buf) return;

  for (UINTN i = 0; i < 1024; i++) buf[i] = c_rr(NULL, 0x1000 + (UINT32)i);
  lg(L"  [MIR %s] %02x:%02x.%x BAR5 mirror PCIe extended capabilities:",
     tag, (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn);

  BOOLEAN have_rebar = FALSE;
  UINTN off = 0x100;
  for (UINTN guard = 0; guard < 64 && off >= 0x100 && off <= 0xFFC; guard++) {
    UINT32 h = buf[off / 4];
    if (h == 0 || h == 0xFFFFFFFFu) break;
    UINT32 id = h & 0xFFFFu;
    UINTN next = (h >> 20) & 0xFFCu;
    lg(L" %03x:%04x", (UINT32)off, id);
    if (id == 0x0015 && off + 12 <= 0x1000) {
      have_rebar = TRUE;
      UINT32 cap = buf[(off + 4) / 4];
      UINT32 ctl = buf[(off + 8) / 4];
      lg(L"[ReBAR cap=%08x ctrl=%08x mask=%05x BAR=%u size=%u]",
         cap, ctl, (cap >> 4) & 0xFFFFFu, ctl & 7u, (ctl >> 8) & 0x3Fu);
    }
    if (!next || next == off) break;
    off = next;
  }
  lg(L" => ReBAR %s (mirror only; no capability or BAR was changed)\n",
     have_rebar ? L"PRESENT" : L"ABSENT");
  TRACE(L"MIR %s %02x:%02x.%x rebar=%d", tag, (UINT32)c->bus,
        (UINT32)c->dev, (UINT32)c->fn, (int)have_rebar);

  if (CFG(S_TRACE)) {
    CHAR16 name[64];
    SPrint(name, sizeof name, L"mir_cfg_%02x%02x%x_%s.bin",
           (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn, tag);
    rb_save(name, buf, 4096);
  }
  FreePool(buf);
}

/* ---------- patch file ---------- */
static BOOLEAN rb_hex(const CHAR8 *s, UINTN n, UINT32 *o)
{
  UINTN i = 0; UINT32 v = 0;
  if (n > 2 && s[0] == '0' && (s[1] | 0x20) == 'x') i = 2;
  if (i >= n) return FALSE;
  for (; i < n; i++) {
    CHAR8 ch = s[i]; INTN d;
    if (ch >= '0' && ch <= '9') d = ch - '0';
    else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f') d = (ch | 0x20) - 'a' + 10;
    else return FALSE;
    v = v * 16 + (UINT32)d;
  }
  *o = v; return TRUE;
}
static BOOLEAN rb_bdf(const CHAR8 *s, UINTN n, UINT32 *b, UINT32 *d, UINT32 *f)
{
  if (n != 7 || s[2] != ':' || s[5] != '.') return FALSE;
  return rb_hex(s, 2, b) && rb_hex(s + 3, 2, d) && rb_hex(s + 6, 1, f);
}

static void rebar_patch_apply(CARD *c)
{
  if (!c || !c->pio || !g_mmio) return;
  rebar_mirror_report(c, L"post-init-before-patch");
  EFI_FILE_HANDLE root = g_vol_root ? g_vol_root : g_root, f;
  if (!root) return;
  CHAR16 *path = own_path(L"rebar_patch.txt");
  EFI_STATUS s = FW(root->Open, root, &f, path, EFI_FILE_MODE_READ, 0);
  FreePool(path);
  if (EFI_ERROR(s)) return;                                   /* no patch file: nothing to do */
  EFI_FILE_INFO *fi = LibFileInfo(f); UINTN sz = fi ? (UINTN)fi->FileSize : 0; if (fi) FreePool(fi);
  if (!sz || sz > 32768) { FW(f->Close, f); return; }
  CHAR8 *b = AllocatePool(sz);
  if (!b || EFI_ERROR(FW(f->Read, f, &sz, b))) { FW(f->Close, f); if (b) FreePool(b); return; }
  FW(f->Close, f);
  UINTN applied = 0;
  for (UINTN i = 0; i < sz;) {
    const CHAR8 *t[8]; UINTN tl[8], nt = 0;
    while (i < sz && b[i] != '\n') {                           /* tokenise one line */
      while (i < sz && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r')) i++;
      if (i >= sz || b[i] == '\n') break;
      if (b[i] == '#') { while (i < sz && b[i] != '\n') i++; break; }
      UINTN st = i; while (i < sz && b[i] != ' ' && b[i] != '\t' && b[i] != '\r' && b[i] != '\n') i++;
      if (nt < 8) { t[nt] = b + st; tl[nt] = i - st; nt++; }
    }
    if (i < sz) i++;
    UINT32 bb, dd, ff;
    if (nt < 4 || !rb_bdf(t[0], tl[0], &bb, &dd, &ff)) continue;
    if (bb != c->bus || dd != c->dev || ff != c->fn) continue;
    UINT32 a[5] = {0, 0, 0, 0, 0xFFFFFFFFu}; BOOLEAN ok = TRUE;
    if (tl[1] == 4 && !CompareMem(t[1], "mmio", 4)) {
      if (nt < 4 || !rb_hex(t[2], tl[2], &a[0]) || !rb_hex(t[3], tl[3], &a[1])) ok = FALSE;
      if (ok && nt >= 5 && !rb_hex(t[4], tl[4], &a[4])) ok = FALSE;
      if (!ok) { lg(L"   [PATCH] bad mmio line ignored\n"); continue; }
      UINT32 old = c_rr(NULL, a[0]), nw = (old & ~a[4]) | (a[1] & a[4]);
      mmio_w(a[0], nw);
      lg(L"   [PATCH] mmio %05x: %08x -> %08x (read back %08x)\n", a[0], old, nw, c_rr(NULL, a[0])); TRACE(L"PATCH mmio %05x %08x->%08x", a[0], old, nw);
    } else if (tl[1] == 3 && !CompareMem(t[1], "ind", 3)) {
      if (nt < 6) { lg(L"   [PATCH] bad ind line ignored\n"); continue; }
      UINT32 idx, dat, addr, val, mask = 0xFFFFFFFFu;
      if (!rb_hex(t[2], tl[2], &idx) || !rb_hex(t[3], tl[3], &dat) || !rb_hex(t[4], tl[4], &addr) || !rb_hex(t[5], tl[5], &val) ||
          (nt >= 7 && !rb_hex(t[6], tl[6], &mask))) { lg(L"   [PATCH] bad ind line ignored\n"); continue; }
      mmio_w(idx, addr); UINT32 old = c_rr(NULL, dat), nw = (old & ~mask) | (val & mask);
      mmio_w(idx, addr); mmio_w(dat, nw); mmio_w(idx, addr);
      lg(L"   [PATCH] ind idx=%x data=%x addr=%08x: %08x -> %08x (read back %08x)\n", idx, dat, addr, old, nw, c_rr(NULL, dat)); TRACE(L"PATCH ind %08x %08x->%08x", addr, old, nw);
    } else if (tl[1] == 3 && !CompareMem(t[1], "cfg", 3)) {
      if (!rb_hex(t[2], tl[2], &a[0]) || !rb_hex(t[3], tl[3], &a[1]) || (nt >= 5 && !rb_hex(t[4], tl[4], &a[4])) || a[0] > 0xFFC) { lg(L"   [PATCH] bad cfg line ignored\n"); continue; }
      UINT32 old = cfg_rd32(c->pio, a[0]), nw = (old & ~a[4]) | (a[1] & a[4]);
      cfg_wr32(c->pio, a[0], nw);
      lg(L"   [PATCH] cfg %03x: %08x -> %08x (read back %08x)\n", a[0], old, nw, cfg_rd32(c->pio, a[0])); TRACE(L"PATCH cfg %03x %08x->%08x", a[0], old, nw);
    } else continue;
    applied++;
  }
  FreePool(b);
  if (applied) { lg(L"   [PATCH] %d operation(s) applied to %02x:%02x.%x\n", (UINT32)applied, (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn); rebar_probe(c, L"patched", CFG(S_TRACE)); }
}
