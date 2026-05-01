#pragma once

#include <stddef.h>
#include <stdint.h>

using BOOLEAN = uint8_t;
using INT16 = int16_t;
using INT32 = int32_t;
using INT64 = int64_t;
using UINT8 = uint8_t;
using UINT16 = uint16_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using UINTN = uint64_t;
using CHAR16 = uint16_t;
using EFI_HANDLE = void *;
using EFI_EVENT = void *;
using EFI_LBA = UINT64;
using EFI_TPL = UINTN;
using EFI_PHYSICAL_ADDRESS = UINT64;
using EFI_VIRTUAL_ADDRESS = UINT64;
using EFI_STATUS = UINT64;

#define EFIAPI __attribute__((ms_abi))

struct EFI_GUID
{
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
};

inline constexpr EFI_STATUS EFIERR(EFI_STATUS code)
{
    return code | 0x8000000000000000ULL;
}

inline constexpr bool efi_error(EFI_STATUS status)
{
    return (status & 0x8000000000000000ULL) != 0;
}

inline constexpr EFI_STATUS EFI_SUCCESS = 0;
inline constexpr EFI_STATUS EFI_LOAD_ERROR = EFIERR(1);
inline constexpr EFI_STATUS EFI_INVALID_PARAMETER = EFIERR(2);
inline constexpr EFI_STATUS EFI_UNSUPPORTED = EFIERR(3);
inline constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL = EFIERR(5);
inline constexpr EFI_STATUS EFI_OUT_OF_RESOURCES = EFIERR(9);
inline constexpr EFI_STATUS EFI_NOT_FOUND = EFIERR(14);
inline constexpr EFI_STATUS EFI_DEVICE_ERROR = EFIERR(7);
inline constexpr EFI_STATUS EFI_ABORTED = EFIERR(21);

enum EFI_ALLOCATE_TYPE : UINT32
{
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2,
    MaxAllocateType = 3,
};

enum EFI_MEMORY_TYPE : UINT32
{
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14,
    EfiMaxMemoryType = 15,
};

struct EFI_TABLE_HEADER
{
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
};

struct EFI_MEMORY_DESCRIPTOR
{
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
using EFI_TEXT_STRING = EFI_STATUS(EFIAPI *)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
{
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

struct EFI_CONFIGURATION_TABLE
{
    EFI_GUID VendorGuid;
    void *VendorTable;
};

struct EFI_BOOT_SERVICES;

struct EFI_SYSTEM_TABLE
{
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    UINT32 Pad0;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
};

using EFI_ALLOCATE_PAGES = EFI_STATUS(EFIAPI *)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
using EFI_FREE_PAGES = EFI_STATUS(EFIAPI *)(EFI_PHYSICAL_ADDRESS, UINTN);
using EFI_GET_MEMORY_MAP = EFI_STATUS(EFIAPI *)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, UINT32 *);
using EFI_ALLOCATE_POOL = EFI_STATUS(EFIAPI *)(EFI_MEMORY_TYPE, UINTN, void **);
using EFI_FREE_POOL = EFI_STATUS(EFIAPI *)(void *);
using EFI_HANDLE_PROTOCOL = EFI_STATUS(EFIAPI *)(EFI_HANDLE, EFI_GUID *, void **);
using EFI_EXIT_BOOT_SERVICES = EFI_STATUS(EFIAPI *)(EFI_HANDLE, UINTN);
using EFI_SET_WATCHDOG_TIMER = EFI_STATUS(EFIAPI *)(UINTN, UINT64, UINTN, CHAR16 *);
using EFI_LOCATE_PROTOCOL = EFI_STATUS(EFIAPI *)(EFI_GUID *, void *, void **);

enum EFI_LOCATE_SEARCH_TYPE : UINT32 {
    AllHandles = 0,
    ByRegisterNotify = 1,
    ByProtocol = 2
};
using EFI_LOCATE_HANDLE_BUFFER = EFI_STATUS(EFIAPI *)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);

struct EFI_BOOT_SERVICES
{
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    void *GetNextMonotonicCount;
    void *Stall;
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

struct EFI_LOADED_IMAGE_PROTOCOL
{
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void *Unload;
};

struct EFI_FILE_PROTOCOL;

using EFI_FILE_OPEN = EFI_STATUS(EFIAPI *)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);
using EFI_FILE_CLOSE = EFI_STATUS(EFIAPI *)(EFI_FILE_PROTOCOL *);
using EFI_FILE_READ = EFI_STATUS(EFIAPI *)(EFI_FILE_PROTOCOL *, UINTN *, void *);
using EFI_FILE_GET_INFO = EFI_STATUS(EFIAPI *)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);

struct EFI_FILE_PROTOCOL
{
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    void *Delete;
    EFI_FILE_READ Read;
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    void *SetInfo;
    void *Flush;
    void *OpenEx;
    void *ReadEx;
    void *WriteEx;
    void *FlushEx;
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
using EFI_OPEN_VOLUME = EFI_STATUS(EFIAPI *)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    UINT64 Revision;
    EFI_OPEN_VOLUME OpenVolume;
};

inline constexpr UINT64 EFI_FILE_MODE_READ = 0x0000000000000001ULL;

struct EFI_FILE_INFO
{
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT64 CreateTime[2];
    UINT64 LastAccessTime[2];
    UINT64 ModificationTime[2];
    UINT64 Attribute;
    CHAR16 FileName[1];
};

struct EFI_PIXEL_BITMASK
{
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
};

enum EFI_GRAPHICS_PIXEL_FORMAT : UINT32
{
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4,
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION
{
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE
{
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL;
using EFI_GRAPHICS_QUERY_MODE = EFI_STATUS(EFIAPI *)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32, UINTN *,
                                                     EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
using EFI_GRAPHICS_SET_MODE = EFI_STATUS(EFIAPI *)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);

struct EFI_GRAPHICS_OUTPUT_PROTOCOL
{
    EFI_GRAPHICS_QUERY_MODE QueryMode;
    EFI_GRAPHICS_SET_MODE SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

struct EFI_EDID_ACTIVE_PROTOCOL
{
    UINT32 SizeOfEdid;
    UINT8 *Edid;
};

struct EFI_EDID_DISCOVERED_PROTOCOL
{
    UINT32 SizeOfEdid;
    UINT8 *Edid;
};

inline constexpr EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID = {
    0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
inline constexpr EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
    0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
inline constexpr EFI_GUID EFI_FILE_INFO_GUID = {
    0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
inline constexpr EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
inline constexpr EFI_GUID EFI_ACPI_TABLE_GUID = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
inline constexpr EFI_GUID EFI_ACPI_20_TABLE_GUID = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
inline constexpr EFI_GUID EFI_EDID_ACTIVE_PROTOCOL_GUID = {
    0xbd8c1056, 0x9f36, 0x44ec, {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86}};
inline constexpr EFI_GUID EFI_EDID_DISCOVERED_PROTOCOL_GUID = {
    0x1c0c34f6, 0xd380, 0x41fa, {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa}};
