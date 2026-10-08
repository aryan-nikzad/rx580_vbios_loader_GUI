/* Host-side test for vfct.h:   gcc -Wall -I. -o /tmp/test_vfct tools/test_vfct.c && /tmp/test_vfct
 * kern_lookup() mirrors the checks of Linux amdgpu_acpi_vfct_bios() (drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef uint64_t UINTN; typedef int64_t INTN;
#include "vfct.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* returns image length and pointer if the kernel would find an image for this PCI function, 0 otherwise */
static uint32_t kern_lookup(const uint8_t *t, uint32_t tbl_size, unsigned bus, unsigned dev, unsigned fn,
                            unsigned vid, unsigned did, const uint8_t **img)
{
  uint32_t offset;
  if (tbl_size < 76 || memcmp(t, "VFCT", 4)) return 0;
  uint32_t len; memcpy(&len, t + 4, 4); if (len != tbl_size) return 0;        /* acpi_get_table gives header length */
  if (vf_sum(t, tbl_size) != 0) return 0;                                      /* ACPICA checksum verification */
  memcpy(&offset, t + 52, 4);
  while (offset < tbl_size) {
    const uint8_t *vh = t + offset; uint32_t pb, pd, pf, il; uint16_t v, d;
    offset += 28;
    if (offset > tbl_size) return 0;
    memcpy(&pb, vh, 4); memcpy(&pd, vh + 4, 4); memcpy(&pf, vh + 8, 4);
    memcpy(&v, vh + 12, 2); memcpy(&d, vh + 14, 2); memcpy(&il, vh + 24, 4);
    offset += il;
    if (offset > tbl_size) return 0;
    if (il && pb == bus && pd == dev && pf == fn && v == vid && d == did) { *img = vh + 28; return il; }
  }
  return 0;
}

static uint8_t *mkrom(uint32_t len, uint8_t seed)
{
  uint8_t *r = malloc(len);
  for (uint32_t i = 0; i < len; i++) r[i] = (uint8_t)(seed + i * 7);
  r[0] = 0x55; r[1] = 0xAA; return r;
}

int main(void)
{
  /* ---- 3 cards, different sizes and ROMs, odd sizes on purpose (no alignment padding between records) ---- */
  VFCT_IMG im[3] = {
    { 0x03, 0, 0, 0x1002, 0x67DF, 0x1682, 0xAAF6, mkrom(0x1E001, 1), 0x1E001 },
    { 0x06, 0, 0, 0x1002, 0x67DF, 0x1458, 0x22FC, mkrom(0x1D801, 2), 0x1D801 },
    { 0x0a, 0, 0, 0x1002, 0x67DF, 0x1043, 0x04C5, mkrom(0x20000, 3), 0x20000 },
  };
  uint32_t total = vfct_total(im, 3);
  uint8_t *t = malloc(total); vfct_write(t, im, 3);
  CHECK(total == 76 + 3 * 28 + 0x1E001 + 0x1D801 + 0x20000);
  CHECK(vf_sum(t, total) == 0);
  for (int i = 0; i < 3; i++) {
    const uint8_t *got = NULL;
    uint32_t l = kern_lookup(t, total, im[i].bus, 0, 0, 0x1002, 0x67DF, &got);
    CHECK(l == im[i].len); CHECK(got && !memcmp(got, im[i].rom, l));
  }
  const uint8_t *g;
  CHECK(kern_lookup(t, total, 0x04, 0, 0, 0x1002, 0x67DF, &g) == 0);           /* unknown card -> no image */
  CHECK(kern_lookup(t, total, 0x03, 0, 1, 0x1002, 0x67DF, &g) == 0);           /* wrong function */

  /* ---- parse round trip (merging a firmware VFCT) ---- */
  VFCT_IMG back[VFCT_MAX_IMAGES]; UINTN nb = vfct_parse(t, total, back, VFCT_MAX_IMAGES);
  CHECK(nb == 3); CHECK(back[1].bus == 6 && back[1].ssv == 0x1458 && back[1].len == 0x1D801 && !memcmp(back[1].rom, im[1].rom, back[1].len));

  /* ---- truncated / hostile tables must not read out of bounds and must not crash ---- */
  CHECK(vfct_parse(t, total - 5, back, VFCT_MAX_IMAGES) == 2);                  /* last record cut off -> stops, keeps first two */
  CHECK(vfct_parse(t, 40, back, VFCT_MAX_IMAGES) == 0);
  uint8_t *bad = malloc(total); memcpy(bad, t, total); vf_put32(bad + 52, 0xFFFFFFF0u);
  CHECK(vfct_parse(bad, total, back, VFCT_MAX_IMAGES) == 0);
  memcpy(bad, t, total); vf_put32(bad + 76 + 24, 0xFFFFFFFFu);                  /* absurd ImageLength in first record */
  CHECK(vfct_parse(bad, total, back, VFCT_MAX_IMAGES) == 0);
  CHECK(vfct_parse(t, total, back, 2) == 2);                                    /* max respected */

  /* ---- zero cards -> header-only table is still valid ---- */
  uint8_t e[VFCT_HDR_SIZE]; vfct_write(e, im, 0); CHECK(vf_sum(e, sizeof e) == 0); CHECK(vfct_parse(e, sizeof e, back, 8) == 0);

  /* ---- XSDT patching ---- */
  uint8_t x[36 + 8 * 4]; memset(x, 0, sizeof x); memcpy(x, "XSDT", 4); x[8] = 1;
  for (int i = 0; i < 4; i++) vf_put64(x + 36 + 8 * i, 0x1000u * (i + 1));
  vf_acpi_seal(x, sizeof x);
  uint8_t xo[sizeof x + 8];
  UINT32 nl = xsdt_patch(x, sizeof x, -1, 0xABCD0000ull, xo);                   /* append */
  CHECK(nl == sizeof x + 8); CHECK(vf_sum(xo, nl) == 0); CHECK(vf_get32(xo + 4) == nl); CHECK(vf_get64(xo + 36 + 32) == 0xABCD0000ull);
  CHECK(vf_get64(xo + 36) == 0x1000 && vf_get64(xo + 36 + 24) == 0x4000);        /* old entries intact */
  nl = xsdt_patch(x, sizeof x, 2, 0xABCD0000ull, xo);                           /* replace */
  CHECK(nl == sizeof x); CHECK(vf_sum(xo, nl) == 0); CHECK(vf_get64(xo + 36 + 16) == 0xABCD0000ull); CHECK(vf_get64(xo + 36 + 8) == 0x2000);
  CHECK(xsdt_patch(x, sizeof x, 4, 1, xo) == 0);                                /* index out of range */
  CHECK(xsdt_patch(x, sizeof x - 3, -1, 1, xo) == 0);                           /* not a multiple of 8 */
  x[0] = 'R'; CHECK(xsdt_patch(x, sizeof x, -1, 1, xo) == 0);                   /* wrong signature */

  printf(fails ? "%d FAILURE(S)\n" : "all VFCT tests passed\n", fails);
  return fails != 0;
}
