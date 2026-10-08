/* loader.h - types shared between the engine (vbios_loader.c), config, gfx and ui modules. */
#ifndef LOADER_H
#define LOADER_H
#include <efi.h>
#include <efilib.h>

#define NARGS_(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,N,...) N
#define NARGS(...) NARGS_(__VA_ARGS__,10,9,8,7,6,5,4,3,2,1,0)
#define FW(f, ...) uefi_call_wrapper((f), NARGS(__VA_ARGS__), __VA_ARGS__)   /* MS ABI */

#define LOADER_VERSION  L"0.2-beta"
#define LOADER_DIRNAME  L"vbios_loader"            /* \EFI\vbios_loader */
#define CFG_FILENAME    L"vbios_loader.cfg"
#define MAX_CARDS       16
#define NRES            64

/* ---------------- settings (config.c) ---------------- */
enum {
  S_UI_MODE, S_BOOT_MODE, S_MENU_TIMEOUT, S_SKIP_INIT, S_RESET_MODE, S_SMART_ORDER, S_PIN_FALLBACK,
  S_RETRY_FAILED, S_FINISH_SECS, S_HOLD_ON_FAIL, S_LOOP_MS, S_ENGINE, S_GOP_TIMEOUT, S_VFCT_ONLY,
  S_POSTDUMP, S_TRACE, S_UI_SCALE, S_DEMO, S_VFCT_MODE, S_COUNT
};
enum { UI_AUTO, UI_GUI, UI_TEXT };
enum { BM_AUTO, BM_MENU };
enum { RS_GPU, RS_PCIE, RS_REBOOT, RS_NONE };
enum { ENG_ATOM, ENG_GOP };
enum { VM_AUTO, VM_ACPI, VM_XSDT, VM_OFF };
enum { OR_DEFAULT, OR_FILE, OR_NVRAM };

typedef struct {
  const CHAR8 *key; UINT8 type;           /* 0 bool, 1 enum, 2 int */
  INT32 lo, hi, def;
  const CHAR8 *choices;                   /* enum: "a|b|c" */
  const CHAR16 *label, *help;
} SETDEF;

typedef struct { UINT8 bus, dev, fn; CHAR16 rom[72]; BOOLEAN skip; CHAR16 name[24]; UINT8 origin; } PIN;

extern INT32 g_set[S_COUNT]; extern UINT8 g_origin[S_COUNT]; extern const SETDEF g_setdef[S_COUNT];
extern CHAR16 g_cfg_romdir[128]; extern PIN g_pins[MAX_CARDS]; extern UINTN g_npins;
extern BOOLEAN g_cfg_file_found; extern CHAR16 g_cfg_path[160];
#define CFG(id) (g_set[id])

void   cfg_defaults(void);
void   cfg_parse(const CHAR8 *buf, UINTN len, UINT8 origin);
void   cfg_load_file(EFI_FILE_HANDLE root, const CHAR16 *owndir);
void   cfg_load_nvram(void);
void   cfg_save_nvram(void);
void   cfg_reset_nvram(void);
CHAR16 *cfg_value_str(UINTN id);                    /* static buffer */
void   cfg_step(UINTN id, INTN dir);                /* change a value (wraps) and mark as NVRAM override */
void   cfg_clear_override(UINTN id);                /* drop NVRAM override -> falls back to file/default */
PIN   *pin_find(UINTN bus, UINTN dev, UINTN fn, BOOLEAN create);
void   pin_commit(void);                            /* persist pins in NVRAM */

/* ---------------- cards ---------------- */
typedef enum { CS_PENDING, CS_RUNNING, CS_NATIVE, CS_LOADED, CS_FAILED, CS_SKIPPED } CARD_STATE;

#define STATE_MAGIC 0x344C4256u
typedef struct {            /* per-card persistent history (NVRAM variable VbiosLdrC<bdf>) */
  UINT32 magic, sig, next, strikes, pinstrikes;
  CHAR16 working[80];
  UINT8  res[NRES]; UINT32 membefore[NRES]; UINT8 loops[NRES];
} CHIST;

typedef struct {
  EFI_HANDLE h; EFI_PCI_IO_PROTOCOL *pio;
  UINTN seg, bus, dev, fn;
  UINT16 did, ssv, ssi; UINT64 mmio;
  CARD_STATE state;
  CHAR16 rom[72];            /* ROM in use ("" = none / native) */
  CHAR16 note[100];
  UINT32 vram_mb; UINT32 tried; UINT32 ms;
  BOOLEAN demo;
  CHIST hist;
} CARD;
extern CARD g_cards[MAX_CARDS]; extern UINTN g_ncards;

/* ---------------- ROM candidates ---------------- */
typedef struct { CHAR16 name[72]; UINT16 vid, did, ssv, ssi; UINT32 size; BOOLEAN ok; } CAND;
extern CAND *cands; extern UINTN ncand; extern CHAR16 *g_romdir; extern CHAR16 *g_owndir;

/* ---------------- log + ui hooks ---------------- */
void lg(const CHAR16 *fmt, ...);                    /* log line: ring buffer, console in text mode */
#define LOG_LINES 300
#define LOG_W     118
extern CHAR16 g_log[LOG_LINES][LOG_W]; extern UINTN g_nlog;
extern CHAR16 g_activity[120];
extern BOOLEAN g_ui_gui;

/* ui.c */
typedef struct { UINT16 sc; CHAR16 ch; } KEY;
BOOLEAN ui_init(void);
void    ui_end(void);
BOOLEAN ui_key(KEY *k, UINTN timeout_ms);           /* FALSE on timeout */
void    ui_draw_status(UINTN sel, const CHAR16 *footer);
void    ui_progress(void);
UINTN   ui_cols(void);
const CHAR16 *brand_of(UINT16 ssv);
void    ui_run_settings(void);
void    ui_run_log(void);
void    ui_message(const CHAR16 *title, const CHAR16 *line1, const CHAR16 *line2, BOOLEAN wait);
typedef struct { CHAR16 left[100]; CHAR16 right[60]; UINT32 color; } ROW;
typedef struct { const CHAR16 *title, *sub, *footer; ROW *rows; UINTN n, sel, top; } LIST;
void    ui_list_draw(LIST *l);
const CHAR16 *state_label(CARD_STATE s);

/* engine entry points used by ui.c */
const CHAR16 *res_tag_for(CARD *c, UINTN i);
#endif
