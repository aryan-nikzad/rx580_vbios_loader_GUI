/*
 * vbios_loader.c - UEFI app: POST an AMD Polaris GPU whose SPI vBIOS chip is dead.
 *
 * ROM sources : every <dir of this .efi>\vbioses\*.rom (sorted by name).
 *               No firmware is embedded in this source tree; the user supplies the ROM files.
 * AUTO mode   : tries ROMs one by one.  For each: publish ROM (PciIo->RomImage +
 *               ACPI VFCT), run the ROM's UEFI GOP driver, ConnectController().
 *               If the GPU init hangs (TIMEOUT_SECS) the timer callback resets the
 *               machine; progress is kept in an NVRAM variable so the next boot tries
 *               the next ROM.  The ROM that works is remembered and used first.
 * Menu keys   : UP/DOWN+ENTER = run one ROM, V = VFCT-only, R = forget state, ESC = skip.
 */
#include "loader.h"

static void trace_write(const CHAR16 *msg);
static void post_code(UINT8 v);
#define TRACE(...) do { CHAR16 _tb[160]; SPrint(_tb, sizeof _tb, __VA_ARGS__); trace_write(_tb); } while (0)

#define AMD_VID       0x1002
#define TIMEOUT_SECS  CFG(S_GOP_TIMEOUT)
#define MAX_ROM_BYTES (2u * 1024 * 1024)

/* ---------- protocols not in gnu-efi ---------- */
typedef struct _DECOMP DECOMP;
struct _DECOMP {
  EFI_STATUS (EFIAPI *GetInfo)(DECOMP *, VOID *, UINT32, UINT32 *, UINT32 *);
  EFI_STATUS (EFIAPI *Decompress)(DECOMP *, VOID *, UINT32, VOID *, UINT32, VOID *, UINT32);
};
static EFI_GUID gDecomp = {0xd8117cfe,0x94a6,0x11d4,{0x9a,0x3a,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
typedef struct _ACPITBL ACPITBL;
struct _ACPITBL {
  EFI_STATUS (EFIAPI *Install)(ACPITBL *, VOID *, UINTN, UINTN *);
  EFI_STATUS (EFIAPI *Uninstall)(ACPITBL *, UINTN);
};
static EFI_GUID gAcpiTbl = {0x31ce593d,0x108a,0x485d,{0xad,0xb2,0x78,0xf2,0x1f,0x29,0x66,0xbe}};
static EFI_GUID gStateGuid = {0x5b6c2a7e,0x9d34,0x4f1a,{0x8e,0x21,0x7a,0x0c,0x3d,0x44,0xb1,0x90}};

/* ---------- persistent per-card history (NVRAM, one variable per card: VbiosLdrC<bus><dev><fn>) ---------- */
CARD g_cards[MAX_CARDS]; UINTN g_ncards;
static CHIST *g_h; static CARD *g_hc;             /* history / card currently being worked on */
#define g_st (*g_h)
static void hist_name(CARD *c, CHAR16 *o) { SPrint(o, 64, L"VbiosLdrC%02x%02x%x", (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn); }
static void state_save(void)
{
  CHAR16 nm[32]; if (!g_hc) return; hist_name(g_hc, nm);
  FW(RT->SetVariable, nm, &gStateGuid, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof(CHIST), &g_hc->hist);
}
static void hist_load(CARD *c, UINT32 sig)
{
  CHAR16 nm[32]; UINTN sz = sizeof(CHIST); UINT32 at; CHIST t; hist_name(c, nm);
  ZeroMem(&c->hist, sizeof(CHIST)); c->hist.magic = STATE_MAGIC; c->hist.sig = sig;
  if (EFI_ERROR(FW(RT->GetVariable, nm, &gStateGuid, &at, &sz, &t)) || sz != sizeof(CHIST) || t.magic != STATE_MAGIC) return;
  if (t.sig != sig) { StrCpy(c->hist.working, t.working); return; }      /* ROM list/card changed: keep only the remembered ROM */
  c->hist = t;
}
static void hist_forget(CARD *c) { ZeroMem(&c->hist, sizeof(CHIST)); c->hist.magic = STATE_MAGIC; c->hist.sig = 0; g_hc = c; g_h = &c->hist; state_save(); }

/* ---------- candidates ---------- */
CAND *cands; UINTN ncand; CHAR16 *g_romdir;
static EFI_FILE_HANDLE g_root; static EFI_FILE_HANDLE g_vol_root;   /* g_vol_root: volume the .efi was started from */

static UINT16 rd16(const UINT8 *p) { return p[0] | (p[1] << 8); }

static CHAR16 *own_dir(EFI_LOADED_IMAGE *li)
{
  CHAR16 *last = NULL; EFI_DEVICE_PATH *dp = li->FilePath;
  for (; dp && !IsDevicePathEnd(dp); dp = NextDevicePathNode(dp))
    if (DevicePathType(dp) == MEDIA_DEVICE_PATH && DevicePathSubType(dp) == MEDIA_FILEPATH_DP)
      last = ((FILEPATH_DEVICE_PATH *)dp)->PathName;
  if (!last) return StrDuplicate(L"");
  CHAR16 *d = StrDuplicate(last); INTN i = (INTN)StrLen(d) - 1;
  while (i >= 0 && d[i] != L'\\') i--;
  if (i < 0) d[0] = 0; else d[i] = 0;              /* drop file name */
  return d;
}

static BOOLEAN ends_rom(const CHAR16 *n)
{
  UINTN l = StrLen(n);
  return l > 4 && (n[l-4] == L'.') && (n[l-3] | 0x20) == L'r' && (n[l-2] | 0x20) == L'o' && (n[l-1] | 0x20) == L'm';
}

static EFI_GUID gSfs = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static UINTN g_nvol, g_ndirs; CHAR16 *g_owndir;

static BOOLEAN open_dir(EFI_FILE_HANDLE root, const CHAR16 *path, EFI_FILE_HANDLE *dh)
{
  return !EFI_ERROR(FW(root->Open, root, dh, (CHAR16 *)(path[0] ? path : L"\\"), EFI_FILE_MODE_READ, 0));
}

/* add every *.rom in root:path to the candidate list; remember volume+dir on first hit */
static UINTN add_dir_roms(EFI_FILE_HANDLE root, CHAR16 *path)
{
  EFI_FILE_HANDLE dh; UINTN added = 0;
  g_ndirs++;
  if (!open_dir(root, path, &dh)) return 0;
  UINT8 *buf = AllocatePool(1024);
  for (;;) {
    UINTN sz = 1024;
    if (EFI_ERROR(FW(dh->Read, dh, &sz, buf)) || !sz) break;
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
    if ((fi->Attribute & EFI_FILE_DIRECTORY) || !ends_rom(fi->FileName)) continue;
    if (StrLen(fi->FileName) >= 70 || ncand >= 256) continue;
    StrCpy(cands[ncand].name, fi->FileName); ncand++; added++;
  }
  FW(dh->Close, dh); FreePool(buf);
  if (added) { g_root = root; g_romdir = StrDuplicate(path); }
  return added;
}

/* depth-limited search for a directory named "vbioses" */
static UINTN search_tree(EFI_FILE_HANDLE root, CHAR16 *path, UINTN depth)
{
  EFI_FILE_HANDLE dh; UINTN found = 0;
  if (g_ndirs > 200 || !open_dir(root, path, &dh)) return 0;
  g_ndirs++;
  UINT8 *buf = AllocatePool(1024);
  CHAR16 (*subs)[72] = AllocateZeroPool(sizeof(CHAR16) * 72 * 40); UINTN ns = 0;
  for (;;) {
    UINTN sz = 1024;
    if (EFI_ERROR(FW(dh->Read, dh, &sz, buf)) || !sz) break;
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
    if (!(fi->Attribute & EFI_FILE_DIRECTORY) || fi->FileName[0] == L'.' || StrLen(fi->FileName) >= 70) continue;
    if (ns < 40) StrCpy(subs[ns++], fi->FileName);
  }
  FW(dh->Close, dh); FreePool(buf);
  for (UINTN i = 0; i < ns && !found; i++) {              /* a folder with the right name here? */
    if (!StriCmp(subs[i], L"vbioses")) {
      CHAR16 *sub = PoolPrint(L"%s\\%s", path, subs[i]);
      found = add_dir_roms(root, sub); FreePool(sub);
    }
  }
  for (UINTN i = 0; i < ns && !found && depth < 3; i++) {
    if (!StriCmp(subs[i], L"vbioses")) continue;
    CHAR16 *sub = PoolPrint(L"%s\\%s", path, subs[i]);
    found = search_tree(root, sub, depth + 1); FreePool(sub);
  }
  FreePool(subs);
  return found;
}

static EFI_LOADED_IMAGE *g_li;
static void init_volume(EFI_HANDLE image)             /* find the volume + directory the .efi was started from */
{
  g_owndir = L""; g_vol_root = NULL;
  if (EFI_ERROR(FW(BS->HandleProtocol, image, &gEfiLoadedImageProtocolGuid, (VOID **)&g_li))) g_li = NULL;
  if (g_li) { g_vol_root = LibOpenRoot(g_li->DeviceHandle); if (g_vol_root) g_owndir = own_dir(g_li); }
}

static void scan_folder(void)
{
  EFI_HANDLE *vols = NULL; UINTN nv = 0;
  cands = AllocateZeroPool(sizeof(CAND) * 256); ncand = 0;
  FW(BS->LocateHandleBuffer, ByProtocol, &gSfs, NULL, &nv, &vols);
  g_nvol = nv;
  if (g_vol_root) {                                    /* 1) own volume: rom_dir=, next to the .efi, then usual places */
    EFI_FILE_HANDLE root = g_vol_root;
    CHAR16 *p[7]; UINTN np = 0;
    if (g_cfg_romdir[0]) p[np++] = StrDuplicate(g_cfg_romdir);
    p[np++] = PoolPrint(L"%s\\vbioses", g_owndir); p[np++] = PoolPrint(L"\\EFI\\%s\\vbioses", LOADER_DIRNAME);
    p[np++] = StrDuplicate(L"\\EFI\\vbios\\vbioses");          /* layout of the first beta */
    p[np++] = StrDuplicate(L"\\vbioses"); p[np++] = StrDuplicate(L"\\EFI\\vbioses"); p[np++] = StrDuplicate(L"\\EFI\\BOOT\\vbioses");
    for (UINTN t = 0; t < np && !ncand; t++) add_dir_roms(root, p[t]);
    if (!ncand) search_tree(root, L"", 0);
  }
  for (UINTN v = 0; v < nv && !ncand; v++) {           /* 2) search every other FAT volume */
    EFI_FILE_HANDLE root = LibOpenRoot(vols[v]);
    if (root) search_tree(root, L"", 0);
  }
  for (UINTN i = 1; i < ncand; i++) {                  /* sort by name, case-insensitive */
    CAND k = cands[i]; INTN j = (INTN)i - 1;
    while (j >= 0 && StriCmp(cands[j].name, k.name) > 0) { cands[j+1] = cands[j]; j--; }
    cands[j+1] = k;
  }
}

static UINT16 rd16(const UINT8 *p);
#define g_engine_atom (CFG(S_ENGINE) == ENG_ATOM)
/* read vendor/device id, size and (if present) the AtomBIOS header's subsystem id of every ROM file */
static void scan_meta(void)
{
  for (UINTN i = 0; i < ncand && g_root; i++) {
    CAND *c = &cands[i]; EFI_FILE_HANDLE f; CHAR16 *path = PoolPrint(L"%s\\%s", g_romdir, c->name);
    EFI_STATUS st = FW(g_root->Open, g_root, &f, path, EFI_FILE_MODE_READ, 0); FreePool(path);
    if (EFI_ERROR(st)) continue;
    EFI_FILE_INFO *fi = LibFileInfo(f); c->size = fi ? (UINT32)fi->FileSize : 0; if (fi) FreePool(fi);
    UINT8 hd[0x400]; UINTN n = sizeof hd;
    if (!EFI_ERROR(FW(f->Read, f, &n, hd)) && n >= 0x60 && hd[0] == 0x55 && hd[1] == 0xAA) {
      UINTN pcir = rd16(hd + 0x18);
      if (pcir + 8 <= n && !CompareMem(hd + pcir, "PCIR", 4)) { c->vid = rd16(hd + pcir + 4); c->did = rd16(hd + pcir + 6); c->ok = (c->vid == 0x1002 && c->size >= 0x1000 && c->size <= MAX_ROM_BYTES); }
      UINTN ah = rd16(hd + 0x48);
      if (ah + 0x18 + 4 <= n && !CompareMem(hd + ah + 4, "ATOM", 4)) { c->ssv = rd16(hd + ah + 0x14); c->ssi = rd16(hd + ah + 0x16); }
    }
    FW(f->Close, f);
  }
}

/* ---------- ROM parsing ---------- */
/* Walk option-ROM chain. Returns UEFI image (code type 3) or NULL; *end = real ROM length. */
static const UINT8 *parse_rom(const UINT8 *rom, UINTN size, UINT16 *vid, UINT16 *did, UINTN *end)
{
  UINTN off = 0; BOOLEAN first = TRUE; const UINT8 *efi = NULL; *end = 0;
  while (off + 0x1C <= size && rom[off] == 0x55 && rom[off+1] == 0xAA) {
    UINTN pcir = off + rd16(rom + off + 0x18);
    if (pcir + 0x18 > size || CompareMem((VOID *)(rom + pcir), "PCIR", 4)) break;
    if (first) { *vid = rd16(rom + pcir + 4); *did = rd16(rom + pcir + 6); first = FALSE; }
    UINTN len = (UINTN)rd16(rom + pcir + 0x10) * 512;
    if (!len || off + len > size) break;
    if (rom[pcir + 0x14] == 0x03 && !efi) efi = rom + off;
    off += len; *end = off;
    if (rom[pcir + 0x15] & 0x80) break;
  }
  return efi;
}

/* UEFI image -> raw PE (decompress with firmware protocol if flagged) */
static EFI_STATUS extract_pe(const UINT8 *img, VOID **pe, UINTN *pesz)
{
  UINT16 mach = rd16(img + 0x0A), comp = rd16(img + 0x0C), hdr = rd16(img + 0x16);
  UINTN init = (UINTN)rd16(img + 2) * 512;
  if (rd16(img + 4) != 0x0EF1) return EFI_UNSUPPORTED;
  if (mach != 0x8664) return EFI_UNSUPPORTED;                 /* x64 only */
  if (hdr >= init) return EFI_VOLUME_CORRUPTED;
  UINT8 *src = (UINT8 *)img + hdr; UINTN srclen = init - hdr;
  if (!comp) {
    *pe = AllocatePool(srclen); if (!*pe) return EFI_OUT_OF_RESOURCES;
    CopyMem(*pe, src, srclen); *pesz = srclen; return EFI_SUCCESS;
  }
  DECOMP *d; EFI_STATUS s = FW(BS->LocateProtocol, &gDecomp, NULL, (VOID **)&d);
  if (EFI_ERROR(s)) return s;
  UINT32 dsz, ssz;
  s = FW(d->GetInfo, d, src, (UINT32)srclen, &dsz, &ssz); if (EFI_ERROR(s)) return s;
  VOID *out = AllocatePool(dsz), *scr = AllocatePool(ssz);
  if (!out || !scr) return EFI_OUT_OF_RESOURCES;
  s = FW(d->Decompress, d, src, (UINT32)srclen, out, dsz, scr, ssz);
  FreePool(scr);
  if (EFI_ERROR(s)) return s;
  *pe = out; *pesz = dsz; return EFI_SUCCESS;
}

typedef struct { UINT8 *rom; UINTN romsz; VOID *pe; UINTN pesz; UINT16 vid, did; UINT8 *full; UINTN fullsz; } ROMDATA;

static EFI_STATUS get_rom(UINTN i, ROMDATA *r)
{
  ZeroMem(r, sizeof *r);
  const UINT8 *efi; UINTN end;
  CHAR16 *path = PoolPrint(L"%s\\%s", g_romdir, cands[i].name);
  EFI_FILE_HANDLE f; EFI_STATUS s = FW(g_root->Open, g_root, &f, path, EFI_FILE_MODE_READ, 0);
  FreePool(path); if (EFI_ERROR(s)) return s;
  EFI_FILE_INFO *fi = LibFileInfo(f);
  UINTN sz = fi ? (UINTN)fi->FileSize : 0; if (fi) FreePool(fi);
  if (sz < 0x1000 || sz > MAX_ROM_BYTES) { FW(f->Close, f); return EFI_BAD_BUFFER_SIZE; }
  UINT8 *buf = AllocatePool(sz); if (!buf) { FW(f->Close, f); return EFI_OUT_OF_RESOURCES; }
  s = FW(f->Read, f, &sz, buf); FW(f->Close, f);
  if (EFI_ERROR(s)) { FreePool(buf); return s; }
  efi = parse_rom(buf, sz, &r->vid, &r->did, &end);
  if (!end) { FreePool(buf); return EFI_VOLUME_CORRUPTED; }
  r->rom = buf; r->romsz = end; r->full = buf; r->fullsz = sz;     /* full = whole chip image incl. data past the BIOS images */
  if (g_engine_atom) {
    /* Atom mode never loads the ROM's UEFI GOP image. Do not decompress or execute it. */
    r->pe = NULL; r->pesz = 0;
    return EFI_SUCCESS;
  }
  if (!efi) return EFI_NOT_FOUND;
  return extract_pe(efi, &r->pe, &r->pesz);
}

/* ---------- ACPI VFCT (one image per card; the OS picks the image by PCI bus/dev/fn) ---------- */
typedef struct { BOOLEAN used; const UINT8 *rom; UINTN sz; UINT32 bus, dev, fn; UINT16 vid, did, ssv, ssi; } VENT;
static VENT g_vent[MAX_CARDS];
static BOOLEAN g_vfct_have; static UINTN g_vfct_key;
static EFI_STATUS vfct_publish(void)
{
  ACPITBL *at; EFI_STATUS s = FW(BS->LocateProtocol, &gAcpiTbl, NULL, (VOID **)&at);
  if (EFI_ERROR(s)) return s;
  UINTN hdrsz = 76, imgh = 28, total = hdrsz, nimg = 0;
  for (UINTN i = 0; i < MAX_CARDS; i++) if (g_vent[i].used) { total += imgh + g_vent[i].sz; nimg++; }
  if (g_vfct_have) { FW(at->Uninstall, at, g_vfct_key); g_vfct_have = FALSE; }
  if (!nimg) return EFI_SUCCESS;
  UINT8 *t = AllocateZeroPool(total); if (!t) return EFI_OUT_OF_RESOURCES;
  CopyMem(t, "VFCT", 4);
  *(UINT32 *)(t + 4) = (UINT32)total; t[8] = 1;
  CopyMem(t + 10, "AMDGPU", 6); CopyMem(t + 16, "VBIOSLDR", 8);
  *(UINT32 *)(t + 24) = 1; CopyMem(t + 28, "VLDR", 4); *(UINT32 *)(t + 32) = 1;
  *(UINT32 *)(t + 52) = (UINT32)hdrsz;
  UINT8 *h = t + hdrsz;
  for (UINTN i = 0; i < MAX_CARDS; i++) {
    VENT *e = &g_vent[i]; if (!e->used) continue;
    *(UINT32 *)(h + 0) = e->bus; *(UINT32 *)(h + 4) = e->dev; *(UINT32 *)(h + 8) = e->fn;
    *(UINT16 *)(h + 12) = e->vid; *(UINT16 *)(h + 14) = e->did;
    *(UINT16 *)(h + 16) = e->ssv; *(UINT16 *)(h + 18) = e->ssi;
    *(UINT32 *)(h + 24) = (UINT32)e->sz;
    CopyMem(h + imgh, (VOID *)e->rom, e->sz);
    h += imgh + e->sz;
  }
  UINT8 sum = 0; for (UINTN i = 0; i < total; i++) sum += t[i];
  t[9] = (UINT8)(0 - sum);
  UINTN key = 0; s = FW(at->Install, at, t, total, &key);
  if (!EFI_ERROR(s)) { g_vfct_key = key; g_vfct_have = TRUE; }
  FreePool(t); return s;
}
static void vfct_set(CARD *c, const UINT8 *rom, UINTN romsz, UINT16 vid, UINT16 did)
{
  UINTN k = (UINTN)(c - g_cards); if (k >= MAX_CARDS) return;
  VENT *e = &g_vent[k]; e->used = TRUE; e->rom = rom; e->sz = romsz; e->bus = (UINT32)c->bus; e->dev = (UINT32)c->dev; e->fn = (UINT32)c->fn;
  e->vid = vid; e->did = did; e->ssv = c->ssv; e->ssi = c->ssi;
}
static void vfct_clear(CARD *c) { UINTN k = (UINTN)(c - g_cards); if (k < MAX_CARDS) g_vent[k].used = FALSE; }

/* ---------- GPU register access + watchdog heartbeat ---------- */
static UINT64 g_mmio; static UINTN g_sec;
static BOOLEAN g_want_reboot;           /* reset_mode=reboot: caller must cold-reboot after a failed ROM */
static UINT32 g_last_mem;              /* MEMSIZE as printed by the last regs_line (no extra register read)
*/
static BOOLEAN g_last_skipped;          /* run_cand left an already-initialised card untouched */
static BOOLEAN g_force_once;            /* a manual run from the card menu: init even if the card looks initialised */

static UINT32 rreg(UINT32 off)           /* BAR5 = MMIO registers on Polaris; read via physical address */
{
  if (!g_mmio) return 0xDEADDEAD;
  return *(volatile UINT32 *)(UINTN)(g_mmio + off);
}
#define R_MEMSIZE 0x5428
#define R_GRBM    0x8010
#define R_SRBM    0x0E50
#define R_SCRATCH 0x1724
static void regs_line(const CHAR16 *tag)
{
  lg(L"  %s MEMSIZE=%08x GRBM=%08x SRBM=%08x SCRATCH0=%08x\n", tag,
        (g_last_mem = rreg(R_MEMSIZE)), rreg(R_GRBM), rreg(R_SRBM), rreg(R_SCRATCH));
}
/* ---------- embedded AtomBIOS interpreter (atomlib/atom.c, from the Linux amdgpu driver) ---------- */
struct card_info {
  void *dev;
  void (*reg_write)(struct card_info *, UINT32, UINT32);  UINT32 (*reg_read)(struct card_info *, UINT32);
  void (*mc_write)(struct card_info *, UINT32, UINT32);   UINT32 (*mc_read)(struct card_info *, UINT32);
  void (*pll_write)(struct card_info *, UINT32, UINT32);  UINT32 (*pll_read)(struct card_info *, UINT32);
};
struct atom_context;
extern struct atom_context *amdgpu_atom_parse(struct card_info *, void *);
extern int amdgpu_atom_asic_init(struct atom_context *);
extern void amdgpu_atom_destroy(struct atom_context *);
extern int atom_break_loops, atom_loops_broken; extern unsigned atom_loop_ms;
extern unsigned long atom_now_ms(void);
#define ATOM_LOOP_MS CFG(S_LOOP_MS)
#define g_force       (!CFG(S_SKIP_INIT))        /* init even if the GPU already looks fully initialised */
#define g_postdump    (CFG(S_POSTDUMP))          /* snapshot all registers after init (diagnostic; can itself freeze the PC) */

/* ---------- memory-controller microcode ----------
 * The SPI ROM chip also holds the MC sequencer firmware (polaris10_mc.bin payload) at ROM offset 0x37000:
 *   [ucode version, n_io_pairs, ucode_dwords, total_dwords] [n_io_pairs x (index,value)] [ucode dwords]
 * ASIC_Init's MC_SEQ_Control hands that offset to the GPU (SMC reg 0xC0600010) and the hardware pulls it from the
 * chip. With a dead chip nothing arrives and the MC never trains, so we do the hardware's job from the ROM file. */
static UINT32 g_smc_idx1;
static const UINT8 *g_mc_fallback; static UINTN g_mc_fallback_sz; static INTN g_mc_fallback_off;
static void mmio_w(UINT32 reg, UINT32 v) { UINT64 o = (UINT64)reg * 4; if (g_mmio && o + 4 <= 0x40000) *(volatile UINT32 *)(UINTN)(g_mmio + o) = v; }
static BOOLEAN mc_block_ok(const UINT8 *full, UINTN fullsz, UINT32 off)
{
  if (!full || (UINTN)off + 16 > fullsz) return FALSE;
  const UINT32 *h = (const UINT32 *)(full + off);
  UINT32 n_io = h[1], ucw = h[2], tot = h[3];
  if (n_io == 0 || n_io > 64 || ucw == 0 || ucw > 0x4000) return FALSE;
  if (16 + n_io * 8 + ucw * 4 != tot * 4) return FALSE;
  return (UINTN)off + tot * 4 <= fullsz;
}
/* offset of a valid MC block: the requested one first, then any 4 KB-aligned spot past the BIOS images; -1 if none */
static INTN mc_find(const UINT8 *full, UINTN fullsz, UINT32 preferred)
{
  if (mc_block_ok(full, fullsz, preferred)) return (INTN)preferred;
  for (UINTN o = 0x1D000; o + 0x1000 <= fullsz; o += 0x1000) if (mc_block_ok(full, fullsz, (UINT32)o)) return (INTN)o;
  return -1;
}
/* first ROM file in the folder that carries a valid MC block (your original dump sorts first) */
static void find_mc_fallback(void)
{
  static BOOLEAN tried;
  if (tried || !g_root) return;
  tried = TRUE;
  for (UINTN i = 0; i < ncand; i++) {
    CHAR16 *path = PoolPrint(L"%s\\%s", g_romdir, cands[i].name);
    EFI_FILE_HANDLE f; EFI_STATUS st = FW(g_root->Open, g_root, &f, path, EFI_FILE_MODE_READ, 0);
    FreePool(path);
    if (EFI_ERROR(st)) continue;
    EFI_FILE_INFO *fi = LibFileInfo(f); UINTN sz = fi ? (UINTN)fi->FileSize : 0; if (fi) FreePool(fi);
    if (sz < 0x38000 || sz > MAX_ROM_BYTES) { FW(f->Close, f); continue; }
    UINT8 *buf = AllocatePool(sz);
    if (!buf) { FW(f->Close, f); continue; }
    st = FW(f->Read, f, &sz, buf); FW(f->Close, f);
    INTN fo = EFI_ERROR(st) ? -1 : mc_find(buf, sz, 0x37000);
    if (fo >= 0) { g_mc_fallback = buf; g_mc_fallback_sz = sz; g_mc_fallback_off = fo; return; }
    FreePool(buf);
  }
}
/* ---------- emulation of the chip's SPI read port + register-copy engine ----------
 * What a normal boot does (decoded from the ROM tables, see README):
 *   1. SMC_IND_INDEX_1 = ROM_INDEX (0xC0600010); data = byte offset in the ROM  (here 0x37000)
 *   2. SMC_IND_INDEX_1 = ROM_DATA  (0xC0600014); every read of SMC_IND_DATA_1 returns the next dword (auto-increment)
 *      -> the tables read the 4-dword header and 24 (index,value) pairs and write them to MC_SEQ_IO_DEBUG
 *   3. regs 0xC064/0xC066 = source/destination register byte addresses (0x20C = SMC_IND_DATA_1, 0x28CC = MC_SEQ_SUP_PGM),
 *      reg 0xC0E8 = byte count (0x7E1C)  -> a hardware copy engine streams the microcode body ROM -> MC_SEQ_SUP_PGM
 *   4. MC_SEQ_SUP_CNTL = 8, 4, 1 starts the sequencer.
 * With a dead chip steps 2 and 3 move garbage. We serve both from the ROM file instead. */
static UINT8 *g_spi; static UINTN g_spi_sz; static UINT32 g_rom_idx; static UINT32 g_cp[4]; static UINTN g_cp_n; static UINT32 g_cp_src, g_cp_dst;
#define SPI_SIZE 0x40000
/* The ROM is already resident in RAM from get_rom(). Do not allocate/copy a
 * second 256 KiB SPI image here: on some UEFI memory maps that transition can
 * stall. The Atom interpreter only reads the emulated SPI image. */
static void spi_setup(const UINT8 *full, UINTN fullsz)
{
  lg(L"   [A5.1.1] SPI setup: reusing existing ROM buffer (%u bytes)\n", (UINT32)fullsz);
  g_spi = (UINT8 *)full;
  g_spi_sz = fullsz;
  g_rom_idx = 0; g_cp_n = 0; g_smc_idx1 = 0;
  lg(L"   [A5.1.2] SPI setup complete\n");
}
static UINT32 spi_next(void)
{
  UINT32 v = (g_rom_idx + 4 <= g_spi_sz) ? *(UINT32 *)(g_spi + g_rom_idx) : 0xFFFFFFFFu;
  g_rom_idx += 4; return v;
}
/* the ROM may ask for its MC block at an offset where this file has none (e.g. trimmed other-brand ROMs):
 * serve the block from another ROM file at the requested offset */
static void spi_overlay_if_needed(UINT32 off)
{
  if (off < 0x1D000 || off >= g_spi_sz || mc_block_ok(g_spi, g_spi_sz, off)) return;
  find_mc_fallback();
  if (!g_mc_fallback || g_mc_fallback_off < 0) { lg(L"   (ROM asks for data at 0x%x that this file lacks and no other file has the MC block)\n", off); return; }
  const UINT32 *h = (const UINT32 *)(g_mc_fallback + g_mc_fallback_off);
  UINTN bytes = (UINTN)h[3] * 4;
  if (off + bytes <= g_spi_sz) { CopyMem(g_spi + off, (VOID *)(g_mc_fallback + g_mc_fallback_off), bytes); lg(L"   (this ROM file lacks the MC block: serving the copy from another ROM file)\n"); }
}

static UINT32 c_rr(struct card_info *c, UINT32 reg)
{
  if (g_spi && reg == 0x83) {                                                /* SMC_IND_DATA_1 */
    if (g_smc_idx1 == 0xC0600014u) return spi_next();                        /* ROM_DATA  */
    if (g_smc_idx1 == 0xC0600010u) return g_rom_idx;                         /* ROM_INDEX */
  }
  UINT64 o = (UINT64)reg * 4; return (!g_mmio || o + 4 > 0x40000) ? 0 : *(volatile UINT32 *)(UINTN)(g_mmio + o);
}
static void   c_rw(struct card_info *c, UINT32 reg, UINT32 v)
{
  if (g_spi) {
    if (reg == 0x82) g_smc_idx1 = v;                                         /* SMC_IND_INDEX_1: forward too */
    else if (reg == 0x83 && g_smc_idx1 == 0xC0600010u) {                     /* ROM_INDEX <- byte offset */
      g_rom_idx = v; spi_overlay_if_needed(v); post_code(0x21); TRACE(L"ROM_INDEX <- %08x", v); return;
    }
    else if (reg >= 0xC064 && reg <= 0xC067) { g_cp[reg - 0xC064] = v; g_cp_n |= 1u << (reg - 0xC064); if (reg == 0xC064) g_cp_src = v; if (reg == 0xC066) g_cp_dst = v; return; }
    else if (reg == 0xC0E8) {
      UINT32 cnt = v & 0x03FFFFFFu;
      if (g_cp_src == 0x20C && g_cp_dst == 0x28CC && g_smc_idx1 == 0xC0600014u) {      /* ROM_DATA -> MC_SEQ_SUP_PGM */
        post_code(0x22); TRACE(L"copy engine: %d bytes ROM@%05x -> MC_SEQ_SUP_PGM", cnt, g_rom_idx);
        for (UINT32 i = 0; i < cnt / 4; i++) mmio_w(0xA33, spi_next());
        post_code(0x23); TRACE(L"copy engine: done");
        lg(L"   MC microcode: copy engine emulated, %d bytes from ROM offset 0x%x into MC_SEQ_SUP_PGM\n", cnt, g_rom_idx - cnt);
        g_cp_n = 0; return;
      }
      for (UINTN i = 0; i < 4; i++) if (g_cp_n & (1u << i)) mmio_w(0xC064 + (UINT32)i, g_cp[i]);   /* some other use: let hardware do it */
      g_cp_n = 0;
    }
  }
  mmio_w(reg, v);
}
static UINT32 c_ir(struct card_info *c, UINT32 reg) { return 0; }                 /* MC / PLL indirect: stubs, as in amdgpu */
static void   c_iw(struct card_info *c, UINT32 reg, UINT32 v) { }


/* ---- what Linux's amdgpu looks at when it starts (vi.c / amdgpu_atombios.c) ---- */
#define SMC_IND_INDEX_0   0x80
#define SMC_IND_DATA_0    0x81
#define ixSMC_RESET_CNTL  0x80000000u
#define ixSMC_CLOCK_CNTL0 0x80000004u
#define ixSMC_PC_C        0x80000370u
#define BIOS_SCRATCH_7    0x5D0
#define S7_INIT_COMPLETE  0x200u
static UINT32 smc_rd(struct card_info *c, UINT32 a) { c_rw(c, SMC_IND_INDEX_0, a); return c_rr(c, SMC_IND_DATA_0); }
static void   smc_wr(struct card_info *c, UINT32 a, UINT32 v) { c_rw(c, SMC_IND_INDEX_0, a); c_rw(c, SMC_IND_DATA_0, v); }
static BOOLEAN smc_running(struct card_info *c) { return !(smc_rd(c, ixSMC_CLOCK_CNTL0) & 1) && smc_rd(c, ixSMC_PC_C) >= 0x20100; }

/* Linux resets the whole GPU at probe if the SMC looks running ("PCI CONFIG reset"), which would wipe our init.
 * Make the GPU look "initialised, SMC stopped" instead. */
static void prepare_for_os(struct card_info *c)
{
  post_code(0x60); TRACE(L"prepare_for_os");
  UINT32 misc0 = c_rr(c, 0xA80), s4 = c_rr(c, 0x5CD);               /* MC_SEQ_MISC0, BIOS_SCRATCH_4 */
  lg(L"   MC_SEQ_MISC0=%08x BIOS_SCRATCH_4=%08x   (reference: your Samsung SMD2 original gives 5060A1F2 / 00010000)\n", misc0, s4);
  UINT32 clk = smc_rd(c, ixSMC_CLOCK_CNTL0), pc = smc_rd(c, ixSMC_PC_C), s7 = c_rr(c, BIOS_SCRATCH_7);
  lg(L"   OS-visible state: SMC clock_cntl0=%08x pc=%08x  BIOS_SCRATCH_7=%08x\n", clk, pc, s7);
  if (smc_running(c)) {
    lg(L"   SMC looks RUNNING -> Linux would reset the GPU at probe and discard our init. Halting the SMC.\n");
    smc_wr(c, ixSMC_RESET_CNTL, smc_rd(c, ixSMC_RESET_CNTL) | 1);
    smc_wr(c, ixSMC_CLOCK_CNTL0, smc_rd(c, ixSMC_CLOCK_CNTL0) | 1);
    lg(L"   after halt: SMC clock_cntl0=%08x pc=%08x  running=%s\n", smc_rd(c, ixSMC_CLOCK_CNTL0), smc_rd(c, ixSMC_PC_C), smc_running(c) ? L"YES" : L"no");
  }
  s7 = c_rr(c, BIOS_SCRATCH_7);
  lg(L"   ASIC_INIT_COMPLETE flag (BIOS_SCRATCH_7 bit 9): %s -> Linux will %s the GPU\n",
        (s7 & S7_INIT_COMPLETE) ? L"SET" : L"NOT set", (s7 & S7_INIT_COMPLETE) ? L"SKIP POSTing" : L"POST again (and stall at SetVoltage)");
}


/* MC sequencer status, printed whenever a stuck poll is broken (see MC_SEQ_Control analysis) */
#define R_MC_SEQ_MISC9   0xAE7
#define R_MC_SEQ_SUPCNTL 0xA32
#define R_MC_SEQ_CMD     0xA31
#define R_MC_SEQ_STATUSM 0xA7D
#define R_MC_SEQ_MISC0   0xA80
#define R_MC_SEQ_MISC1   0xA81
#define R_MC_TRAIN_WAKE  0xA3A      /* good/trained value: C00000E0 */
void loader_loop_diag(void)
{
  post_code(0x30);
  TRACE(L"loop broken: MISC9=%08x SUP_CNTL=%08x CMD=%08x STATUS_M=%08x WAKE=%08x", c_rr(NULL, R_MC_SEQ_MISC9), c_rr(NULL, R_MC_SEQ_SUPCNTL), c_rr(NULL, R_MC_SEQ_CMD), c_rr(NULL, R_MC_SEQ_STATUSM), c_rr(NULL, R_MC_TRAIN_WAKE));
  lg(L"      MC: MISC9=%08x SUP_CNTL=%08x CMD=%08x STATUS_M=%08x WAKE=%08x\n"
        L"          (trained = MISC9 11000707, SUP_CNTL 27800001, CMD 00030000, STATUS_M 00010300, WAKE C00000E0)\n",
        c_rr(NULL, R_MC_SEQ_MISC9), c_rr(NULL, R_MC_SEQ_SUPCNTL), c_rr(NULL, R_MC_SEQ_CMD),
        c_rr(NULL, R_MC_SEQ_STATUSM), c_rr(NULL, R_MC_TRAIN_WAKE));
}

/* ---------- reliable reset (ResetSystem can deadlock inside a timer callback) ---------- */
static void outb(UINT16 port, UINT8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static void hard_reset(void)
{
  outb(0xCF9, 0x02); FW(BS->Stall, 100);       /* chipset reset-control register */
  outb(0xCF9, 0x0E); FW(BS->Stall, 300000);
  outb(0x64, 0xFE);  FW(BS->Stall, 300000);    /* keyboard-controller reset line */
  FW(RT->ResetSystem, EfiResetCold, EFI_SUCCESS, 0, NULL);
  FW(BS->Stall, 1000000);
  struct { UINT16 l; UINT64 b; } __attribute__((packed)) idt = { 0, 0 };   /* last resort: triple fault */
  __asm__ volatile("lidt %0; int3" :: "m"(idt));
}

static EFI_HANDLE g_image; static UINTN g_cur;
static BOOLEAN g_fired;

static void __attribute__((ms_abi)) heartbeat(EFI_EVENT e, VOID *c)
{
  g_sec++;
  if (!(g_sec & 1)) { CHAR16 t[16]; SPrint(t, sizeof t, L"[%ds]", g_sec); regs_line(t); }
  if (g_sec >= TIMEOUT_SECS && !g_fired) {
    g_fired = TRUE;
    lg(L"\n  TIMEOUT: firmware GOP driver did not finish in %d s -> resetting the whole machine\n", TIMEOUT_SECS);
    FW(BS->Stall, 2000000);
    hard_reset();
  }
}

/* AMD "PCI config reset": magic dword to config offset 0x7C resets the ASIC only (same as amdgpu/vi.c) */
static BOOLEAN gpu_reset(EFI_PCI_IO_PROTOCOL *pio)
{
  UINT16 cmd = 0, nobm; UINT32 key = 0x39D5E86B; BOOLEAN ok = FALSE;
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint16, 4, 1, &cmd);
  nobm = cmd & ~0x4;
  FW(pio->Pci.Write, pio, EfiPciIoWidthUint16, 4, 1, &nobm);          /* bus master off */
  FW(pio->Pci.Write, pio, EfiPciIoWidthUint32, 0x7C, 1, &key);        /* reset */
  FW(BS->Stall, 2000);
  for (UINTN i = 0; i < 3000 && !ok; i++) {                           /* wait up to ~3 s for the ASIC to answer */
    if (rreg(R_MEMSIZE) != 0xFFFFFFFF) { FW(BS->Stall, 1000); ok = (rreg(R_MEMSIZE) != 0xFFFFFFFF); }
    else FW(BS->Stall, 1000);
  }
  FW(pio->Pci.Write, pio, EfiPciIoWidthUint16, 4, 1, &cmd);           /* bus master back on */
  return ok;
}

/* PCIe secondary-bus reset of the card's upstream bridge (reset_mode=pcie). Config header is saved/restored around it. */
static BOOLEAN pcie_reset(CARD *c)
{
  UINTN n = 0; EFI_HANDLE *hs = NULL; EFI_PCI_IO_PROTOCOL *br = NULL;
  if (EFI_ERROR(FW(BS->LocateHandleBuffer, ByProtocol, &gEfiPciIoProtocolGuid, NULL, &n, &hs))) return FALSE;
  for (UINTN i = 0; i < n && !br; i++) {
    EFI_PCI_IO_PROTOCOL *p; UINT8 cl[2], sec = 0, hdr = 0;
    if (EFI_ERROR(FW(BS->HandleProtocol, hs[i], &gEfiPciIoProtocolGuid, (VOID **)&p))) continue;
    FW(p->Pci.Read, p, EfiPciIoWidthUint8, 0x0A, 2, cl); FW(p->Pci.Read, p, EfiPciIoWidthUint8, 0x0E, 1, &hdr);
    if ((hdr & 0x7F) != 1 || cl[1] != 0x06 || cl[0] != 0x04) continue;
    FW(p->Pci.Read, p, EfiPciIoWidthUint8, 0x19, 1, &sec);
    if (sec == c->bus) br = p;
  }
  FreePool(hs);
  if (!br) { lg(L"   no upstream PCIe bridge found for bus %02x\n", (UINT32)c->bus); return FALSE; }
  UINT32 cfg[16]; FW(c->pio->Pci.Read, c->pio, EfiPciIoWidthUint32, 0, 16, cfg);
  UINT16 bc = 0; FW(br->Pci.Read, br, EfiPciIoWidthUint16, 0x3E, 1, &bc);
  UINT16 on = bc | 0x40, off = bc & ~0x40;
  FW(br->Pci.Write, br, EfiPciIoWidthUint16, 0x3E, 1, &on);   FW(BS->Stall, 5000);
  FW(br->Pci.Write, br, EfiPciIoWidthUint16, 0x3E, 1, &off);  FW(BS->Stall, 200000);
  BOOLEAN up = FALSE;
  for (UINTN i = 0; i < 40 && !up; i++) { UINT16 v = 0xFFFF; FW(c->pio->Pci.Read, c->pio, EfiPciIoWidthUint16, 0, 1, &v); if (v == AMD_VID) up = TRUE; else FW(BS->Stall, 50000); }
  if (!up) return FALSE;
  for (UINTN i = 4; i < 16; i++) FW(c->pio->Pci.Write, c->pio, EfiPciIoWidthUint32, i * 4, 1, &cfg[i]);   /* BARs, ROM BAR, irq ... */
  FW(c->pio->Pci.Write, c->pio, EfiPciIoWidthUint32, 0x04, 1, &cfg[1]);                                   /* command register last */
  for (UINTN i = 0; i < 3000; i++) { if (rreg(R_MEMSIZE) != 0xFFFFFFFF) return TRUE; FW(BS->Stall, 1000); }
  return FALSE;
}
/* what happens after a ROM failed to bring the card up: reset_mode setting */
static BOOLEAN card_reset(CARD *c)
{
  switch (CFG(S_RESET_MODE)) {
    case RS_NONE:   lg(L"   reset_mode=none: no reset, trying next ROM directly\n"); return TRUE;
    case RS_REBOOT: lg(L"   reset_mode=reboot: the whole PC will be cold-rebooted, then the next ROM is tried\n"); g_want_reboot = TRUE; return TRUE;
    case RS_PCIE:   { lg(L"   PCIe secondary-bus reset of the card...\n"); if (pcie_reset(c)) return TRUE; lg(L"   PCIe reset did not bring the card back, falling back to the AMD config reset\n"); }
    /* fall through */
    default:        return gpu_reset(c->pio);
  }
}

static EFI_STATUS dump_to(const CHAR16 *name, BOOLEAN progress);
static BOOLEAN reg_is_hazard(UINTN i);
/* ---------- running one candidate ---------- */
enum { RES_OK, RES_SKIP, RES_SYSTEMIC, RES_FAIL, RES_VFCT, RES_HUNG, RES_DEAD };

static BOOLEAN card_reset(CARD *c);
static UINTN run_cand(CARD *c, UINTN idx, BOOLEAN vfct_only, EFI_HANDLE image)
{
  EFI_STATUS s; ROMDATA r;
  lg(L"\n>> ROM %d/%d: %s\n", idx + 1, ncand, cands[idx].name);
  lg(L"   [A0] ROM load: opening file...\n");
  TRACE(L"ROM %d/%d %s", idx + 1, ncand, cands[idx].name); post_code(0x10);
  s = get_rom(idx, &r);
  if (EFI_ERROR(s)) { lg(L"   [A0] get_rom FAILED: %r\n", s); if (idx < NRES) { g_st.res[idx] = 2; state_save(); } return RES_SKIP; }
  lg(L"   [A1] ROM loaded: %u bytes, parsed BIOS image %u bytes\n", (UINT32)r.fullsz, (UINT32)r.romsz);
  if (r.vid != AMD_VID) { lg(L"   not an AMD ROM\n"); return RES_SKIP; }
  if (r.did != c->did) { lg(L"   ROM is for device %04x, this card is %04x\n", r.did, c->did); return RES_SKIP; }

  EFI_HANDLE gpu = c->h; EFI_PCI_IO_PROTOCOL *pio = c->pio;
  UINTN seg, bus, dev, fn; UINT16 ssv, ssi; UINT32 bar5 = 0;
  lg(L"   [A2] ROM accepted; PCI config only...\n");
  FW(pio->GetLocation, pio, &seg, &bus, &dev, &fn);
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint16, 0x2C, 1, &ssv);
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint16, 0x2E, 1, &ssi);
  FW(pio->Attributes, pio, EfiPciIoAttributeOperationEnable,
     EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);
  FW(pio->Pci.Read, pio, EfiPciIoWidthUint32, 0x24, 1, &bar5);
  g_mmio = (bar5 & 1) ? 0 : (bar5 & ~0xFu);
  lg(L"   GPU %02x:%02x.%x  %04x:%04x  subsys %04x:%04x  MMIO BAR5=%08x\n",
        bus, dev, fn, AMD_VID, r.did, ssv, ssi, (UINT32)g_mmio);

  TRACE(L"GPU %02x:%02x.%x mmio=%08x", bus, dev, fn, (UINT32)g_mmio); post_code(0x11);
  lg(L"   [A3] PCI/BAR setup complete; MMIO address=%08x\n", (UINT32)g_mmio);
  if (g_engine_atom) {
    /* Atom mode deliberately does not touch PciIo->RomImage or install VFCT before ASIC_Init. */
    if (vfct_only) { lg(L"   VFCT-only requested, but Atom mode requires ASIC_Init first.\n"); }
  } else {
    pio->RomImage = r.rom; pio->RomSize = r.romsz;
    vfct_set(c, r.rom, r.romsz, AMD_VID, r.did); s = vfct_publish();
    lg(L"   VFCT table: %r\n", s); post_code(0x12);
    if (vfct_only) { lg(L"   VFCT-only mode: GOP driver skipped\n"); return RES_VFCT; }
  }

  if (g_engine_atom) {
    /* Do not allocate/copy a second full-size ROM buffer here. The ROM is already
     * resident in RAM from get_rom(). A second large pool allocation was introduced
     * by the v2.1 path and can hang/fail on constrained UEFI memory maps before the
     * Atom interpreter even starts. Use the existing buffer whenever possible; only
     * allocate a tiny padding tail if the file ends exactly at the parsed BIOS image. */
    UINT8 *bios = r.rom;
    UINT8 *bios_pad = NULL;
    UINTN bios_avail = r.fullsz - (UINTN)(r.rom - r.full);
    if (bios_avail < r.romsz) {
      lg(L"   [A4] invalid ROM buffer bounds (avail=%u romsz=%u)\n", (UINT32)bios_avail, (UINT32)r.romsz);
      return RES_SKIP;
    }
    if (bios_avail < r.romsz + 0x4000) {
      bios_pad = AllocatePool(r.romsz + 0x4000);
      if (!bios_pad) { lg(L"   [A4] small padded AtomBIOS allocation failed\n"); return RES_SKIP; }
      CopyMem(bios_pad, r.rom, r.romsz);
      SetMem(bios_pad + r.romsz, 0x4000, 0);
      bios = bios_pad;
      lg(L"   [A4] ROM needed padding; allocated %u-byte padded view\n", (UINT32)(r.romsz + 0x4000));
    } else {
      lg(L"   [A4] using existing ROM buffer; no second ROM allocation/copy\n");
    }
    struct card_info card = { NULL, c_rw, c_rr, c_iw, c_ir, c_iw, c_ir };
    lg(L"   [A4] AtomBIOS parsing tables in RAM only...\n");
    struct atom_context *actx = amdgpu_atom_parse(&card, bios);
    if (!actx) { lg(L"   AtomBIOS parse failed (not a usable ROM)\n"); FreePool(bios); if (idx < NRES) { g_st.res[idx] = 2; state_save(); } return RES_SKIP; }
    lg(L"   [A5] AtomBIOS parsed. NO GPU MMIO probing will occur before ASIC_Init.\n");
    /* Do not inspect MC/MEMSIZE/SCRATCH or attempt initialized-state detection here. */
    /* Do not perform an EFI Runtime SetVariable while the GPU is being brought up. */
    if (idx < NRES) { g_st.res[idx] = 1; }
    lg(L"   [A5.1] preparing SPI/MC emulation...\n");
    spi_setup(r.full, r.fullsz);
    if (!g_spi) { lg(L"   [A5] SPI emulation buffer allocation failed\n"); amdgpu_atom_destroy(actx); if (bios_pad) FreePool(bios_pad); return RES_SKIP; }
    lg(L"   [A6] SPI/MC ROM emulation prepared.\n");
    atom_break_loops = 1; atom_loop_ms = ATOM_LOOP_MS; atom_loops_broken = 0;
    lg(L"   [A7] ASIC_Init STARTING NOW. First GPU MMIO access may occur inside the Atom interpreter.\n");
    lg(L"   running ASIC_Init in the built-in interpreter (a poll stuck > %d ms is skipped)...\n", ATOM_LOOP_MS);
    TRACE(L"ASIC_Init start"); post_code(0x20);
    unsigned long t0 = atom_now_ms();
    int rc = amdgpu_atom_asic_init(actx);
    lg(L"   [A8] ASIC_Init RETURNED: rc=%d, loops skipped=%d\n", rc, atom_loops_broken);
    post_code(0x40); TRACE(L"ASIC_Init returned %d, %d loops skipped", rc, atom_loops_broken);
    g_spi = NULL; g_spi_sz = 0;                                                    /* back to the real hardware */
    post_code(0x41); TRACE(L"ASIC_Init complete; leaving Atom path");
    unsigned long dt = atom_now_ms() - t0;
    BOOLEAN s7_ok = (c_rr(NULL, BIOS_SCRATCH_7) & S7_INIT_COMPLETE) != 0;
    lg(L"   ASIC_Init returned %d after %d ms, %d stuck loop(s) skipped\n", rc, (UINTN)dt, atom_loops_broken);
    /* Do not perform the broad post-init register probe here.  The GPU is now alive,
     * but GRBM/SRBM/SCRATCH reads are not required for continuing to the next card and
     * some power-gated blocks can stall a UEFI MMIO read indefinitely. */
    lg(L"   [A8.1] destroying Atom context...\n");
    post_code(0x42);
    amdgpu_atom_destroy(actx);
    post_code(0x43);
    if (bios_pad) { FreePool(bios_pad); bios_pad = NULL; }
    if (g_postdump) { post_code(0x50); TRACE(L"post-init dump start"); EFI_STATUS ds = dump_to(L"regdump_postinit.bin", FALSE); TRACE(L"post-init dump done"); lg(L"   register snapshot after init -> regdump_postinit.bin: %r\n", ds); }
    BOOLEAN good = (rc == 0) && s7_ok;
    if (idx < NRES) { g_st.res[idx] = good ? (atom_loops_broken ? 6 : 4) : 3; g_st.loops[idx] = (UINT8)(atom_loops_broken > 255 ? 255 : atom_loops_broken); state_save(); }
    if (good) {
      if (atom_loops_broken) lg(L"   NOTE: %d polling loop(s) never completed and were skipped (see SetVoltage analysis);\n"
                                   L"         the GPU may be running at its regulator's default voltage.\n", atom_loops_broken);
      lg(L"   [A8.2] remembering/publishing initialized card...\n");
      post_code(0x44);
      vfct_set(c, r.rom, r.romsz, AMD_VID, r.did);
      post_code(0x44);
      lg(L"   [A8.2.1] VFCT entry stored; deferring ACPI VFCT publication until all cards are initialized.\n");
      /* Do not uninstall/install the ACPI VFCT table while iterating cards.  On
       * some firmware this ACPI protocol operation can block after a GPU has
       * just been initialized.  The entry is retained in g_vent and published
       * once, after run_all() has finished. */
      post_code(0x45);
      lg(L"   [A8.3] preparing GPU for OS handoff...\n");
      prepare_for_os(&card);
      post_code(0x46);
      lg(L"   [A8.4] card initialization complete; returning to multi-card loop.\n");
      post_code(0x47);
      return RES_OK;
    }
    prepare_for_os(&card);
    lg(L"   init did not complete -> resetting only the GPU\n");
    BOOLEAN up = card_reset(c);
    regs_line(L"post-reset:");
    if (!up) { lg(L"   GPU does not answer after reset\n"); return RES_DEAD; }
    return RES_HUNG;                                              /* next ROM in the same boot, no reboot */
  }


  EFI_HANDLE drv;
  s = FW(BS->LoadImage, FALSE, image, NULL, r.pe, r.pesz, &drv);
  lg(L"   LoadImage: %r\n", s);
  if (EFI_ERROR(s)) return RES_FAIL;
  s = FW(BS->StartImage, drv, NULL, NULL);
  lg(L"   StartImage: %r\n", s);
  if (EFI_ERROR(s)) return RES_FAIL;

  regs_line(L"before:");
  g_cur = idx; g_fired = FALSE; g_image = image;
  if (idx < NRES) { g_st.res[idx] = 1; g_st.membefore[idx] = rreg(R_MEMSIZE); state_save(); }
  lg(L"   ConnectController (GPU init, timeout %d s)...\n", TIMEOUT_SECS);
  EFI_EVENT hb; g_sec = 0;
  FW(BS->CreateEvent, EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, (EFI_EVENT_NOTIFY)heartbeat, NULL, &hb);
  FW(BS->SetTimer, hb, TimerPeriodic, 10000000);
  s = FW(BS->ConnectController, gpu, NULL, NULL, TRUE);
  FW(BS->SetTimer, hb, TimerCancel, 0); FW(BS->CloseEvent, hb);
  lg(L"   ConnectController: %r\n", s);
  regs_line(L"after: ");

  VOID *gop = NULL; UINT32 mem = rreg(R_MEMSIZE);
  BOOLEAN has_gop = !EFI_ERROR(FW(BS->HandleProtocol, gpu, &GraphicsOutputProtocol, &gop));
  BOOLEAN mem_ok = (mem >= 256 && mem <= 32768);
  lg(L"   GOP on GPU: %s   VRAM size register: %s\n", has_gop ? L"yes" : L"no", mem_ok ? L"valid" : L"invalid");
  BOOLEAN good = !EFI_ERROR(s) && (has_gop || mem_ok);
  if (idx < NRES) { g_st.res[idx] = good ? 4 : 3; state_save(); }
  return good ? RES_OK : RES_FAIL;
}


/* ---------- register dump for offline comparison (dead ROM chip vs working ROM chip) ---------- */
static CHAR16 *own_path(const CHAR16 *name) { return PoolPrint(L"%s\\%s", g_owndir ? g_owndir : L"", name); }

/* Registers that hang the CPU when read on a cold GPU (block is clock/power gated and never answers).
 * VCE (video encode): 0x8000..0x8FFF - found the hard way (reading 0x8001 froze the PC). */
static BOOLEAN reg_is_hazard(UINTN i) { return i >= 0x8000 && i < 0x9000; }

/* write all registers (minus hazards) to <name> on the loader's volume, chunk by chunk */
static EFI_STATUS dump_to(const CHAR16 *name, BOOLEAN progress)
{
  EFI_FILE_HANDLE root = g_vol_root ? g_vol_root : g_root;
  if (!root) return EFI_NOT_FOUND;
  CHAR16 *path = own_path(name); EFI_FILE_HANDLE f;
  EFI_STATUS s = FW(root->Open, root, &f, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  FreePool(path);
  if (EFI_ERROR(s)) return s;
  UINT32 *buf = AllocatePool(0x4000);
  if (!buf) { FW(f->Close, f); return EFI_OUT_OF_RESOURCES; }
  for (UINTN base = 0; base < 0x10000; base += 0x1000) {
    for (UINTN k = 0; k < 0x1000; k++) {
      UINTN i = base + k;
      if (progress && !(i & 0xFF)) lg(L"    reg 0x%04x   \r", (UINT32)i);
      if (!k) { post_code((UINT8)(0xD0 | (base >> 12))); TRACE(L"dump chunk 0x%04x", (UINT32)base); }   /* last number on screen = where it froze */
      buf[k] = reg_is_hazard(i) ? 0xBAD0BAD0u : c_rr(NULL, (UINT32)i);
    }
    UINTN sz = 0x4000; FW(f->Write, f, &sz, buf); FW(f->Flush, f);
  }
  /* indirect MC_SEQ_IO_DEBUG space (memory-PHY settings and training results live here, not in the 256 KB window):
   * index register 0xA91, data register 0xA92. Appended after the MMIO block: 0x400 dwords. */
  post_code(0xE0); TRACE(L"dump: MC IO_DEBUG indirect space");
  UINT32 saved_idx = c_rr(NULL, 0xA91);
  for (UINTN i = 0; i < 0x400; i++) { mmio_w(0xA91, (UINT32)i); buf[i & 0xFFF] = c_rr(NULL, 0xA92); }
  mmio_w(0xA91, saved_idx);
  { UINTN sz = 0x1000; FW(f->Write, f, &sz, buf); FW(f->Flush, f); }
  TRACE(L"dump: done");
  FW(f->Close, f); FreePool(buf);
  return EFI_SUCCESS;
}

static void dump_registers(CARD *c)
{
  if (!c->mmio) { lg(L"  Card has no usable BAR5.\n"); return; }
  g_mmio = c->mmio;
  UINT32 memsz = c_rr(NULL, 0x150A);
  CHAR16 name[64]; SPrint(name, sizeof name, L"regdump_%02x%02x%x_mem%08x.bin", (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn, memsz);
  lg(L"\n  Dumping GPU registers of %02x:%02x.%x to %s (state: MEMSIZE=%08x).\n"
     L"  Written chunk by chunk, so a freeze keeps what was read. If the PC freezes, note the last number.\n", c->bus, c->dev, c->fn, name, memsz);
  EFI_STATUS s = dump_to(name, !g_ui_gui);
  lg(L"\n  Saved %s : %r  (copy it off the EFI partition)\n", name, s);
}


/* ---------- breadcrumbs for hard freezes ----------
 * \loader_trace.txt on the EFI partition (flushed after every line) and the motherboard POST-code port 0x80
 * (shows on boards with a 2-digit Q-Code display). After a freeze the last line / last code is the last phase reached. */
static void post_code(UINT8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"((UINT16)0x80)); }
static EFI_FILE_HANDLE g_trace; static BOOLEAN g_trace_tried;
static void trace_write(const CHAR16 *msg)
{
  if (!CFG(S_TRACE)) return;
  if (!g_trace && !g_trace_tried) {
    g_trace_tried = TRUE;
    EFI_FILE_HANDLE root = g_vol_root ? g_vol_root : g_root, f;
    if (root) {
      CHAR16 *tp = own_path(L"loader_trace.txt");
      if (!EFI_ERROR(FW(root->Open, root, &f, tp, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) FW(f->Delete, f);   /* start a fresh file */
      if (!EFI_ERROR(FW(root->Open, root, &f, tp, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) g_trace = f;
      FreePool(tp);
    }
  }
  if (!g_trace) return;
  CHAR8 line[200]; UINTN n = 0;
  for (; msg[n] && n < 196; n++) line[n] = (CHAR8)(msg[n] < 0x80 ? msg[n] : '?');
  line[n++] = '\r'; line[n++] = '\n';
  UINTN sz = n; FW(g_trace->Write, g_trace, &sz, line); FW(g_trace->Flush, g_trace);
}

/* =====================================================================================================
 *  v0.2 control layer: logging, card detection (multi-GPU), ROM ordering, per-card AUTO, interactive pages
 *  The ASIC init path above (run_cand / AtomBIOS / SPI+MC emulation / prepare_for_os) is the proven v0.1 code.
 * ===================================================================================================== */
#include <stdarg.h>

/* ---------- log ring buffer (+ console in text mode) ---------- */
CHAR16 g_log[LOG_LINES][LOG_W]; UINTN g_nlog; CHAR16 g_activity[120]; BOOLEAN g_ui_gui;
static UINTN g_logcol;
void lg(const CHAR16 *fmt, ...)
{
  CHAR16 b[420]; va_list a;
  va_start(a, fmt); VSPrint(b, sizeof b, fmt, a); va_end(a);
  if (!g_ui_gui) Print(L"%s", b);
  for (CHAR16 *p = b; *p; p++) {
    if (*p == L'\r') continue;
    if (*p == L'\n') { if (g_logcol || 1) { g_log[g_nlog][g_logcol] = 0; g_nlog++; g_logcol = 0;
        if (g_nlog >= LOG_LINES) { CopyMem(g_log[0], g_log[1], sizeof(CHAR16) * LOG_W * (LOG_LINES - 1)); g_nlog = LOG_LINES - 1; }
        g_log[g_nlog][0] = 0; } continue; }
    if (g_logcol + 1 < LOG_W) { g_log[g_nlog][g_logcol++] = *p; g_log[g_nlog][g_logcol] = 0; }
  }
}
static void set_note(CARD *c, const CHAR16 *fmt, ...)
{ va_list a; va_start(a, fmt); VSPrint(c->note, sizeof c->note, fmt, a); va_end(a); }
static void set_activity(const CHAR16 *fmt, ...)
{ va_list a; va_start(a, fmt); VSPrint(g_activity, sizeof g_activity, fmt, a); va_end(a); ui_progress(); }

/* ---------- brands: subsystem vendor -> name used to prefer ROM files (smart_order) ---------- */
static const struct { UINT16 id; const CHAR16 *show; const CHAR16 *key; } g_brands[] = {
  {0x1682, L"XFX", L"xfx"}, {0x1458, L"Gigabyte", L"gigabyte"}, {0x1043, L"ASUS", L"asus"}, {0x1462, L"MSI", L"msi"},
  {0x148c, L"PowerColor", L"powercolor"}, {0x174b, L"Sapphire", L"sapphire"}, {0x1da2, L"Sapphire", L"sapphire"},
  {0x1849, L"ASRock", L"asrock"}, {0x1002, L"AMD", L"amd"}, {0x1787, L"HIS", L"his"}, {0x19da, L"Zotac", L"zotac"},
  {0x1569, L"Palit", L"palit"}, {0x196d, L"Club3D", L"club3d"}, {0x7377, L"Colorful", L"colorful"}, {0x1b4b, L"Dataland", L"dataland"},
};
const CHAR16 *brand_of(UINT16 ssv) { for (UINTN i = 0; i < sizeof g_brands / sizeof g_brands[0]; i++) if (g_brands[i].id == ssv) return g_brands[i].show; return NULL; }
static BOOLEAN name_has(const CHAR16 *n, const CHAR16 *key)
{
  for (UINTN i = 0; n[i]; i++) { UINTN k = 0; while (key[k] && n[i+k] && (n[i+k] | 0x20) == key[k]) k++; if (!key[k]) return TRUE; }
  return FALSE;
}
static UINTN rom_score(CARD *c, CAND *r)
{
  if (!CFG(S_SMART_ORDER)) return 0;
  if (c->ssv && r->ssv == c->ssv && r->ssi == c->ssi) return 3;
  for (UINTN i = 0; i < sizeof g_brands / sizeof g_brands[0]; i++) if (g_brands[i].id == c->ssv && name_has(r->name, g_brands[i].key)) return 2;
  return 0;
}

/* ---------- per-card ROM order ---------- */
static UINTN g_order[MAX_CARDS][256], g_norder[MAX_CARDS];
static UINTN find_by_name(const CHAR16 *n)
{
  for (UINTN i = 0; i < ncand; i++) if (!StrCmp(cands[i].name, (CHAR16 *)n)) return i;
  return (UINTN)-1;
}
static UINT32 fnv(UINT32 h, const void *p, UINTN n) { const UINT8 *b = p; while (n--) { h ^= *b++; h *= 16777619u; } return h; }
static void card_prepare(CARD *c)
{
  UINTN k = (UINTN)(c - g_cards), n = 0;
  for (UINTN sc = 3; sc <= 3; sc--) {                       /* best score first, name order inside a score */
    for (UINTN i = 0; i < ncand && n < 256; i++) if (cands[i].ok && cands[i].did == c->did && rom_score(c, &cands[i]) == sc) g_order[k][n++] = i;
    if (sc == 0) break;
  }
  g_norder[k] = n;
  UINT32 sig = 2166136261u; sig = fnv(sig, &c->did, 2); sig = fnv(sig, &c->ssv, 2); sig = fnv(sig, &c->ssi, 2);
  for (UINTN i = 0; i < n; i++) sig = fnv(sig, cands[g_order[k][i]].name, StrLen(cands[g_order[k][i]].name) * 2);
  hist_load(c, sig);
}
const CHAR16 *res_tag_for(CARD *c, UINTN i)
{
  static CHAR16 b[2][64]; static UINTN k; CHAR16 *o = b[k ^= 1];
  UINT8 r = i < NRES ? c->hist.res[i] : 0;
  switch (r) {
    case 1: SPrint(o, 128, L"hung (VRAM before=%x)", c->hist.membefore[i]); break;
    case 2: SPrint(o, 128, L"unusable"); break;
    case 3: SPrint(o, 128, L"driver error"); break;
    case 4: SPrint(o, 128, L"OK"); break;
    case 6: SPrint(o, 128, L"OK, %d stuck poll(s) skipped", (int)c->hist.loops[i]); break;
    default: o[0] = 0;
  }
  return o;
}

/* ---------- card detection ---------- */
static const UINT16 g_polaris[] = { 0x67C0,0x67C4,0x67C7,0x67CA,0x67CC,0x67CF,0x67D0,0x67DF,0x67E0,0x67E1,0x67E3,0x67E7,0x67E8,0x67E9,0x67EB,0x67EF,0x67FF,
                                    0x6980,0x6981,0x6985,0x6986,0x6987,0x6995,0x6997,0x699F,0x6FDF };
static BOOLEAN did_known(UINT16 d)
{
  for (UINTN i = 0; i < sizeof g_polaris / sizeof g_polaris[0]; i++) if (g_polaris[i] == d) return TRUE;
  for (UINTN i = 0; i < ncand; i++) if (cands[i].ok && cands[i].did == d) return TRUE;
  return FALSE;
}
static void add_demo_cards(void)
{
  static const struct { UINT8 bus; UINT16 ssv, ssi; CARD_STATE st; const CHAR16 *note; UINT32 mb; } d[] = {
    {0x03, 0x1682, 0xAAF6, CS_NATIVE, L"already initialised - left untouched", 8192},
    {0x06, 0x1682, 0xAAF6, CS_LOADED, L"initialised from ROM", 8192},
    {0x09, 0x1458, 0x22FC, CS_LOADED, L"initialised from ROM, 2 stuck polls skipped", 8192},
    {0x0C, 0x1043, 0x04C5, CS_FAILED, L"all 7 ROMs failed", 0},
    {0x0F, 0x1462, 0x3417, CS_SKIPPED, L"disabled in settings", 0},
  };
  for (UINTN i = 0; i < 5; i++) {
    CARD *c = &g_cards[g_ncards++]; ZeroMem(c, sizeof *c);
    c->demo = TRUE; c->bus = d[i].bus; c->dev = 0; c->fn = 0; c->did = 0x67DF; c->ssv = d[i].ssv; c->ssi = d[i].ssi; c->state = CS_PENDING;
    c->vram_mb = d[i].mb; set_note(c, L"%s", d[i].note);
    if (d[i].st == CS_LOADED || d[i].st == CS_FAILED) StrCpy(c->rom, ncand ? cands[i % ncand].name : L"00_demo_original.rom");
  }
}
static void detect_cards(void)
{
  g_ncards = 0; ZeroMem(g_cards, sizeof g_cards);
  if (CFG(S_DEMO)) { add_demo_cards(); return; }
  UINTN n = 0; EFI_HANDLE *hs = NULL;
  if (EFI_ERROR(FW(BS->LocateHandleBuffer, ByProtocol, &gEfiPciIoProtocolGuid, NULL, &n, &hs))) return;
  for (UINTN i = 0; i < n && g_ncards < MAX_CARDS; i++) {
    EFI_PCI_IO_PROTOCOL *p; UINT16 v, d; UINT8 cls;
    if (EFI_ERROR(FW(BS->HandleProtocol, hs[i], &gEfiPciIoProtocolGuid, (VOID **)&p))) continue;
    FW(p->Pci.Read, p, EfiPciIoWidthUint16, 0, 1, &v); FW(p->Pci.Read, p, EfiPciIoWidthUint16, 2, 1, &d);
    FW(p->Pci.Read, p, EfiPciIoWidthUint8, 0x0B, 1, &cls);
    if (v != AMD_VID || cls != 0x03 || !did_known(d)) continue;
    CARD *c = &g_cards[g_ncards++]; c->h = hs[i]; c->pio = p; c->did = d;
    FW(p->GetLocation, p, &c->seg, &c->bus, &c->dev, &c->fn);
    FW(p->Pci.Read, p, EfiPciIoWidthUint16, 0x2C, 1, &c->ssv); FW(p->Pci.Read, p, EfiPciIoWidthUint16, 0x2E, 1, &c->ssi);
    UINT32 bar5 = 0;
    FW(p->Pci.Read, p, EfiPciIoWidthUint32, 0x24, 1, &bar5);       /* config space only: the card is enabled later, right before its init (as in v0.1) */
    c->mmio = (bar5 & 1) ? 0 : (bar5 & ~0xFu);
    c->state = CS_PENDING;
  }
  FreePool(hs);
  for (UINTN i = 1; i < g_ncards; i++) {                  /* stable order: by bus/dev/fn */
    CARD k = g_cards[i]; INTN j = (INTN)i - 1; UINTN kk = (k.bus << 16) | (k.dev << 8) | k.fn;
    while (j >= 0 && ((g_cards[j].bus << 16) | (g_cards[j].dev << 8) | g_cards[j].fn) > kk) { g_cards[j+1] = g_cards[j]; j--; }
    g_cards[j+1] = k;
  }
  for (UINTN i = 0; i < g_ncards; i++) { card_prepare(&g_cards[i]); g_cards[i].hist.magic = STATE_MAGIC; }
}

/* ---------- helpers for one card ---------- */
static void use_card(CARD *c) { g_hc = c; g_h = &c->hist; g_mmio = c->mmio; }
static void remember_working(CARD *c, UINTN idx)
{
  post_code(0x70); TRACE(L"SUCCESS %s", cands[idx].name);
  StrCpy(g_st.working, cands[idx].name); g_st.strikes = 0; g_st.pinstrikes = 0; state_save();
  lg(L"\n  SUCCESS - remembered working ROM: %s\n", cands[idx].name);
  StrCpy(c->rom, cands[idx].name);
}
static void do_reboot(void)
{
  lg(L"\n  Cold-rebooting; the next ROM is tried after the restart.\n");
  set_activity(L"Rebooting to continue with the next ROM...");
  FW(BS->Stall, 2500000); hard_reset();
}
static void demo_run(CARD *c)
{
  static const CARD_STATE st[] = { CS_NATIVE, CS_LOADED, CS_LOADED, CS_FAILED, CS_SKIPPED };
  UINTN k = (UINTN)(c - g_cards);
  FW(BS->Stall, 700000); c->state = st[k % 5]; c->tried = c->state == CS_FAILED ? 7 : (c->state == CS_LOADED ? 1 + k : 0); c->ms = 800 + 350 * k;
  if (c->state == CS_NATIVE) c->rom[0] = 0;
  if (c->state == CS_FAILED) { c->rom[0] = 0; }
  ui_progress();
}

/* Handle one card.  manual >= 0: run exactly that ROM (candidate index) once.  manual < 0: AUTO. */
static void process_card(CARD *c, EFI_HANDLE image, INTN manual)
{
  UINTN ci = (UINTN)(c - g_cards), *order = g_order[ci], n = g_norder[ci];
  PIN *pin = pin_find(c->bus, c->dev, c->fn, FALSE);
  if (pin && pin->skip && manual < 0) { c->state = CS_SKIPPED; c->rom[0] = 0; c->tried = 0; set_note(c, L"disabled in settings"); vfct_clear(c); vfct_publish(); return; }
  if (c->demo) { c->state = CS_RUNNING; set_activity(L"DEMO: card %d of %d (no hardware is touched)", ci + 1, g_ncards); demo_run(c); return; }
  c->note[0] = 0; c->rom[0] = 0; c->tried = 0; c->ms = 0;
  c->state = CS_RUNNING;
  set_activity(L"Card %d/%d  %02x:%02x.%x : checking state", ci + 1, g_ncards, c->bus, c->dev, c->fn);
  use_card(c);
  TRACE(L"card %d/%d %02x:%02x.%x subsys %04x:%04x: %d candidate ROM(s), next=%d working=%s", ci + 1, g_ncards, (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn, c->ssv, c->ssi, n, g_st.next, g_st.working[0] ? g_st.working : L"-");
  post_code(0x06);
  unsigned long t0 = atom_now_ms();
  lg(L"\n== Card %d/%d  %02x:%02x.%x  %04x:%04x  subsys %04x:%04x ==\n", ci + 1, g_ncards, c->bus, c->dev, c->fn, AMD_VID, c->did, c->ssv, c->ssi);
  if (!c->mmio) { c->state = CS_FAILED; set_note(c, L"no MMIO BAR (enable Above 4G Decoding)"); lg(L"   %s\n", c->note); return; }
  /* NOTE: no register access here. Everything that touches the card happens inside run_cand(), in the v0.1 order. */
  if (manual >= 0) {
    g_force_once = TRUE; c->tried = 1;
    set_activity(L"Card %d/%d : running %s", ci + 1, g_ncards, cands[manual].name);
    UINTN r = run_cand(c, (UINTN)manual, CFG(S_VFCT_ONLY), image); g_force_once = FALSE; c->ms = (UINT32)(atom_now_ms() - t0);
    if (r == RES_OK || r == RES_VFCT) { if (r == RES_OK) remember_working(c, (UINTN)manual); c->state = CS_LOADED; StrCpy(c->rom, cands[manual].name); c->vram_mb = g_last_mem; set_note(c, r == RES_VFCT ? L"ROM published to the OS only (VFCT)" : L"initialised from ROM (manual)"); }
    else { c->state = CS_FAILED; set_note(c, r == RES_DEAD ? L"no response after reset - power-cycle" : L"ROM did not bring the card up"); vfct_clear(c); }
    vfct_publish(); if (g_want_reboot) { g_want_reboot = FALSE; } return;
  }
  if (n == 0) { c->state = CS_FAILED; set_note(c, L"no usable ROM file for device %04x", c->did); lg(L"   %s\n", c->note); vfct_clear(c); vfct_publish(); return; }
  if (g_st.next >= n && !g_st.working[0]) {
    if (!CFG(S_RETRY_FAILED)) { c->state = CS_FAILED; c->tried = (UINT32)n; set_note(c, L"all %d ROMs failed earlier - press R to retry", n); lg(L"   %s\n", c->note); vfct_clear(c); vfct_publish(); return; }
    UINT32 sg = g_st.sig; ZeroMem(&c->hist, sizeof(CHIST)); c->hist.magic = STATE_MAGIC; c->hist.sig = sg; state_save();
  }

  for (;;) {
    UINTN idx, w = g_st.working[0] ? find_by_name(g_st.working) : (UINTN)-1; BOOLEAN was_working = FALSE, was_pin = FALSE;
    UINTN pi = (pin && pin->rom[0] && g_st.pinstrikes == 0) ? find_by_name(pin->rom) : (UINTN)-1;
    if (pi != (UINTN)-1 && cands[pi].did == c->did) { idx = pi; was_pin = TRUE; g_st.pinstrikes++; state_save(); lg(L"   using the ROM pinned to this card\n"); }
    else if (w != (UINTN)-1 && g_st.strikes < 2) { idx = w; was_working = TRUE; g_st.strikes++; state_save(); }
    else {
      if (g_st.working[0]) { g_st.working[0] = 0; g_st.strikes = 0; g_st.next = 0; state_save(); }
      if (g_st.next >= n) { c->state = CS_FAILED; c->tried = (UINT32)n; set_note(c, L"all %d ROMs failed", n); lg(L"\n  %s\n", c->note); vfct_clear(c); vfct_publish(); return; }
      idx = order[g_st.next]; g_st.next++; state_save();      /* assume failure; cleared on success */
    }
    c->tried++; StrCpy(c->rom, cands[idx].name);
    set_activity(L"Card %d/%d (%02x:%02x.%x): ROM %d - %s", ci + 1, g_ncards, c->bus, c->dev, c->fn, c->tried, cands[idx].name);
    g_want_reboot = FALSE;
    g_last_skipped = FALSE;
    UINTN r = run_cand(c, idx, CFG(S_VFCT_ONLY), image);
    c->ms = (UINT32)(atom_now_ms() - t0);
    if (r == RES_OK && g_last_skipped) {                   /* green: the card was already up, nothing was changed */
      c->state = CS_NATIVE; c->vram_mb = g_last_mem; c->rom[0] = 0; set_note(c, L"already initialised - left untouched");
      if (g_st.working[0]) { StrCpy(c->rom, g_st.working); }                         /* it needed a ROM before: keep publishing it */
      else { vfct_clear(c); }                                                         /* own working chip: do not hand the OS a foreign ROM */
      vfct_publish(); return;
    }
    if (r == RES_OK) {
      lg(L"   [CARD] run_cand returned RES_OK; saving working ROM...\n");
      post_code(0x48);
      remember_working(c, idx);
      post_code(0x49);
      c->state = CS_LOADED; c->vram_mb = g_last_mem;
      if (cands[idx].size && idx < NRES && g_st.loops[idx]) set_note(c, L"initialised, %d stuck poll(s) skipped", (int)g_st.loops[idx]); else set_note(c, L"initialised from ROM");
      lg(L"   [CARD] final card state recorded; returning to run_all() without VFCT ACPI publish.\n");
      /* VFCT is intentionally published only once after every card has been
       * initialized. Publishing it here can hang firmware after ASIC_Init. */
      post_code(0x4A);
      return;
    }
    if (r == RES_VFCT) { c->state = CS_LOADED; set_note(c, L"ROM published to the OS only (VFCT)"); vfct_publish(); return; }
    if (r == RES_SYSTEMIC) { c->state = CS_FAILED; set_note(c, L"PCI access problem"); c->rom[0] = 0; return; }
    if (r == RES_DEAD) { c->state = CS_FAILED; set_note(c, L"no response after reset - power-cycle, then the next ROM is tried"); c->rom[0] = 0; vfct_clear(c); vfct_publish(); lg(L"\n  Power-cycle the PC; AUTO resumes with the next ROM.\n"); return; }
    if (r == RES_FAIL) {                                 /* GOP engine: card was touched -> cold reset, next boot tries the next ROM */
      if (was_working) { g_st.working[0] = 0; g_st.strikes = 0; g_st.next = 0; state_save(); }
      lg(L"\n  ROM failed -> resetting to try the next one...\n"); set_activity(L"ROM failed - rebooting to try the next one...");
      FW(BS->Stall, 2500000); FW(RT->ResetSystem, EfiResetCold, EFI_SUCCESS, 0, NULL); hard_reset();
    }
    /* RES_HUNG / RES_SKIP: card was reset according to reset_mode, go on with the next ROM */
    if (was_working) { g_st.working[0] = 0; g_st.strikes = 0; g_st.next = 0; state_save(); }
    if (r == RES_HUNG && g_want_reboot) { g_want_reboot = FALSE; do_reboot(); }
    if (was_pin && !CFG(S_PIN_FALLBACK)) { c->state = CS_FAILED; set_note(c, L"pinned ROM failed (pin_fallback=off)"); vfct_clear(c); vfct_publish(); return; }
  }
}

/* ---------- pages ---------- */
enum { ACT_START, ACT_SKIP, ACT_BOOT };
/* Final OS handoff is deliberately separated from GPU initialization.
 * During ASIC_Init we must not touch PciIo->RomImage or ACPI VFCT: doing so
 * can interfere with firmware while the next GPU is still being initialized.
 * Once every card has returned from run_cand(), the ROM buffers are still alive
 * and it is safe to expose them to the firmware/OS in one operation. */
static void publish_os_roms(void)
{
  UINTN n = 0;
  lg(L"[HANDOFF] attaching loaded VBIOS images to PciIo protocols...\\n");
  post_code(0x4D);
  for (UINTN i = 0; i < MAX_CARDS; i++) {
    if (!g_vent[i].used) continue;
    CARD *c = &g_cards[i];
    if (!c->pio || !g_vent[i].rom || !g_vent[i].sz) continue;
    c->pio->RomImage = (UINT8 *)g_vent[i].rom;
    c->pio->RomSize = g_vent[i].sz;
    lg(L"   [HANDOFF] %02x:%02x.%x ROM attached: %u bytes\\n",
       (UINT32)c->bus, (UINT32)c->dev, (UINT32)c->fn, (UINT32)g_vent[i].sz);
    n++;
  }
  lg(L"[HANDOFF] %u PCI ROM image(s) attached; VFCT publication DISABLED for isolation.\\n", (UINT32)n);
  /* VFCT is intentionally disabled in this build. The previous build stopped
   * responding exactly when vfct_publish() was called, even with one GPU.
   * First verify PciIo->RomImage alone survives the return to firmware/OS. */
  post_code(0x4E);
  post_code(0x4F);
}

static void run_all(EFI_HANDLE image, BOOLEAN only_failed)
{
  if (!g_ui_gui) FW(ST->ConOut->ClearScreen, ST->ConOut);
  for (UINTN i = 0; i < g_ncards; i++) {
    CARD *c = &g_cards[i];
    if (only_failed && c->state != CS_FAILED && c->state != CS_PENDING) continue;
    process_card(c, image, -1);
    ui_progress();
  }
  publish_os_roms();
  lg(L"[MULTI] all cards processed; OS VBIOS handoff completed.\\n");
  post_code(0x50);
  g_activity[0] = 0;
}
static BOOLEAN any_failed(void) { for (UINTN i = 0; i < g_ncards; i++) if (g_cards[i].state == CS_FAILED) return TRUE; return FALSE; }
static void text_pause(void) { if (!g_ui_gui) { lg(L"\nPress any key...\n"); KEY k; ui_key(&k, 0); } }

static void card_menu(CARD *c, EFI_HANDLE image)
{
  UINTN ci = (UINTN)(c - g_cards); ROW *rows = AllocateZeroPool(sizeof(ROW) * 260); CHAR16 title[100], sub[120];
  for (;;) {
    UINTN n = 0; PIN *pin = pin_find(c->bus, c->dev, c->fn, FALSE); UINTN idxmap[260];
    StrCpy(rows[n].left, L"AUTO - try the ROMs one by one (smart order)"); rows[n].right[0] = 0; rows[n].color = 0xFFFFFF; idxmap[n++] = (UINTN)-1;
    for (UINTN i = 0; i < ncand && n < 258; i++) {
      if (!cands[i].ok) continue;
      SPrint(rows[n].left, sizeof rows[n].left, L"%s", cands[i].name);
      CHAR16 tg[60]; tg[0] = 0;
      if (pin && !StrCmp(pin->rom, cands[i].name)) StrCat(tg, L"[PINNED] ");
      if (!StrCmp(c->hist.working, cands[i].name)) StrCat(tg, L"[remembered] ");
      if (cands[i].did != c->did) StrCat(tg, L"[other device]");
      SPrint(rows[n].right, sizeof rows[n].right, L"%s%s", tg, res_tag_for(c, i));
      rows[n].color = cands[i].did == c->did ? 0xE6EDF3 : 0x6B7785; idxmap[n++] = i;
    }
    StrCpy(rows[n].left, (pin && pin->skip) ? L"Enable this card again" : L"Disable this card (leave it alone)"); rows[n].right[0] = 0; rows[n].color = 0xFFB86B; idxmap[n++] = (UINTN)-2;
    SPrint(title, sizeof title, L"Card %d   %02x:%02x.%x   %04x:%04x   subsystem %04x:%04x%s%s", ci + 1, c->bus, c->dev, c->fn, AMD_VID, c->did, c->ssv, c->ssi, brand_of(c->ssv) ? L"  " : L"", brand_of(c->ssv) ? brand_of(c->ssv) : L"");
    SPrint(sub, sizeof sub, L"Pinned ROM: %s", (pin && pin->rom[0]) ? pin->rom : L"none (AUTO)");
    static UINTN sel, top; if (sel >= n) sel = 0;
    LIST l = { title, sub, L"UP/DOWN select   ENTER run now   P pin ROM to this card   U unpin   ESC back", rows, n, sel, top };
    ui_list_draw(&l); sel = l.sel; top = l.top;
    KEY k; if (!ui_key(&k, 0)) continue;
    if (k.sc == 0x17) break;
    if (k.sc == 0x01 && sel > 0) sel--;
    if (k.sc == 0x02 && sel + 1 < n) sel++;
    if (k.sc == 0x09) sel = sel > 8 ? sel - 8 : 0;
    if (k.sc == 0x0A) sel = sel + 8 < n ? sel + 8 : n - 1;
    if (k.ch == L'p' || k.ch == L'P') { if (idxmap[sel] < ncand) { pin = pin_find(c->bus, c->dev, c->fn, TRUE); if (pin) { StrCpy(pin->rom, cands[idxmap[sel]].name); pin->origin = OR_NVRAM; pin_commit(); } } }
    if (k.ch == L'u' || k.ch == L'U') { if (pin) { pin->rom[0] = 0; pin->origin = OR_NVRAM; pin_commit(); } }
    if (k.ch == L'\r') {
      if (idxmap[sel] == (UINTN)-2) { pin = pin_find(c->bus, c->dev, c->fn, TRUE); if (pin) { pin->skip = !pin->skip; pin->origin = OR_NVRAM; pin_commit(); c->state = pin->skip ? CS_SKIPPED : CS_PENDING; set_note(c, pin->skip ? L"disabled in settings" : L""); } }
      else { g_force_once = TRUE; if (idxmap[sel] == (UINTN)-1) { process_card(c, image, -1); } else process_card(c, image, (INTN)idxmap[sel]); g_force_once = FALSE; text_pause(); break; }
    }
  }
  FreePool(rows);
}

/* the status page: interactive; secs >= 0 runs a countdown that ends the page with ACT_START / ACT_BOOT */
static UINTN g_sel;
static UINTN status_page(EFI_HANDLE image, BOOLEAN done, INTN secs)
{
  for (;;) {
    CHAR16 foot[200];
    if (secs >= 0) SPrint(foot, sizeof foot, done ? L"Continuing boot in %d s - any key to stay here" : L"Starting AUTO in %d s - any key for options", (int)secs);
    else SPrint(foot, sizeof foot, done ? L"ENTER continue boot  -  A retry failed cards" : L"ENTER or A start  -  ESC skip loader");
    ui_draw_status(g_sel, foot);
    KEY k; BOOLEAN got = ui_key(&k, secs >= 0 ? 1000 : 0);
    if (!got) { if (--secs < 0) return done ? ACT_BOOT : ACT_START; continue; }
    if (secs >= 0) { secs = -1; continue; }                 /* first key only stops the countdown */
    UINTN cols = ui_cols();
    if (k.sc == 0x17) return done ? ACT_BOOT : ACT_SKIP;
    if (k.ch == L'\r') return done ? ACT_BOOT : ACT_START;
    if (k.ch == L'a' || k.ch == L'A') return ACT_START;
    if (k.sc == 0x04 && g_sel > 0) g_sel--;
    if (k.sc == 0x03 && g_sel + 1 < g_ncards) g_sel++;
    if (k.sc == 0x01) g_sel = g_sel >= cols ? g_sel - cols : g_sel;
    if (k.sc == 0x02) g_sel = g_sel + cols < g_ncards ? g_sel + cols : g_sel;
    if (g_ncards && (k.ch == L'c' || k.ch == L'C' || k.ch == L' ')) { card_menu(&g_cards[g_sel], image); }
    if (k.ch == L's' || k.ch == L'S') { ui_run_settings(); ui_init(); }
    if (k.ch == L'l' || k.ch == L'L') ui_run_log();
    if (g_ncards && (k.ch == L'd' || k.ch == L'D')) { ui_message(L"Register dump", L"Dumping GPU registers... (a freeze here is a result too: see loader_trace.txt)", NULL, FALSE); dump_registers(&g_cards[g_sel]); ui_message(L"Register dump", L"Done. The file is in the loader folder on the EFI partition.", NULL, TRUE); }
    if (g_ncards && (k.ch == L'x' || k.ch == L'X')) { PIN *p = pin_find(g_cards[g_sel].bus, g_cards[g_sel].dev, g_cards[g_sel].fn, TRUE); if (p) { p->skip = !p->skip; p->origin = OR_NVRAM; pin_commit(); g_cards[g_sel].state = p->skip ? CS_SKIPPED : CS_PENDING; set_note(&g_cards[g_sel], p->skip ? L"disabled in settings" : L""); } }
    if (k.ch == L'r' || k.ch == L'R') { for (UINTN i = 0; i < g_ncards; i++) hist_forget(&g_cards[i]); for (UINTN i = 0; i < g_ncards; i++) card_prepare(&g_cards[i]); ui_message(L"History cleared", L"Remembered ROMs and per-ROM results of all cards were forgotten.", L"Press A to run AUTO again.", TRUE); }
  }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
  InitializeLib(image, systab);
  g_image = image; post_code(0x01);
  init_volume(image);
  cfg_defaults(); cfg_load_file(g_vol_root, g_owndir); cfg_load_nvram();
  TRACE(L"v%s start: config %s, ui_mode=%d reset_mode=%d smart_order=%d", LOADER_VERSION, g_cfg_file_found ? g_cfg_path : L"(none)", CFG(S_UI_MODE), CFG(S_RESET_MODE), CFG(S_SMART_ORDER)); post_code(0x02);
  scan_folder(); scan_meta(); TRACE(L"%d ROM file(s) found", ncand); post_code(0x03);
  ui_init(); TRACE(L"ui ready (%s)", g_ui_gui ? L"gui" : L"text"); post_code(0x04);
  detect_cards(); TRACE(L"%d card(s) detected", g_ncards); post_code(0x05);
  lg(L"RX 580 vBIOS loader %s: %d card(s), %d ROM file(s) in %s, config %s\n", LOADER_VERSION, g_ncards, ncand, g_romdir ? g_romdir : L"(none)", g_cfg_file_found ? g_cfg_path : L"(none, built-in defaults)");

  if (!g_ncards || !ncand) {
    set_activity(g_ncards ? L"No ROM files found: put *.rom files into %s\\vbioses" : L"No supported AMD GPU found", g_owndir);
    ui_draw_status(0, L"Continuing boot in 8 s - any key to stay here");
    KEY k; if (ui_key(&k, 8000)) { status_page(image, TRUE, -1); }
    ui_end(); return EFI_SUCCESS;
  }
  INTN secs = (CFG(S_BOOT_MODE) == BM_AUTO) ? (INTN)CFG(S_MENU_TIMEOUT) : -1;
  UINTN act = ACT_START;
  if (secs != 0) act = status_page(image, FALSE, secs);
  if (act != ACT_SKIP) {
    for (;;) {
      run_all(image, g_cards[0].state != CS_PENDING);
      BOOLEAN fail = any_failed();
      if (!fail) {
        lg(L"[BOOT] all GPU initialization attempts completed successfully; leaving loader now.\n");
        post_code(0x4C);
        break;
      }
      INTN wait = CFG(S_FINISH_SECS);
      if (wait == 0) break;
      if (status_page(image, TRUE, wait) != ACT_START) break;
    }
  }
  ui_end();
  return EFI_SUCCESS;
}
