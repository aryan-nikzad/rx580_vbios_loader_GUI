#ifndef GFX_H
#define GFX_H
#include "loader.h"
extern UINTN gfx_w, gfx_h, gfx_scale, gfx_cw, gfx_ch;     /* cell = one font character at the current scale */
BOOLEAN gfx_init(UINTN scale_cfg);
void gfx_close(void);
void gfx_clear(UINT32 c);
void gfx_rect(INTN x, INTN y, INTN w, INTN h, UINT32 c);
void gfx_rrect(INTN x, INTN y, INTN w, INTN h, INTN r, UINT32 c);
void gfx_circle(INTN cx, INTN cy, INTN r, UINT32 c);
void gfx_ring(INTN cx, INTN cy, INTN r, INTN thick, UINT32 c);
void gfx_line(INTN x0, INTN y0, INTN x1, INTN y1, INTN thick, UINT32 c);
void gfx_text(INTN x, INTN y, const CHAR16 *s, UINT32 c, BOOLEAN bold);
void gfx_text_n(INTN x, INTN y, const CHAR16 *s, UINTN maxcols, UINT32 c, BOOLEAN bold);   /* truncates with ".." */
UINT32 gfx_mix(UINT32 a, UINT32 b, UINTN t256);            /* t=0 -> a, 256 -> b */
void gfx_present(void);
#endif
