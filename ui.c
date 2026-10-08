/* ui.c - two front ends on the same logic: GUI (GOP, software rendered) and classic text console. */
#include "gfx.h"

/* ---------- palette ---------- */
#define C_BG     0x0D1117
#define C_PANEL  0x161B22
#define C_PANEL2 0x21262D
#define C_BORDER 0x30363D
#define C_TEXT   0xE6EDF3
#define C_MUTED  0x8B949E
#define C_GREEN  0x3FB950
#define C_YELLOW 0xE3B341
#define C_RED    0xF85149
#define C_BLUE   0x58A6FF
#define C_GREY   0x6E7681

const CHAR16 *state_label(CARD_STATE s)
{
  switch (s) { case CS_NATIVE: return L"READY"; case CS_LOADED: return L"LOADED"; case CS_FAILED: return L"FAILED";
               case CS_RUNNING: return L"WORKING"; case CS_SKIPPED: return L"DISABLED"; default: return L"WAITING"; }
}
static UINT32 state_color(CARD_STATE s)
{
  switch (s) { case CS_NATIVE: return C_GREEN; case CS_LOADED: return C_YELLOW; case CS_FAILED: return C_RED;
               case CS_RUNNING: return C_BLUE; default: return C_GREY; }
}
static UINTN g_cols_layout = 1;
UINTN ui_cols(void) { return g_ui_gui ? g_cols_layout : 1; }

/* ---------- init / end / input ---------- */
BOOLEAN ui_init(void)
{
  BOOLEAN want_gui = CFG(S_UI_MODE) != UI_TEXT;
  if (g_ui_gui) { gfx_close(); g_ui_gui = FALSE; }
  if (want_gui && gfx_init((UINTN)CFG(S_UI_SCALE))) {
    g_ui_gui = TRUE; FW(ST->ConOut->EnableCursor, ST->ConOut, FALSE);
  } else FW(ST->ConOut->EnableCursor, ST->ConOut, FALSE);
  FW(ST->ConIn->Reset, ST->ConIn, FALSE);
  FW(ST->ConOut->SetAttribute, ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
  FW(ST->ConOut->ClearScreen, ST->ConOut);
  return g_ui_gui;
}
void ui_end(void)
{
  if (g_ui_gui) { gfx_clear(C_BG); gfx_present(); gfx_close(); g_ui_gui = FALSE; }
  FW(ST->ConOut->SetAttribute, ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
  FW(ST->ConOut->ClearScreen, ST->ConOut); FW(ST->ConOut->EnableCursor, ST->ConOut, TRUE);
}
BOOLEAN ui_key(KEY *k, UINTN timeout_ms)
{
  EFI_EVENT ev[2]; UINTN idx = 0, n = 1; EFI_INPUT_KEY ik; BOOLEAN got = FALSE;
  ev[0] = ST->ConIn->WaitForKey;
  if (timeout_ms) { FW(BS->CreateEvent, EVT_TIMER, 0, NULL, NULL, &ev[1]); FW(BS->SetTimer, ev[1], TimerRelative, (UINT64)timeout_ms * 10000); n = 2; }
  for (;;) {
    FW(BS->WaitForEvent, n, ev, &idx);
    if (idx == 1) break;
    if (!EFI_ERROR(FW(ST->ConIn->ReadKeyStroke, ST->ConIn, &ik))) { k->sc = ik.ScanCode; k->ch = ik.UnicodeChar; got = TRUE; break; }
  }
  if (timeout_ms) { FW(BS->SetTimer, ev[1], TimerCancel, 0); FW(BS->CloseEvent, ev[1]); }
  return got;
}

/* ================================================================== GUI drawing ================ */
static void big_text(INTN x, INTN y, const CHAR16 *s, UINT32 c) { UINTN o = gfx_scale; gfx_scale = o * 2; gfx_text(x, y, s, c, TRUE); gfx_scale = o; }
static INTN text_w(const CHAR16 *s) { return (INTN)(StrLen(s) * gfx_cw); }
static void text_right(INTN xr, INTN y, const CHAR16 *s, UINT32 c, BOOLEAN b) { gfx_text(xr - text_w(s), y, s, c, b); }

static const INTN dir8[8][2] = { {256,0},{181,181},{0,256},{-181,181},{-256,0},{-181,-181},{0,-256},{181,-181} };
static void symbol(INTN cx, INTN cy, INTN r, CARD_STATE st, UINT32 col)
{
  gfx_circle(cx, cy, r, col);
  INTN t = (INTN)gfx_scale + (r > 14 ? 1 : 0), k = r / 2;
  switch (st) {
    case CS_NATIVE: gfx_line(cx - k, cy, cx - k / 3, cy + k - 1, t + 1, C_BG); gfx_line(cx - k / 3, cy + k - 1, cx + k, cy - k + 2, t + 1, C_BG); break;
    case CS_LOADED: gfx_line(cx + k / 3, cy - k, cx - k / 2, cy + 1, t + 1, C_BG); gfx_line(cx - k / 2, cy + 1, cx + k / 3, cy, t + 1, C_BG); gfx_line(cx + k / 3, cy, cx - k / 3, cy + k, t + 1, C_BG); break;
    case CS_FAILED: gfx_line(cx - k + 1, cy - k + 1, cx + k - 1, cy + k - 1, t + 1, C_BG); gfx_line(cx - k + 1, cy + k - 1, cx + k - 1, cy - k + 1, t + 1, C_BG); break;
    case CS_RUNNING: gfx_ring(cx, cy, k + 1, t, C_BG); gfx_rect(cx - t / 2, cy - k - 1, t, k + 1, C_BG); gfx_rect(cx, cy - t / 2, k, t, C_BG); break;
    case CS_SKIPPED: gfx_rect(cx - k, cy - t / 2, 2 * k, t + 1, C_BG); break;
    default: for (INTN i = -1; i <= 1; i++) gfx_circle(cx + i * (k - 1), cy, t, C_BG); break;
  }
}
/* a side view of a graphics card: bracket, body with two fans, PCIe edge connector, status badge */
static void draw_gpu(INTN x, INTN y, INTN w, INTN h, CARD_STATE st, BOOLEAN dim)
{
  UINT32 col = state_color(st), body = gfx_mix(C_PANEL2, col, dim ? 60 : 110);
  INTN u = w / 24; if (u < 2) u = 2;
  INTN bh = h * 78 / 100, bx = x + 3 * u, bw = w - 3 * u;
  gfx_rect(x, y, u + 1, h * 92 / 100, 0xA8B0BA);                                   /* slot bracket */
  gfx_rect(x, y + h / 5, u + 1, u, 0x5B636D); gfx_rect(x, y + h * 3 / 5, u + 1, u, 0x5B636D);
  gfx_rrect(bx, y + u, bw - u, bh, u, gfx_mix(body, 0, 40));                        /* shroud shadow */
  gfx_rrect(bx, y, bw - u, bh, u, body);
  gfx_rect(bx + u, y + u, bw - 3 * u, u / 2 + 1, gfx_mix(body, 0xFFFFFF, 60));      /* top highlight */
  INTN fr = bh * 36 / 100, fy = y + bh / 2;
  for (INTN f = 0; f < 2; f++) {
    INTN fx = bx + bw * (f ? 68 : 30) / 100;
    gfx_circle(fx, fy, fr + 2, 0x0A0D12); gfx_circle(fx, fy, fr, 0x12171E);
    for (UINTN i = f; i < 8; i += 2) gfx_line(fx + dir8[i][0] * 3 / 8 * fr / 256, fy + dir8[i][1] * 3 / 8 * fr / 256, fx + dir8[i][0] * (fr - 3) / 256, fy + dir8[i][1] * (fr - 3) / 256, (INTN)gfx_scale, 0x3A424D);
    gfx_ring(fx, fy, fr, 2, 0x56606C); gfx_circle(fx, fy, fr / 4 + 1, 0x56606C);
  }
  gfx_rect(bx + bw * 6 / 100, y + bh, bw * 56 / 100, h * 14 / 100, 0xC9A64B);       /* gold edge connector */
  for (INTN i = 0; i < 7; i++) gfx_rect(bx + bw * 6 / 100 + 2 + i * (bw * 56 / 100 / 7), y + bh + 2, 1 + (INTN)gfx_scale, h * 14 / 100 - 2, 0x8C7330);
  gfx_rect(bx + bw * 80 / 100, y - u / 2, bw * 12 / 100, u + 1, 0x0A0D12);          /* power connector */
  symbol(x + w - u * 2, y + u * 2, u * 3 + 3, st, col);
}

static void count_states(UINTN *g, UINTN *y, UINTN *r, UINTN *o)
{
  *g = *y = *r = *o = 0;
  for (UINTN i = 0; i < g_ncards; i++) switch (g_cards[i].state) { case CS_NATIVE: (*g)++; break; case CS_LOADED: (*y)++; break; case CS_FAILED: (*r)++; break; default: (*o)++; }
}
static void chip(INTN *x, INTN y, UINT32 col, const CHAR16 *s)
{
  gfx_circle(*x + (INTN)gfx_cw / 2, y + (INTN)gfx_ch / 2, (INTN)gfx_cw / 2 + 1, col);
  gfx_text(*x + (INTN)gfx_cw * 2, y, s, C_TEXT, FALSE); *x += (INTN)gfx_cw * 3 + text_w(s) + (INTN)gfx_cw * 2;
}

static void tile(INTN x, INTN y, INTN tw, INTN th, CARD *c, UINTN idx, BOOLEAN sel, BOOLEAN compact)
{
  INTN cw = (INTN)gfx_cw, ch = (INTN)gfx_ch; UINT32 col = state_color(c->state); BOOLEAN dim = (c->state == CS_PENDING || c->state == CS_SKIPPED);
  gfx_rrect(x, y, tw, th, cw, sel ? C_BLUE : gfx_mix(C_BORDER, col, dim ? 70 : 255));
  INTN b = sel ? 3 : 2; b *= (INTN)gfx_scale;
  gfx_rrect(x + b, y + b, tw - 2 * b, th - 2 * b, cw - 2, C_PANEL);
  if (c->state != CS_PENDING && c->state != CS_SKIPPED) gfx_rect(x + cw, y + b, tw - 2 * cw, (INTN)gfx_scale, col);
  CHAR16 t[120]; const CHAR16 *br = brand_of(c->ssv);
  CHAR16 head[40]; SPrint(head, sizeof head, L"CARD %d", idx + 1);
  CHAR16 bdf[40]; SPrint(bdf, sizeof bdf, L"%02x:%02x.%x", c->bus, c->dev, c->fn);
  PIN *pn = pin_find(c->bus, c->dev, c->fn, FALSE);
  UINTN maxc = (UINTN)(tw / cw) - 2;
  if (!compact) {
    gfx_text(x + cw, y + ch / 2, head, C_TEXT, TRUE); text_right(x + tw - cw, y + ch / 2, bdf, C_MUTED, FALSE);
    INTN iw = tw - 6 * cw; if (iw > 24 * cw) iw = 24 * cw; INTN ih = iw * 40 / 100;
    draw_gpu(x + (tw - iw) / 2, y + ch * 2 + ch / 2, iw, ih, c->state, dim);
    INTN ty = y + ch * 2 + ch / 2 + ih + ch;
    gfx_text(x + cw, ty, state_label(c->state), col, TRUE);
    if (pn && pn->rom[0]) text_right(x + tw - cw, ty, L"PINNED", C_BLUE, TRUE);
    SPrint(t, sizeof t, L"%s%s%04x:%04x", br ? br : L"", br ? L"  " : L"", c->ssv, c->ssi); gfx_text_n(x + cw, ty + ch * 5 / 4, t, maxc, C_TEXT, FALSE);
    SPrint(t, sizeof t, L"ROM %s", c->rom[0] ? c->rom : (c->state == CS_NATIVE ? L"(own chip)" : L"-")); gfx_text_n(x + cw, ty + ch * 5 / 2, t, maxc, C_MUTED, FALSE);
    if (c->vram_mb) SPrint(t, sizeof t, L"%d MB  %d.%d s  %d tr%s", c->vram_mb, c->ms / 1000, (c->ms / 100) % 10, c->tried, c->tried == 1 ? L"y" : L"ies");
    else if (c->tried) SPrint(t, sizeof t, L"%d ROM(s) tried", c->tried); else t[0] = 0;
    gfx_text_n(x + cw, ty + ch * 15 / 4, t, maxc, C_MUTED, FALSE);
    gfx_text_n(x + cw, ty + ch * 5, c->note, maxc, c->state == CS_FAILED ? C_RED : C_MUTED, FALSE);
  } else {
    INTN iw = cw * 11, ih = iw * 40 / 100 + ch;
    draw_gpu(x + cw, y + (th - ih) / 2, iw, iw * 40 / 100 + 2, c->state, dim);
    INTN tx = x + cw * 13; UINTN mc = (UINTN)((x + tw - cw - tx) / cw);
    gfx_text(tx, y + ch / 2, head, C_TEXT, TRUE); text_right(x + tw - cw, y + ch / 2, bdf, C_MUTED, FALSE);
    gfx_text(tx, y + ch * 3 / 2, state_label(c->state), col, TRUE);
    SPrint(t, sizeof t, L"%s%s%04x:%04x", br ? br : L"", br ? L" " : L"", c->ssv, c->ssi); gfx_text_n(tx, y + ch * 5 / 2, t, mc, C_TEXT, FALSE);
    SPrint(t, sizeof t, L"ROM %s", c->rom[0] ? c->rom : L"-"); gfx_text_n(tx, y + ch * 7 / 2, t, mc, C_MUTED, FALSE);
    gfx_text_n(tx, y + ch * 9 / 2, c->note, mc, c->state == CS_FAILED ? C_RED : C_MUTED, FALSE);
  }
}

static void gui_status(UINTN sel, const CHAR16 *footer)
{
  INTN W = (INTN)gfx_w, H = (INTN)gfx_h, cw = (INTN)gfx_cw, ch = (INTN)gfx_ch; CHAR16 t[200];
  gfx_clear(C_BG);
  /* header */
  gfx_rect(0, 0, W, ch * 5, C_PANEL); gfx_rect(0, ch * 5, W, (INTN)gfx_scale, C_BORDER);
  big_text(cw * 3, ch / 2 + 2, L"RX 580 vBIOS Loader", C_TEXT);
  SPrint(t, sizeof t, L"v%s    %d card(s)    %d ROM file(s)    config: %s", LOADER_VERSION, g_ncards, ncand, g_cfg_file_found ? L"file + defaults" : L"built-in defaults");
  gfx_text_n(cw * 3, ch * 3 + ch / 4, t, (UINTN)(W / cw) - 6, C_MUTED, FALSE);
  UINTN ng, ny, nr, no; count_states(&ng, &ny, &nr, &no);
  { INTN x = W - cw * 3 - (INTN)(cw * 3) * 3 - cw * 40; if (x < W / 2) x = W / 2;
    SPrint(t, sizeof t, L"%d ready", ng); chip(&x, ch / 2 + 4, C_GREEN, t);
    SPrint(t, sizeof t, L"%d loaded", ny); chip(&x, ch / 2 + 4, C_YELLOW, t);
    SPrint(t, sizeof t, L"%d failed", nr); chip(&x, ch / 2 + 4, C_RED, t); }
  /* footer */
  INTN fh = ch * 5, fy = H - fh;
  gfx_rect(0, fy, W, fh, C_PANEL); gfx_rect(0, fy, W, (INTN)gfx_scale, C_BORDER);
  if (g_activity[0] && sel == (UINTN)-1) gfx_text_n(cw * 3, fy + ch / 2, g_activity, (UINTN)(W / cw) - 6, C_BLUE, TRUE);
  else gfx_text_n(cw * 3, fy + ch / 2, footer ? footer : L"", (UINTN)(W / cw) - 6, C_TEXT, TRUE);
  { INTN x = cw * 3; chip(&x, fy + ch * 2, C_GREEN, L"ready: already initialised"); chip(&x, fy + ch * 2, C_YELLOW, L"initialised by the loader"); chip(&x, fy + ch * 2, C_RED, L"no ROM worked"); chip(&x, fy + ch * 2, C_GREY, L"waiting / disabled"); }
  gfx_text_n(cw * 3, fy + ch * 7 / 2, L"ARROWS select   C card/ROMs   S settings   L log   D dump regs   X disable   R forget history   A run   ENTER go   ESC skip", (UINTN)(W / cw) - 6, C_MUTED, FALSE);
  /* tiles */
  INTN ay0 = ch * 5 + ch, ay1 = fy - ch, ah = ay1 - ay0, mg = cw * 3, gap = cw * 2;
  if (!g_ncards) { gfx_text(cw * 3, ay0 + ch, L"No supported AMD GPU found in this system.", C_RED, TRUE); gfx_present(); return; }
  BOOLEAN compact = FALSE; INTN tw = 36 * cw, th = ch * 15;
  INTN cols = (W - 2 * mg + gap) / (tw + gap); if (cols < 1) cols = 1;
  INTN rows = ((INTN)g_ncards + cols - 1) / cols;
  if (rows * (th + gap) - gap > ah) { compact = TRUE; tw = 34 * cw; th = ch * 6; cols = (W - 2 * mg + gap) / (tw + gap); if (cols < 1) cols = 1; rows = ((INTN)g_ncards + cols - 1) / cols; }
  if (cols > (INTN)g_ncards) cols = (INTN)g_ncards;
  INTN maxw = compact ? 40 * cw : 50 * cw;                         /* let tiles grow a little when there is room */
  INTN avail = (W - 2 * mg - (cols - 1) * gap) / cols; if (avail > tw) tw = avail < maxw ? avail : maxw;
  g_cols_layout = (UINTN)cols;
  INTN vis_rows = (ah + gap) / (th + gap); if (vis_rows < 1) vis_rows = 1;
  INTN first = 0, selrow = (sel < g_ncards) ? (INTN)sel / cols : 0;
  if (rows > vis_rows) { first = selrow - vis_rows + 1; if (first < 0) first = 0; }
  INTN total_w = cols * tw + (cols - 1) * gap, x0 = (W - total_w) / 2;
  INTN used_h = (rows < vis_rows ? rows : vis_rows) * (th + gap) - gap, y0 = ay0 + (ah - used_h) / 3;
  for (UINTN i = 0; i < g_ncards; i++) {
    INTN r = (INTN)i / cols - first, c = (INTN)i % cols; if (r < 0 || r >= vis_rows) continue;
    tile(x0 + c * (tw + gap), y0 + r * (th + gap), tw, th, &g_cards[i], i, i == sel, compact);
  }
  if (rows > vis_rows) { SPrint(t, sizeof t, L"rows %d-%d of %d", first + 1, first + vis_rows, rows); text_right(W - cw * 3, ay1 - ch, t, C_MUTED, FALSE); }
  gfx_present();
}

/* ================================================================== GUI lists / message ======== */
static UINTN list_visible(void) { INTN H = (INTN)gfx_h, ch = (INTN)gfx_ch; INTN v = (H - ch * 4 - ch * 5 - ch * 3) / (ch * 8 / 5); return v < 3 ? 3 : (UINTN)v; }
static void gui_list(LIST *l)
{
  INTN W = (INTN)gfx_w, H = (INTN)gfx_h, cw = (INTN)gfx_cw, ch = (INTN)gfx_ch, rh = ch * 8 / 5;
  gfx_clear(C_BG);
  gfx_rrect(cw * 2, ch, W - cw * 4, H - ch * 2, cw, C_PANEL);
  gfx_text_n(cw * 4, ch * 2, l->title, (UINTN)(W / cw) - 8, C_TEXT, TRUE);
  if (l->sub) gfx_text_n(cw * 4, ch * 7 / 2, l->sub, (UINTN)(W / cw) - 8, C_MUTED, FALSE);
  gfx_rect(cw * 4, ch * 5, W - cw * 8, (INTN)gfx_scale, C_BORDER);
  UINTN vis = list_visible();
  if (l->sel < l->top) l->top = l->sel; if (l->sel >= l->top + vis) l->top = l->sel - vis + 1;
  INTN y = ch * 11 / 2; INTN rw = W - cw * 8;
  for (UINTN i = l->top; i < l->n && i < l->top + vis; i++, y += rh) {
    if (i == l->sel) gfx_rrect(cw * 4, y - ch / 6, rw, rh, cw / 2 + 1, 0x1F3A5F);
    UINTN rc = StrLen(l->rows[i].right); UINTN lmax = (UINTN)(rw / cw) - 4 - (rc ? rc + 2 : 0);
    gfx_text_n(cw * 5, y, l->rows[i].left, lmax, l->rows[i].color, i == l->sel);
    if (rc) text_right(cw * 4 + rw - cw, y, l->rows[i].right, i == l->sel ? C_BLUE : C_MUTED, FALSE);
  }
  if (l->n > vis) { CHAR16 t[40]; SPrint(t, sizeof t, L"%d-%d of %d", l->top + 1, l->top + vis < l->n ? l->top + vis : l->n, l->n); text_right(W - cw * 4, H - ch * 5 / 2 - ch, t, C_MUTED, FALSE); }
  gfx_rect(cw * 4, H - ch * 4, W - cw * 8, (INTN)gfx_scale, C_BORDER);
  if (l->footer) gfx_text_n(cw * 4, H - ch * 3, l->footer, (UINTN)(W / cw) - 8, C_MUTED, FALSE);
  gfx_present();
}
static void gui_message(const CHAR16 *title, const CHAR16 *a, const CHAR16 *b, BOOLEAN wait)
{
  INTN W = (INTN)gfx_w, H = (INTN)gfx_h, cw = (INTN)gfx_cw, ch = (INTN)gfx_ch;
  INTN bw = W * 3 / 4 < 90 * cw ? W * 3 / 4 : 90 * cw, bh = ch * 8, x = (W - bw) / 2, y = (H - bh) / 2;
  gfx_rrect(x - 2, y - 2, bw + 4, bh + 4, cw, C_BLUE); gfx_rrect(x, y, bw, bh, cw, C_PANEL);
  gfx_text(x + cw * 2, y + ch, title, C_TEXT, TRUE);
  gfx_text_n(x + cw * 2, y + ch * 3, a, (UINTN)(bw / cw) - 4, C_TEXT, FALSE);
  if (b) gfx_text_n(x + cw * 2, y + ch * 9 / 2, b, (UINTN)(bw / cw) - 4, C_MUTED, FALSE);
  if (wait) gfx_text(x + cw * 2, y + ch * 6, L"press any key", C_MUTED, FALSE);
  gfx_present();
}

/* ================================================================== text mode ================== */
static UINTN tcols, trows;
/* left-aligned, truncated/padded copy (gnu-efi's %-Ns does not pad) */
static void padcat(CHAR16 *d, const CHAR16 *src, UINTN w)
{
  UINTN n = StrLen(d), i = 0; while (src[i] && i < w) { d[n + i] = src[i]; i++; } for (; i < w; i++) d[n + i] = L' '; d[n + w] = 0;
}
static void tx_begin(void)
{
  FW(ST->ConOut->QueryMode, ST->ConOut, ST->ConOut->Mode->Mode, &tcols, &trows);
  if (tcols < 40) tcols = 80; if (trows < 15) trows = 25;
  FW(ST->ConOut->SetAttribute, ST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)); FW(ST->ConOut->ClearScreen, ST->ConOut);
}
static void tx_put(UINTN col, UINTN row, UINTN attr, const CHAR16 *s)
{
  if (row + 1 >= trows || col >= tcols) return;
  CHAR16 b[200]; UINTN n = 0, mx = tcols - 1 - col; if (mx > 190) mx = 190;
  while (s[n] && n < mx) { b[n] = s[n]; n++; } b[n] = 0;
  FW(ST->ConOut->SetCursorPosition, ST->ConOut, col, row); FW(ST->ConOut->SetAttribute, ST->ConOut, attr); FW(ST->ConOut->OutputString, ST->ConOut, b);
}
static UINTN tx_attr(CARD_STATE s)
{
  switch (s) { case CS_NATIVE: return EFI_TEXT_ATTR(EFI_LIGHTGREEN, EFI_BLACK); case CS_LOADED: return EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLACK);
               case CS_FAILED: return EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BLACK); case CS_RUNNING: return EFI_TEXT_ATTR(EFI_LIGHTCYAN, EFI_BLACK);
               default: return EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK); }
}
static void txt_status(UINTN sel, const CHAR16 *footer)
{
  CHAR16 t[200]; tx_begin();
  tx_put(1, 0, EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE), L" RX 580 vBIOS Loader                                                                          ");
  SPrint(t, sizeof t, L" v%s  -  %d card(s), %d ROM file(s), config: %s", LOADER_VERSION, g_ncards, ncand, g_cfg_file_found ? g_cfg_path : L"built-in defaults");
  tx_put(0, 1, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), t);
  t[0] = 0; padcat(t, L"   #", 5); padcat(t, L"PCI", 9); padcat(t, L"Subsystem", 15); padcat(t, L"State", 10); padcat(t, L"ROM", 30); padcat(t, L"VRAM", 8);
  tx_put(0, 3, EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK), t);
  for (UINTN i = 0; i < g_ncards && 4 + i + 6 < trows; i++) {
    CARD *c = &g_cards[i]; const CHAR16 *br = brand_of(c->ssv); CHAR16 x[40];
    t[0] = 0;
    SPrint(x, sizeof x, L"%s%2d", i == sel ? L" >" : L"  ", i + 1); padcat(t, x, 5);
    SPrint(x, sizeof x, L"%02x:%02x.%x", c->bus, c->dev, c->fn); padcat(t, x, 9);
    SPrint(x, sizeof x, L"%04x:%04x %s", c->ssv, c->ssi, br ? br : L""); padcat(t, x, 15);
    SPrint(x, sizeof x, L"%s", state_label(c->state)); padcat(t, x, 10);
    padcat(t, c->rom[0] ? c->rom : (c->state == CS_NATIVE ? L"(own chip)" : L"-"), 30);
    if (c->vram_mb) { SPrint(x, sizeof x, L"%d MB", c->vram_mb); padcat(t, x, 8); }
    tx_put(0, 4 + i, i == sel ? (tx_attr(c->state) | EFI_BACKGROUND_BLUE) : tx_attr(c->state), t);
  }
  if (g_ncards && sel < g_ncards && g_cards[sel].note[0]) { SPrint(t, sizeof t, L"  Card %d: %s", sel + 1, g_cards[sel].note); tx_put(0, 5 + g_ncards, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), t); }
  if (!g_ncards) tx_put(2, 5, EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BLACK), L"No supported AMD GPU found in this system.");
  tx_put(0, trows - 5, EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK), (g_activity[0] && sel == (UINTN)-1) ? g_activity : (footer ? footer : L""));
  tx_put(0, trows - 4, EFI_TEXT_ATTR(EFI_LIGHTGREEN, EFI_BLACK), L" green  = ready (already initialised)");
  tx_put(0, trows - 3, EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLACK), L" yellow = initialised by the loader   red = no ROM worked");
  tx_put(0, trows - 2, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), L" C card/ROMs  S settings  L log  D dump  X disable  R forget  A run  ENTER go  ESC");
}
static void txt_list(LIST *l)
{
  tx_begin();
  tx_put(1, 0, EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE), l->title);
  if (l->sub) tx_put(1, 1, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), l->sub);
  UINTN vis = trows > 8 ? trows - 6 : 3;
  if (l->sel < l->top) l->top = l->sel; if (l->sel >= l->top + vis) l->top = l->sel - vis + 1;
  for (UINTN i = l->top; i < l->n && i < l->top + vis; i++) {
    CHAR16 t[220]; t[0] = 0; padcat(t, i == l->sel ? L">" : L" ", 2); padcat(t, l->rows[i].left, 50); padcat(t, l->rows[i].right, 30);
    tx_put(0, 3 + (i - l->top), i == l->sel ? EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE) : EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), t);
  }
  if (l->footer) tx_put(0, trows - 2, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), l->footer);
}

/* ================================================================== public ===================== */
void ui_draw_status(UINTN sel, const CHAR16 *footer) { if (g_ui_gui) gui_status(sel, footer); else txt_status(sel, footer); }
void ui_progress(void) { if (g_ui_gui) gui_status((UINTN)-1, g_activity); }
void ui_list_draw(LIST *l) { if (g_ui_gui) gui_list(l); else txt_list(l); }
void ui_message(const CHAR16 *title, const CHAR16 *a, const CHAR16 *b, BOOLEAN wait)
{
  if (g_ui_gui) gui_message(title, a, b, wait);
  else { tx_begin(); tx_put(2, 3, EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE), title); tx_put(2, 5, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), a); if (b) tx_put(2, 6, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), b); if (wait) tx_put(2, 8, EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK), L"press any key"); }
  if (wait) { KEY k; ui_key(&k, 0); }
}

/* ---------- settings page ---------- */
void ui_run_settings(void)
{
  ROW *rows = AllocateZeroPool(sizeof(ROW) * (S_COUNT + 4)); UINTN sel = 0, top = 0;
  for (;;) {
    UINTN n = 0; CHAR16 sub[200], foot[200];
    for (UINTN i = 0; i < S_COUNT; i++, n++) {
      SPrint(rows[n].left, sizeof rows[n].left, L"%s", g_setdef[i].label);
      SPrint(rows[n].right, sizeof rows[n].right, L"< %s >   %s", cfg_value_str(i), g_origin[i] == OR_NVRAM ? L"[NVRAM]" : (g_origin[i] == OR_FILE ? L"[config file]" : L"[default]"));
      rows[n].color = g_origin[i] == OR_NVRAM ? C_BLUE : C_TEXT;
    }
    StrCpy(rows[n].left, L"Remove ALL NVRAM overrides (back to config file / defaults)"); rows[n].right[0] = 0; rows[n].color = C_YELLOW; n++;
    StrCpy(rows[n].left, L"Back"); rows[n].right[0] = 0; rows[n].color = C_TEXT; n++;
    if (sel < S_COUNT) SPrint(sub, sizeof sub, L"%s", g_setdef[sel].help); else SPrint(sub, sizeof sub, L"Config file: %s    ROM folder: %s", g_cfg_file_found ? g_cfg_path : L"(none)", g_romdir ? g_romdir : L"(none)");
    SPrint(foot, sizeof foot, L"UP/DOWN select   LEFT/RIGHT change   DEL back to file/default   changes are stored in NVRAM at once   ESC back");
    LIST l = { L"Settings", sub, foot, rows, n, sel, top };
    ui_list_draw(&l); sel = l.sel; top = l.top;
    KEY k; if (!ui_key(&k, 0)) continue;
    if (k.sc == 0x17) break;
    if (k.sc == 0x01 && sel > 0) sel--;
    if (k.sc == 0x02 && sel + 1 < n) sel++;
    if (k.sc == 0x09) sel = sel > 8 ? sel - 8 : 0;
    if (k.sc == 0x0A) sel = sel + 8 < n ? sel + 8 : n - 1;
    if (sel < S_COUNT) {
      if (k.sc == 0x03 || k.ch == L'\r') cfg_step(sel, 1);
      if (k.sc == 0x04) cfg_step(sel, -1);
      if (k.sc == 0x08) cfg_clear_override(sel);
    } else if (k.ch == L'\r') { if (sel == S_COUNT) cfg_reset_nvram(); else break; }
  }
  FreePool(rows);
}

/* ---------- log page ---------- */
void ui_run_log(void)
{
  ROW *rows = AllocateZeroPool(sizeof(ROW) * (LOG_LINES + 2)); UINTN n = 0;
  for (UINTN i = 0; i <= g_nlog && i < LOG_LINES; i++) { StrnCpy(rows[n].left, g_log[i], 99); rows[n].left[99] = 0; rows[n].color = C_TEXT; n++; }
  UINTN sel = n ? n - 1 : 0, top = 0;
  for (;;) {
    LIST l = { L"Log", L"Everything the loader reported during this boot (also in loader_trace.txt)", L"UP/DOWN/PGUP/PGDN scroll   HOME/END   ESC back", rows, n ? n : 1, sel, top };
    ui_list_draw(&l); sel = l.sel; top = l.top;
    KEY k; if (!ui_key(&k, 0)) continue;
    if (k.sc == 0x17) break;
    if (k.sc == 0x01 && sel > 0) sel--;
    if (k.sc == 0x02 && sel + 1 < n) sel++;
    if (k.sc == 0x09) sel = sel > 10 ? sel - 10 : 0;
    if (k.sc == 0x0A) sel = sel + 10 < n ? sel + 10 : (n ? n - 1 : 0);
    if (k.sc == 0x05) sel = 0;
    if (k.sc == 0x06) sel = n ? n - 1 : 0;
  }
  FreePool(rows);
}
