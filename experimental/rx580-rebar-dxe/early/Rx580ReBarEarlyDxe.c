/*
 * Rx580ReBarEarlyDxe.c
 * Early BAR0 sizing request for RX 580 / Polaris 10 (1002:67DF).
 *
 * The hook follows the host-bridge resource-allocation approach of
 * xCuri0/ReBarUEFI (MIT, copyright xCuri0 2022-2023).
 *
 * This module does not fake a missing PCIe capability or run ASIC_Init. It
 * selects 8 GiB only when the live endpoint advertises that size. Firmware
 * must still have enough 64-bit MMIO space to allocate the BAR.
 */
#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>

#define RX580_VENDOR_ID       0x1002
#define RX580_POLARIS10_DID   0x67DF
#define PCI_EXT_CAP_START     0x100
#define PCI_EXT_CAP_END       0x1000
#define PCI_EXT_CAP_REBAR     0x0015
#define REBAR_CTRL_NBAR_MASK  0x000000E0
#define REBAR_CTRL_NBAR_SHIFT 5
#define REBAR_CTRL_BAR_INDEX  0x00000007
#define REBAR_CTRL_SIZE_MASK  0x00001F00
#define REBAR_CTRL_SIZE_SHIFT 8
#define REBAR_SIZE_8G_EXP     13
#define REBAR_8G_MASK_BIT     (1U << REBAR_SIZE_8G_EXP)
#define MAX_HOST_BRIDGES      16
#define LOG_CHARS             4096
#define LOG_VAR_NAME          L"RX580ReBarEarlyLog"
#define LOG_VAR_ATTRIBUTES    (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

STATIC EFI_GUID mLogGuid = {
  0x5d4c6f81, 0x70c0, 0x49a6, { 0x96, 0x2b, 0x44, 0x0e, 0x58, 0x11, 0x92, 0x70 }
};

typedef EFI_STATUS (EFIAPI *PREPROCESS_CONTROLLER)(
  IN EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *This,
  IN EFI_HANDLE RootBridgeHandle,
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS PciAddress,
  IN EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE Phase
);

typedef struct {
  EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *Protocol;
  PREPROCESS_CONTROLLER Original;
} HOST_HOOK;

STATIC HOST_HOOK mHooks[MAX_HOST_BRIDGES];
STATIC UINTN mHookCount;
STATIC CHAR16 mLog[LOG_CHARS];
STATIC UINTN mLogChars;

STATIC
VOID
LogLine (
  IN CONST CHAR16 *Format,
  ...
  )
{
  VA_LIST Args;
  EFI_STATUS Status;
  UINTN RemainingBytes;

  if (mLogChars + 2 >= LOG_CHARS) {
    return;
  }

  RemainingBytes = (LOG_CHARS - mLogChars) * sizeof (CHAR16);
  VA_START (Args, Format);
  UnicodeVSPrint (&mLog[mLogChars], RemainingBytes, Format, Args);
  VA_END (Args);
  mLogChars = StrLen (mLog);
  Status = gRT->SetVariable (
                  LOG_VAR_NAME,
                  &mLogGuid,
                  LOG_VAR_ATTRIBUTES,
                  (mLogChars + 1) * sizeof (CHAR16),
                  mLog
                  );
  (VOID)Status; /* The diagnostic variable is optional; BAR handling is not. */
}

STATIC
UINT64
PciAddressWithOffset (
  IN UINTN PciAddress,
  IN UINTN Offset
  )
{
  UINTN Register = (UINTN)(((UINT64)PciAddress & 0xffffffff00000000ULL) >> 32);
  UINTN Bus      = (PciAddress & 0xff000000U) >> 24;
  UINTN Device   = (PciAddress & 0x00ff0000U) >> 16;
  UINTN Function = (PciAddress & 0x0000ff00U) >> 8;

  return EFI_PCI_ADDRESS (Bus, Device, Function, Register + Offset);
}

STATIC
EFI_STATUS
PciRead32 (
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *RootIo,
  IN UINTN PciAddress,
  IN UINTN Offset,
  OUT UINT32 *Value
  )
{
  *Value = 0xffffffffU;
  return RootIo->Pci.Read (
                       RootIo,
                       EfiPciWidthUint32,
                       PciAddressWithOffset (PciAddress, Offset),
                       1,
                       Value
                       );
}

STATIC
EFI_STATUS
PciWrite32 (
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *RootIo,
  IN UINTN PciAddress,
  IN UINTN Offset,
  IN UINT32 Value
  )
{
  return RootIo->Pci.Write (
                       RootIo,
                       EfiPciWidthUint32,
                       PciAddressWithOffset (PciAddress, Offset),
                       1,
                       &Value
                       );
}

STATIC
UINTN
FindExtendedCapability (
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *RootIo,
  IN UINTN PciAddress,
  IN UINT16 WantedId
  )
{
  UINT32 Header;
  UINTN Offset = PCI_EXT_CAP_START;
  UINTN Guard;

  for (Guard = 0; Guard < ((PCI_EXT_CAP_END - PCI_EXT_CAP_START) / 8); Guard++) {
    if (EFI_ERROR (PciRead32 (RootIo, PciAddress, Offset, &Header))) {
      return 0;
    }
    if (Header == 0 || Header == 0xffffffffU) {
      return 0;
    }
    if ((Header & 0xffffU) == WantedId) {
      return Offset;
    }
    Offset = (Header >> 20) & 0x0ffcU;
    if (Offset < PCI_EXT_CAP_START || Offset > 0x0ff8U) {
      return 0;
    }
  }
  return 0;
}

STATIC
VOID
RequestRx580Bar0EightGiB (
  IN EFI_HANDLE RootBridgeHandle,
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS Address,
  IN EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE Phase
  )
{
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *RootIo;
  EFI_STATUS Status;
  UINTN PciAddress;
  UINT16 Vendor;
  UINT16 Device;
  UINTN RebarOffset;
  UINT32 FirstControl;
  UINTN NumBars;
  UINTN Index;
  UINT32 Capability;
  UINT32 Control;
  UINT32 ReadBack;
  UINT32 SupportedMask;
  UINT32 CurrentSize;
  UINTN CapOffset;
  UINTN CtrlOffset;

  Status = gBS->HandleProtocol (
                  RootBridgeHandle,
                  &gEfiPciRootBridgeIoProtocolGuid,
                  (VOID **)&RootIo
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  PciAddress = EFI_PCI_ADDRESS (Address.Bus, Address.Device, Address.Function, 0);
  Status = RootIo->Pci.Read (RootIo, EfiPciWidthUint16, PciAddress, 1, &Vendor);
  if (EFI_ERROR (Status) || Vendor != RX580_VENDOR_ID) {
    return;
  }
  Status = RootIo->Pci.Read (RootIo, EfiPciWidthUint16,
                             PciAddressWithOffset (PciAddress, 2), 1, &Device);
  if (EFI_ERROR (Status) || Device != RX580_POLARIS10_DID) {
    return;
  }

  LogLine (L"RX580 ReBAR DXE: BDF %02x:%02x.%x phase=%u vendor/device=%04x:%04x\r\n",
           Address.Bus, Address.Device, Address.Function, (UINT32)Phase, Vendor, Device);

  RebarOffset = FindExtendedCapability (RootIo, PciAddress, PCI_EXT_CAP_REBAR);
  if (RebarOffset == 0) {
    LogLine (L"  ReBAR capability absent before resource collection; no synthetic capability or unsafe write attempted.\r\n");
    return;
  }

  if (EFI_ERROR (PciRead32 (RootIo, PciAddress, RebarOffset + 8, &FirstControl))) {
    LogLine (L"  ReBAR @%03x found but control read failed.\r\n", (UINT32)RebarOffset);
    return;
  }

  NumBars = (FirstControl & REBAR_CTRL_NBAR_MASK) >> REBAR_CTRL_NBAR_SHIFT;
  if (NumBars == 0) {
    NumBars = 1;
  }
  if (NumBars > 6) {
    LogLine (L"  Invalid ReBAR entry count=%u; refusing to write.\r\n", (UINT32)NumBars);
    return;
  }

  for (Index = 0; Index < NumBars; Index++) {
    CapOffset = RebarOffset + 4 + 8 * Index;
    CtrlOffset = RebarOffset + 8 + 8 * Index;
    if (EFI_ERROR (PciRead32 (RootIo, PciAddress, CapOffset, &Capability)) ||
        EFI_ERROR (PciRead32 (RootIo, PciAddress, CtrlOffset, &Control))) {
      LogLine (L"  ReBAR entry %u read failed; skipping.\r\n", (UINT32)Index);
      continue;
    }
    if ((Control & REBAR_CTRL_BAR_INDEX) != 0) {
      continue; /* Only resize BAR0, the RX 580 VRAM aperture. */
    }

    SupportedMask = (Capability >> 4) & 0x000fffffU;
    CurrentSize = (Control & REBAR_CTRL_SIZE_MASK) >> REBAR_CTRL_SIZE_SHIFT;
    LogLine (L"  BAR0 ReBAR @%03x cap=%08x mask=%05x current-exp=%u target-exp=%u\r\n",
             (UINT32)RebarOffset, Capability, SupportedMask, CurrentSize, REBAR_SIZE_8G_EXP);

    if ((SupportedMask & REBAR_8G_MASK_BIT) == 0) {
      LogLine (L"  8 GiB is not advertised by BAR0 (mask bit 13 clear); no write performed.\r\n");
      return;
    }
    if (CurrentSize == REBAR_SIZE_8G_EXP) {
      LogLine (L"  BAR0 already selects 8 GiB.\r\n");
      return;
    }

    Control = (Control & ~REBAR_CTRL_SIZE_MASK) |
              (REBAR_SIZE_8G_EXP << REBAR_CTRL_SIZE_SHIFT);
    Status = PciWrite32 (RootIo, PciAddress, CtrlOffset, Control);
    if (EFI_ERROR (Status)) {
      LogLine (L"  Write to ReBAR control @%03x failed: %r\r\n", (UINT32)CtrlOffset, Status);
      return;
    }
    if (EFI_ERROR (PciRead32 (RootIo, PciAddress, CtrlOffset, &ReadBack))) {
      LogLine (L"  BAR0 size was written but readback failed.\r\n");
      return;
    }
    LogLine (L"  BAR0 control new=%08x readback=%08x result=%s\r\n",
             Control, ReadBack,
             (((ReadBack & REBAR_CTRL_SIZE_MASK) >> REBAR_CTRL_SIZE_SHIFT) == REBAR_SIZE_8G_EXP)
               ? L"8 GiB selected before resource allocation"
               : L"FAILED");
    return;
  }

  LogLine (L"  ReBAR capability exists but has no BAR0 entry.\r\n");
}

STATIC
PREPROCESS_CONTROLLER
GetOriginal (
  IN EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *This
  )
{
  UINTN Index;
  for (Index = 0; Index < mHookCount; Index++) {
    if (mHooks[Index].Protocol == This) {
      return mHooks[Index].Original;
    }
  }
  return NULL;
}

STATIC
EFI_STATUS
EFIAPI
PreprocessControllerOverride (
  IN EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *This,
  IN EFI_HANDLE RootBridgeHandle,
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS PciAddress,
  IN EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE Phase
  )
{
  PREPROCESS_CONTROLLER Original;
  EFI_STATUS Status;

  Original = GetOriginal (This);
  if (Original == NULL) {
    return EFI_UNSUPPORTED;
  }

  Status = Original (This, RootBridgeHandle, PciAddress, Phase);
  if (Phase <= EfiPciBeforeResourceCollection) {
    RequestRx580Bar0EightGiB (RootBridgeHandle, PciAddress, Phase);
  }
  return Status;
}

EFI_STATUS
EFIAPI
Rx580ReBarEarlyDxeEntryPoint (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN HandleCount = 0;
  UINTN Index;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  mLog[0] = L'\0';
  mLogChars = 0;
  LogLine (L"RX580 ReBAR DXE started. Target=1002:67DF BAR0=8GiB; no unsupported-size writes.\r\n");

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciHostBridgeResourceAllocationProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    LogLine (L"LocateHandleBuffer(HostBridgeResourceAllocation) failed: %r\r\n", Status);
    return EFI_SUCCESS; /* Diagnostics must never block boot. */
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *Protocol;

    Protocol = NULL;
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiPciHostBridgeResourceAllocationProtocolGuid,
                    (VOID **)&Protocol
                    );
    if (EFI_ERROR (Status) || Protocol == NULL ||
        Protocol->PreprocessController == NULL ||
        Protocol->PreprocessController == PreprocessControllerOverride) {
      continue;
    }
    if (mHookCount >= MAX_HOST_BRIDGES) {
      LogLine (L"Host-bridge hook capacity reached; remaining bridges untouched.\r\n");
      break;
    }

    mHooks[mHookCount].Protocol = Protocol;
    mHooks[mHookCount].Original = Protocol->PreprocessController;
    Protocol->PreprocessController = PreprocessControllerOverride;
    LogLine (L"Hooked host bridge %u (PreprocessController=%p).\r\n",
             (UINT32)Index, Protocol->PreprocessController);
    mHookCount++;
  }

  if (Handles != NULL) {
    gBS->FreePool (Handles);
  }
  LogLine (L"Installed %u host-bridge hook(s). Log variable: RX580ReBarEarlyLog.\r\n",
           (UINT32)mHookCount);
  return EFI_SUCCESS;
}
