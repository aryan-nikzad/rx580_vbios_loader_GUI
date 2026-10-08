/* gfx.c - tiny software renderer on top of the UEFI Graphics Output Protocol (back buffer + one Blt per frame). */
#include "gfx.h"
#include "font_terminus.h"

UINTN gfx_w, gfx_h, gfx_scale = 1, gfx_cw = 8, gfx_ch = 16;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop; static UINT32 *g_bb;

BOOLEAN gfx_init(UINTN scale_cfg)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  if (EFI_ERROR(FW(BS->HandleProtocol, ST->ConsoleOutHandle, &GraphicsOutputProtocol, (VOID **)&gop)) || !gop)
    if (EFI_ERROR(FW(BS->LocateProtocol, &GraphicsOutputProtocol, NULL, (VOID **)&gop)) || !gop) return FALSE;
  if (!gop->Mode || !gop->Mode->Info) return FALSE;
  UINTN w = gop->Mode->Info->HorizontalResolution, h = gop->Mode->Info->VerticalResolution;
  if (w < 640 || h < 480 || w > 8192 || h > 8192) return FALSE;
  if (g_bb) FreePool(g_bb);
  g_bb = AllocatePool(w * h * 4); if (!g_bb) return FALSE;
  g_gop = gop; gfx_w = w; gfx_h = h;
  gfx_scale = scale_cfg ? scale_cfg : (h >= 2400) ? 3 : (h >= 1400) ? 2 : 1;
  if (h < 600) gfx_scale = 1;
  gfx_cw = 8 * gfx_scale; gfx_ch = 16 * gfx_scale;
  return TRUE;
}
void gfx_close(void) { if (g_bb) { FreePool(g_bb); g_bb = NULL; } g_gop = NULL; }

void gfx_present(void)
{
  if (g_gop && g_bb) FW(g_gop->Blt, g_gop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)g_bb, EfiBltBufferToVideo, 0, 0, 0, 0, gfx_w, gfx_h, 0);
}

void gfx_clear(UINT32 c) { for (UINTN i = 0; i < gfx_w * gfx_h; i++) g_bb[i] = c; }

UINT32 gfx_mix(UINT32 a, UINT32 b, UINTN t)
{
  UINT32 o = 0;
  for (int s = 0; s < 24; s += 8) { INTN x = (a >> s) & 255, y = (b >> s) & 255; o |= (UINT32)(x + (y - x) * (INTN)t / 256) << s; }
  return o;
}
static void px(INTN x, INTN y, UINT32 c, UINTN a)       /* a: 0..256 coverage */
{
  if (x < 0 || y < 0 || (UINTN)x >= gfx_w || (UINTN)y >= gfx_h || !a) return;
  UINT32 *p = &g_bb[(UINTN)y * gfx_w + (UINTN)x]; *p = a >= 256 ? c : gfx_mix(*p, c, a);
}

void gfx_rect(INTN x, INTN y, INTN w, INTN h, UINT32 c)
{
  INTN x1 = x + w, y1 = y + h; if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > (INTN)gfx_w) x1 = (INTN)gfx_w; if (y1 > (INTN)gfx_h) y1 = (INTN)gfx_h;
  for (INTN j = y; j < y1; j++) { UINT32 *p = &g_bb[(UINTN)j * gfx_w]; for (INTN i = x; i < x1; i++) p[i] = c; }
}

/* coverage (0..256) of a pixel at half-pixel offsets (dx2,dy2) from a circle centre, radius r */
static UINTN cov(INTN dx2, INTN dy2, INTN r)
{
  if (r < 1) return 256;
  INTN d4 = dx2 * dx2 + dy2 * dy2;                         /* 4 * dist^2 */
  INTN fp = d4 * 32 / r + r * 128;                         /* dist*256 ~ d2*128/r + r*128 */
  INTN c = r * 256 + 128 - fp; if (c < 0) c = 0; if (c > 256) c = 256; return (UINTN)c;
}
void gfx_circle(INTN cx, INTN cy, INTN r, UINT32 c)
{
  for (INTN y = cy - r - 1; y <= cy + r + 1; y++) for (INTN x = cx - r - 1; x <= cx + r + 1; x++)
    px(x, y, c, cov(2 * (x - cx), 2 * (y - cy), r));
}
void gfx_ring(INTN cx, INTN cy, INTN r, INTN t, UINT32 c)
{
  for (INTN y = cy - r - 1; y <= cy + r + 1; y++) for (INTN x = cx - r - 1; x <= cx + r + 1; x++) {
    INTN a = (INTN)cov(2 * (x - cx), 2 * (y - cy), r), b = (INTN)cov(2 * (x - cx), 2 * (y - cy), r - t);
    INTN k = a - b; if (k > 0) px(x, y, c, (UINTN)k);
  }
}
void gfx_rrect(INTN x, INTN y, INTN w, INTN h, INTN r, UINT32 c)
{
  if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
  if (r < 1) { gfx_rect(x, y, w, h, c); return; }
  gfx_rect(x + r, y, w - 2 * r, h, c); gfx_rect(x, y + r, r, h - 2 * r, c); gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
  for (INTN j = 0; j < r; j++) for (INTN i = 0; i < r; i++) {
    INTN dx2 = 2 * (r - i) - 1, dy2 = 2 * (r - j) - 1; UINTN a = cov(dx2, dy2, r);
    px(x + i, y + j, c, a); px(x + w - 1 - i, y + j, c, a); px(x + i, y + h - 1 - j, c, a); px(x + w - 1 - i, y + h - 1 - j, c, a);
  }
}
void gfx_line(INTN x0, INTN y0, INTN x1, INTN y1, INTN t, UINT32 c)
{
  INTN dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx - dy;
  for (;;) {
    gfx_rect(x0 - t / 2, y0 - t / 2, t, t, c);
    if (x0 == x1 && y0 == y1) break;
    INTN e2 = 2 * e; if (e2 > -dy) { e -= dy; x0 += sx; } if (e2 < dx) { e += dx; y0 += sy; }
  }
}

void gfx_text(INTN x, INTN y, const CHAR16 *s, UINT32 c, BOOLEAN bold)
{
  INTN sc = (INTN)gfx_scale;
  for (; *s; s++, x += 8 * sc) {
    UINT16 ch = *s; if (ch < 0x20 || ch > 0xFF) ch = '?';
    const unsigned char *g = bold ? font_bold[ch - 0x20] : font_reg[ch - 0x20];
    for (INTN r = 0; r < 16; r++) { unsigned char bits = g[r]; if (!bits) continue;
      for (INTN b = 0; b < 8; b++) if (bits & (0x80 >> b)) gfx_rect(x + b * sc, y + r * sc, sc, sc, c); }
  }
}
void gfx_text_n(INTN x, INTN y, const CHAR16 *s, UINTN maxcols, UINT32 c, BOOLEAN bold)
{
  UINTN n = StrLen(s);
  if (n <= maxcols) { gfx_text(x, y, s, c, bold); return; }
  if (maxcols < 3) return;
  CHAR16 t[160]; UINTN k = maxcols - 2 < 156 ? maxcols - 2 : 156; for (UINTN i = 0; i < k; i++) t[i] = s[i]; t[k] = L'.'; t[k+1] = L'.'; t[k+2] = 0;
  gfx_text(x, y, t, c, bold);
}
