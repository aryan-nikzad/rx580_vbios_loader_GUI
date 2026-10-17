[Defines]
  DSC_SPECIFICATION              = 0x0001001C
  PLATFORM_NAME                  = Rx580ReBarEarly
  PLATFORM_GUID                  = CED68C9B-CC23-49C6-A7FD-5D8A1ED12FC4
  PLATFORM_VERSION               = 1.0
  OUTPUT_DIRECTORY               = Build/Rx580ReBarEarly
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE

[Components]
  experimental/rx580-rebar-dxe/early/Rx580ReBarEarlyDxe.inf

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
