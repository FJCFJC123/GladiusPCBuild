"""Library functions replaced by gladiusport.dll, and in-place byte patches, applied by xbe2pe.py.

REPLACE entries: (xbe_address, export_name, stack_arg_bytes, note). xbe2pe.py writes a jmp to the DLL export at the
address; the export has the same calling convention and Win32 semantics.
"""
REPLACE = [
    # --- heap (CRT malloc/free/realloc/_msize land here; Xbox XAPI carries its own NT Rtl heap)
    (0x16BC09, 'xapi_HeapCreate',        12, 'flOptions, dwInitialSize, dwMaximumSize -> RtlCreateHeap'),
    (0x16BC58, 'xapi_HeapDestroy',        4, 'hHeap -> RtlDestroyHeap'),
    (0x16BC74, 'xapi_HeapFree',          12, 'hHeap, dwFlags, lpMem -> RtlFreeHeap'),
    (0x16BC8B, 'xapi_GetProcessHeap',     0, 'returns [0x597204] XapiProcessHeap'),
    (0x16D4DC, 'xapi_RtlAllocateHeap',   12, 'called directly by CRT _heap_alloc 0x2751B1'),
    (0x16E4D1, 'xapi_RtlFreeHeap',       12, 'BOOLEAN'),
    (0x16CB3A, 'xapi_RtlSizeHeap',       12, 'called by CRT _msize / XAPI HeapSize 0x16BCDA'),
    (0x16E888, 'xapi_RtlReAllocateHeap', 16, 'called by CRT realloc 0x2752EC'),

    # --- threads, synchronization, last error, time
    (0x16893B, 'xapi_CreateEventA',             16, 'XbSymbolDatabase'),
    (0x16899C, 'xapi_OpenEventA',               12, 'XbSymbolDatabase'),
    (0x1689F8, 'xapi_SetEvent',                  4, 'XbSymbolDatabase'),
    (0x168A18, 'xapi_ResetEvent',                4, 'XbSymbolDatabase'),
    (0x168A36, 'xapi_PulseEvent',                4, 'XbSymbolDatabase'),
    (0x168A56, 'xapi_CreateSemaphoreA',         16, 'NtCreateSemaphore'),
    (0x168B0D, 'xapi_ReleaseSemaphore',         12, 'NtReleaseSemaphore'),
    (0x168B33, 'xapi_CreateMutexA',             12, 'XbSymbolDatabase'),
    (0x168BE7, 'xapi_ReleaseMutex',              4, 'NtReleaseMutant'),
    (0x168C07, 'xapi_WaitForSingleObjectEx',    12, 'NtWaitForSingleObjectEx'),
    (0x168C4B, 'xapi_SignalObjectAndWait',      16, 'XbSymbolDatabase'),
    (0x168C92, 'xapi_WaitForMultipleObjectsEx', 20, 'NtWaitForMultipleObjectsEx'),
    (0x168D12, 'xapi_SleepEx',                   8, 'KeDelayExecutionThread'),
    (0x168E84, 'xapi_WaitForSingleObject',       8, '-> 0x168C07'),
    (0x168EB0, 'xapi_Sleep',                     4, '-> 0x168D12'),
    (0x168FF8, 'xapi_GetLastError',              0, 'XbSymbolDatabase (Xbox TLS +8)'),
    (0x169020, 'xapi_SetLastError',              4, 'XbSymbolDatabase'),
    (0x16904E, 'xapi_XapiSetLastNTError',        4, 'RtlNtStatusToDosError + SetLastError'),
    (0x169191, 'xapi_GetOverlappedResult',      16, 'XbSymbolDatabase'),
    (0x169D96, 'xapi_CloseHandle',               4, 'NtClose'),
    (0x169E12, 'xapi_SetThreadPriority',         8, 'XbSymbolDatabase'),
    (0x169E64, 'xapi_GetThreadPriority',         4, 'XbSymbolDatabase'),
    (0x169EB8, 'xapi_SetThreadPriorityBoost',    8, 'XbSymbolDatabase'),
    (0x169F3F, 'xapi_SuspendThread',             4, 'NtSuspendThread'),
    (0x169F65, 'xapi_ResumeThread',              4, 'NtResumeThread'),
    (0x169F8B, 'xapi_RaiseException',           16, 'XbSymbolDatabase'),
    (0x169FEC, 'xapi_QueueUserAPC',             12, 'XbSymbolDatabase'),
    (0x16A011, 'xapi_SwitchToThread',            0, 'XbSymbolDatabase'),
    (0x16A024, 'xapi_ExitThread',                4, 'XbSymbolDatabase'),
    (0x16A036, 'xapi_GetExitCodeThread',         8, 'XbSymbolDatabase'),
    (0x16A2B1, 'xapi_CreateThread',             24, 'XbSymbolDatabase; stack 0 -> XBE default [0x10130]'),
    (0x16A31F, 'xapi_GetCurrentThreadId',        0, 'KTHREAD+0x12C'),
    (0x2840A3, 'xapi_GetTickCount',              0, 'KeTickCount'),
    (0x284653, 'xapi_QueryPerformanceCounter',   4, 'Xbox: rdtsc'),
    (0x284664, 'xapi_QueryPerformanceFrequency', 4, 'Xbox: constant 733333333'),
    (0x2847D7, 'xapi_OutputDebugStringA',        4, 'XbSymbolDatabase'),

    # --- dashboard settings (ExQueryNonVolatileSetting readers) -> PC defaults
    (0x1714E4, 'xapi_XQueryValue',              20, 'index, type*, value*, size, result_len*'),
    (0x16B0A6, 'xapi_XGetParentalControlSetting', 0, 'setting 0xA'),
    (0x284930, 'xapi_XGetAVPack',                0, 'setting 0x103 byte 1'),
    (0x284959, 'xapi_XGetVideoFlags',            0, 'setting 8'),
    (0x2849AA, 'xapi_XGetAudioFlags',            0, 'setting 9'),

    # --- memory: physical/contiguous (GPU-visible on Xbox) and virtual
    (0x1720A1, 'xapi_XPhysicalAlloc',           16, 'size, max_address, alignment, protect'),
    (0x1720E4, 'xapi_XPhysicalAllocEx',         20, 'size, lowest, highest, alignment, protect'),
    (0x172111, 'xapi_XPhysicalSize',             4, 'jmp MmQueryAllocationSize (6-byte stub)'),
    (0x172117, 'xapi_XPhysicalProtect',         12, 'MmSetAddressProtect'),
    (0x172127, 'xapi_XPhysicalFree',             4, 'jmp MmFreeContiguousMemory (6-byte stub)'),
    (0x284678, 'xapi_VirtualAlloc',             16, 'NtAllocateVirtualMemory'),
    (0x2846A6, 'xapi_VirtualFree',              12, 'NtFreeVirtualMemory'),
    (0x2846E2, 'xapi_VirtualProtect',           16, 'NtProtectVirtualMemory'),
    (0x28470E, 'xapi_VirtualQuery',             12, 'NtQueryVirtualMemory'),
    (0x284788, 'xapi_GlobalMemoryStatus',        4, 'MmQueryStatistics'),

    # --- file I/O (Xbox drive letters -> PC folders, gladiusport.ini)
    (0x16FD36, 'xapi_CreateFileA',               28, 'NtCreateFile'),
    (0x16F2D2, 'xapi_ReadFile',                  20, 'NtReadFile'),
    (0x16F3BF, 'xapi_WriteFile',                 20, 'NtWriteFile'),
    (0x16F8F1, 'xapi_ReadFileEx',                20, 'NtReadFile + APC'),
    (0x16F942, 'xapi_WriteFileEx',               20, 'NtWriteFile + APC'),
    (0x16F495, 'xapi_SetEndOfFile',               4, 'NtSetInformationFile'),
    (0x16F519, 'xapi_SetFilePointer',            16, 'NtSetInformationFile'),
    (0x16F613, 'xapi_SetFilePointerEx',          20, 'LARGE_INTEGER by value'),
    (0x16F7B4, 'xapi_GetFileTime',               16, 'NtQueryInformationFile'),
    (0x16F88C, 'xapi_FlushFileBuffers',           4, 'NtFlushBuffersFile'),
    (0x16FCFB, 'xapi_GetFileSize',                8, '-> 0x16F8B3'),
    (0x16F8B3, 'xapi_GetFileSizeEx',              8, 'NtQueryInformationFile'),
    (0x171EC1, 'xapi_GetFileAttributesA',         4, 'NtQueryFullAttributesFile'),
    (0x171ABF, 'xapi_SetFileAttributesA',         8, 'NtSetInformationFile'),
    (0x171CC8, 'xapi_DeleteFileA',                4, 'NtOpenFile non-dir, DELETE'),
    (0x285169, 'xapi_RemoveDirectoryA',           4, 'NtOpenFile dir, DELETE'),
    (0x284EA3, 'xapi_CreateDirectoryA',           8, 'NtCreateFile dir'),
    (0x171C32, 'xapi_MoveFileA',                  8, 'XbSymbolDatabase'),
    (0x284F9F, 'xapi_FindFirstFileA',             8, 'NtQueryDirectoryFile'),
    (0x2850AC, 'xapi_FindNextFileA',              8, 'NtQueryDirectoryFile'),
    (0x16B0CB, 'xapi_GetDiskFreeSpaceExA',       16, 'NtQueryVolumeInformationFile'),
    (0x16B1BF, 'xapi_GetVolumeInformationA',     32, 'NtQueryVolumeInformationFile, 8 args'),

    # --- save-game plumbing (saves themselves are files on U:)
    (0x16B9F4, 'xapi_XGetDiskClusterSize',        4, 'NtQueryVolumeInformationFile: SectorsPerUnit*BytesPerSector'),
    (0x16BA93, 'xapi_XGetDiskSectorSize',         4, 'NtQueryVolumeInformationFile: BytesPerSector'),
    (0x17182C, 'xapi_XCalculateSignatureBegin',   4, 'XcHMAC via kernel pointers'),
    (0x171840, 'xapi_XCalculateSignatureUpdate', 12, 'XcSHAUpdate via [0x418B9C]'),
    (0x17188E, 'xapi_XCalculateSignatureEnd',     8, 'XcSHAFinal via [0x418BA0]'),
    (0x16BB2E, 'xapi_XMUNameFromDriveLetter',    12, 'NtFsControlFile'),
    (0x334566, 'xapi_XMountMUA',                 12, 'IoDismountVolumeByName / symbolic links'),
    (0x334804, 'xapi_XUnmountMU',                 8, 'IoDeleteSymbolicLink'),
    (0x16B641, 'xapi_XMountUtilityDrive',         4, 'IoCreateSymbolicLink'),
    (0x285645, 'xapi_XCreateSaveGame',           24, 'writes SaveMeta.xbx via NtCreateFile'),
    (0x2850FD, 'xapi_XapiCreateSaveDirectory',    8, 'NtCreateFile FILE_CREATE dir (XCreateSaveGame)'),
    (0x2855E9, 'xapi_XapiTouchSaveDirectory',     4, 'NtSetInformationFile LastWriteTime = KeQuerySystemTime'),
    (0x171A50, 'xapi_XapiDeleteDirectoryContents', 8, 'NtOpenFile + recursive NtQueryDirectoryFile/delete (XDeleteSaveGame)'),

    # --- XBE sections (always mapped in the converted exe)
    (0x16A32C, 'xapi_XGetSectionHandleA',         4, 'walks XBE section headers'),
    (0x16A393, 'xapi_XLoadSectionByHandle',       4, 'XeLoadSection'),
    (0x16A3B4, 'xapi_XFreeSectionByHandle',       4, 'XeUnloadSection'),

    # --- input (XPP USB stack -> Windows XInput + keyboard)
    (0x3331B7, 'xapi_XInitDevices',               8, 'XbSymbolDatabase (0x334000 is a jmp to it)'),
    (0x334005, 'xapi_XGetDevices',                4, 'XbSymbolDatabase'),
    (0x334027, 'xapi_XGetDeviceChanges',         12, 'XbSymbolDatabase'),
    (0x333C69, 'xapi_XInputOpen',                16, 'XbSymbolDatabase'),
    (0x333CBF, 'xapi_XInputClose',                4, 'XbSymbolDatabase'),
    (0x333CCB, 'xapi_XInputGetCapabilities',      8, 'XbSymbolDatabase'),
    (0x333EA3, 'xapi_XInputGetState',             8, 'XbSymbolDatabase'),
    (0x333F16, 'xapi_XInputSetState',             8, 'XbSymbolDatabase'),

    # --- CRI middleware lock: priority-boost "lock" -> real recursive mutex (multi-core safe)
    (0x22C7E0, 'cri_Lock',                        0, 'call [0x567AA0] = SetThreadPriority(self, 15)'),
    (0x22C800, 'cri_Unlock',                      0, 'call [0x567AA8] = restore saved priority'),

    # --- XGRAPHICS shader assembler: record the engine's generated shader source, return tagged placeholders
    (0x2F0DE3, 'xg_XGAssembleShader',            44, 'XGAssembleShader(name, src, len, flags, 7 more)'),

    # --- diagnostics
    (0x1D5D10, 'port_engine_halt',                0, 'engine halt `jmp $` (CRI error callback): logs the reason'),
]

# In-place byte patches: (xbe_address, expected_hex, new_hex, note). 'FAKE_KTHREAD' in new_hex becomes the address of
# a .port block standing in for the Xbox KTHREAD (+0x28 TlsData != 0).
FS_TLS_ARRAY = [0x277883, 0x2779CD, 0x277A2A, 0x282034, 0x285B73, 0x285B90]

PATCHES = (
    [(a, '648b0d04000000', '648b0d2c000000', 'CRT: Xbox TLS array fs:[4] -> Windows fs:[0x2C]') for a in FS_TLS_ARRAY] +
    [(0x285BEB, '648b3d04000000', '648b3d2c000000', 'XAPI TLS helper: fs:[4] -> fs:[0x2C]')] +
    [(a, '640fb60524000000', '31c0909090909090', 'CRT: KeGetCurrentIrql -> 0 (PASSIVE_LEVEL)')
     for a in (0x27782C, 0x27785C, 0x2779A7, 0x285DDA)] +
    [(a, '64a128000000', 'b8FAKE_KTHREAD90', 'CRT _getptd: KeGetCurrentThread()->TlsData check')
     for a in (0x277872, 0x2779BC)] +
    # save icon (dashboard thumbnail) needs D3DX on real D3D state: skipped (returns 0 = no image, allowed on Xbox too)
    [(0x272C60, '81ec24010000', '31c0c20800' + '90', 'save icon writer -> return 0 (no image)')] +
    # CRI idle-time meter: a spinning priority-2 thread that would hold the CRI lock forever on PC. Keep the nesting
    # count, skip the rest.
    [(0x211470, 'a13092520085c0', 'ff053092520' + '0c3', 'CRI idle meter begin -> inc nesting; ret'),
     (0x2114B0, 'ff0d3092520075', 'ff0d3092520' + '0c3', 'CRI idle meter end -> dec nesting; ret')]
)

# --- Xbox D3D8 -> D3D11 shim (tools/d3d_table.py)
from d3d_table import entries as _d3d_entries, dsound_entries as _dsound_entries
D3D_FASTCALL = {}                    # export name -> decorated arg bytes (MSVC @name@N)
for _a, _n, _conv, _ns, _regs in _d3d_entries():
    REPLACE.append((_a, 'd3d_' + _n, 4 * _ns, _conv))
    if _conv == 'fastcall':
        D3D_FASTCALL['d3d_' + _n] = 8 + 4 * _ns

# --- Xbox DirectSound -> software mixer (src/dsound_*.cpp)
for _a, _n, _conv, _ns, _regs in _dsound_entries():
    REPLACE.append((_a, 'dsound_' + _n, 4 * _ns, _conv))
