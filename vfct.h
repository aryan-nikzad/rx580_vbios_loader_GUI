/* vfct.h - ACPI VFCT table construction / parsing, deliberately free of any EFI calls so it can be unit-tested
 * on a host (see tools/test_vfct.c).  Needs UINT8/UINT16/UINT32/UINT64/UINTN/BOOLEAN from the includer.
 *
 * Layout (what Linux amdgpu_acpi_vfct_bios() / radeon_acpi_vfct_bios() walk):
 *   0x00  ACPI header, 36 bytes, signature "VFCT"
 *   0x24  TableUUID[16]
 *   0x34  VBIOSImageOffset  (from the start of the table to the first image record)
 *   0x38  Lib1ImageOffset
 *   0x3C  Reserved[4]                                                          -> 76 bytes total
 *   then, back to back, no padding:  VFCT_IMAGE_HEADER (28 bytes) + ImageLength bytes of ROM
 *     +0 PCIBus u32, +4 PCIDevice u32, +8 PCIFunction u32, +12 VendorID u16, +14 DeviceID u16,
 *     +16 SSVID u16, +18 SSID u16, +20 Revision u32, +24 ImageLength u32
 * The kernel matches an image by bus/dev/fn AND vendor/device, and ignores records with ImageLength == 0.
 */
#ifndef VFCT_H
#define VFCT_H

#define VFCT_HDR_SIZE   76u
#define VFCT_IMGH_SIZE  28u
#define VFCT_MAX_IMAGES 40u
#define ACPI_HDR_SIZE   36u

typedef struct {
  UINT32 bus, dev, fn;
  UINT16 vid, did, ssv, ssi;
  const UINT8 *rom; UINT32 len;
} VFCT_IMG;

static inline void vf_put16(UINT8 *p, UINT32 v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); }
static inline void vf_put32(UINT8 *p, UINT32 v) { vf_put16(p, v & 0xFFFF); vf_put16(p + 2, v >> 16); }
static inline void vf_put64(UINT8 *p, UINT64 v) { vf_put32(p, (UINT32)v); vf_put32(p + 4, (UINT32)(v >> 32)); }
static inline UINT32 vf_get16(const UINT8 *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8); }
static inline UINT32 vf_get32(const UINT8 *p) { return vf_get16(p) | (vf_get16(p + 2) << 16); }
static inline UINT64 vf_get64(const UINT8 *p) { return (UINT64)vf_get32(p) | ((UINT64)vf_get32(p + 4) << 32); }
static inline void vf_copy(UINT8 *d, const UINT8 *s, UINTN n) { for (UINTN i = 0; i < n; i++) d[i] = s[i]; }
static inline void vf_zero(UINT8 *d, UINTN n) { for (UINTN i = 0; i < n; i++) d[i] = 0; }

/* fixes ACPI header length + checksum of a finished table (checksum byte lives at offset 9) */
static inline void vf_acpi_seal(UINT8 *t, UINT32 len)
{
  UINT8 sum = 0;
  vf_put32(t + 4, len); t[9] = 0;
  for (UINT32 i = 0; i < len; i++) sum = (UINT8)(sum + t[i]);
  t[9] = (UINT8)(0u - sum);
}
static inline UINT8 vf_sum(const UINT8 *p, UINTN n) { UINT8 s = 0; for (UINTN i = 0; i < n; i++) s = (UINT8)(s + p[i]); return s; }

static inline UINT32 vfct_total(const VFCT_IMG *im, UINTN n)
{
  UINT32 t = VFCT_HDR_SIZE;
  for (UINTN i = 0; i < n; i++) t += VFCT_IMGH_SIZE + im[i].len;
  return t;
}

/* writes the whole table (header, records, checksum) into t[0 .. vfct_total) */
static inline void vfct_write(UINT8 *t, const VFCT_IMG *im, UINTN n)
{
  UINT32 total = vfct_total(im, n); UINT8 *h;
  vf_zero(t, VFCT_HDR_SIZE);
  vf_copy(t, (const UINT8 *)"VFCT", 4);
  t[8] = 1;                                                        /* revision */
  vf_copy(t + 10, (const UINT8 *)"VBLDR ", 6);                      /* OEM id */
  vf_copy(t + 16, (const UINT8 *)"VBIOSLDR", 8);                   /* OEM table id */
  vf_put32(t + 24, 1);                                             /* OEM revision */
  vf_copy(t + 28, (const UINT8 *)"VLDR", 4); vf_put32(t + 32, 1);  /* creator id / revision */
  vf_put32(t + 52, VFCT_HDR_SIZE);                                 /* VBIOSImageOffset */
  h = t + VFCT_HDR_SIZE;
  for (UINTN i = 0; i < n; i++) {
    vf_put32(h + 0, im[i].bus); vf_put32(h + 4, im[i].dev); vf_put32(h + 8, im[i].fn);
    vf_put16(h + 12, im[i].vid); vf_put16(h + 14, im[i].did); vf_put16(h + 16, im[i].ssv); vf_put16(h + 18, im[i].ssi);
    vf_put32(h + 20, 0); vf_put32(h + 24, im[i].len);
    vf_copy(h + VFCT_IMGH_SIZE, im[i].rom, im[i].len);
    h += VFCT_IMGH_SIZE + im[i].len;
  }
  vf_acpi_seal(t, total);
}

/* Reads the image records of an existing VFCT (the firmware's own, if any) with the same bounds checks as the kernel.
 * Returns the number of records stored (images with ImageLength 0 are skipped); stops quietly at the first malformed record. */
static inline UINTN vfct_parse(const UINT8 *t, UINT32 len, VFCT_IMG *out, UINTN max)
{
  UINTN n = 0; UINT32 off;
  if (len < VFCT_HDR_SIZE || t[0] != 'V' || t[1] != 'F' || t[2] != 'C' || t[3] != 'T') return 0;
  off = vf_get32(t + 52);
  if (off < VFCT_HDR_SIZE) off = VFCT_HDR_SIZE;
  while (n < max) {
    UINT32 il;
    if ((UINT64)off + VFCT_IMGH_SIZE > len) break;
    il = vf_get32(t + off + 24);
    if ((UINT64)off + VFCT_IMGH_SIZE + il > len) break;
    if (il) {
      out[n].bus = vf_get32(t + off); out[n].dev = vf_get32(t + off + 4); out[n].fn = vf_get32(t + off + 8);
      out[n].vid = (UINT16)vf_get16(t + off + 12); out[n].did = (UINT16)vf_get16(t + off + 14);
      out[n].ssv = (UINT16)vf_get16(t + off + 16); out[n].ssi = (UINT16)vf_get16(t + off + 18);
      out[n].rom = t + off + VFCT_IMGH_SIZE; out[n].len = il; n++;
    }
    off += VFCT_IMGH_SIZE + il;
  }
  return n;
}

/* New XSDT = copy of `old` where entry `replace` (>= 0) is overwritten with `addr`, or, when replace < 0, `addr` is appended.
 * `out` needs oldlen + 8 bytes.  Returns the new length, or 0 if `old` is not a well-formed XSDT / index out of range. */
static inline UINT32 xsdt_patch(const UINT8 *old, UINT32 oldlen, INTN replace, UINT64 addr, UINT8 *out)
{
  UINT32 n, newlen;
  if (oldlen < ACPI_HDR_SIZE || old[0] != 'X' || old[1] != 'S' || old[2] != 'D' || old[3] != 'T') return 0;
  if (((oldlen - ACPI_HDR_SIZE) & 7u) != 0) return 0;
  n = (oldlen - ACPI_HDR_SIZE) / 8;
  if (replace >= (INTN)n) return 0;
  vf_copy(out, old, oldlen);
  if (replace >= 0) { vf_put64(out + ACPI_HDR_SIZE + 8u * (UINT32)replace, addr); newlen = oldlen; }
  else              { vf_put64(out + oldlen, addr); newlen = oldlen + 8; }
  vf_acpi_seal(out, newlen);
  return newlen;
}
#endif
