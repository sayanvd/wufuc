#include "hooks.h"

#include "appverifier.h"
#include "patchwua.h"
#include "helpers.h"
#include "tracing.h"
#include "rtl_malloc.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include <phnt_windows.h>
#include <phnt.h>
#include <Psapi.h>


LSTATUS WINAPI RegQueryValueExW_Hook(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
        LSTATUS result;

        if ( (lpData && lpcbData)
                && (lpValueName && !_wcsicmp(lpValueName, L"ServiceDll")) ) {

                // store original lpData buffer size
                DWORD cbData = *lpcbData;

                // this way the dll path is guaranteed to be null-terminated
                result = RegGetValueW(hKey, NULL, lpValueName, RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, lpType, lpData, lpcbData);

                NTSTATUS Status;
                ULONG ResultLength;
                if ( result != ERROR_SUCCESS 
                        || (Status = NtQueryKey((HANDLE)hKey, KeyNameInformation, NULL, 0, &ResultLength),
                        Status != STATUS_BUFFER_OVERFLOW && Status != STATUS_BUFFER_TOO_SMALL) )
                        goto L_ret;

                PKEY_NAME_INFORMATION pkni = rtl_malloc(ResultLength);

                if ( NtQueryKey((HANDLE)hKey, KeyNameInformation, (PVOID)pkni, ResultLength, &ResultLength) != STATUS_SUCCESS )
                        goto L_free_pkni;

                size_t BufferCount = pkni->NameLength / sizeof(wchar_t);

                // change key name to lower-case because there is no case-insensitive version of _snwscanf_s
                for ( size_t i = 0; i < BufferCount; i++ )
                        pkni->Name[i] = towlower(pkni->Name[i]);

                int current, pos;
                if ( _snwscanf_s(pkni->Name, BufferCount, 
                        L"\\registry\\machine\\system\\controlset%03d\\services\\wuauserv\\parameters%n", &current, &pos) == 1 
                        && pos == BufferCount ) {

                        wchar_t drive[_MAX_DRIVE], dir[_MAX_DIR], fname[_MAX_FNAME], ext[_MAX_EXT];
                        _wsplitpath_s((wchar_t *)lpData,
                                drive, _countof(drive),
                                dir, _countof(dir),
                                fname, _countof(fname),
                                ext, _countof(ext));

                        if ( !_wcsicmp(ext, L".dll")
                                && (!_wcsicmp(fname, L"wuaueng2") // UpdatePack7R2
                                || !_wcsicmp(fname, L"WuaCpuFix64") // WuaCpuFix
                                || !_wcsicmp(fname, L"WuaCpuFix")) ) {

                                wchar_t *tmp = rtl_malloc(cbData);

                                size_t MaxCount = cbData / sizeof(wchar_t);
                                _wmakepath_s(tmp, MaxCount, drive, dir, L"wuaueng", ext);
                                DWORD nSize = ExpandEnvironmentStringsW(tmp, NULL, 0);

                                wchar_t *lpDst = rtl_calloc(nSize, sizeof(wchar_t));
                                ExpandEnvironmentStringsW(tmp, lpDst, nSize);

                                rtl_free(tmp);

                                if ( file_exists(lpDst) ) {
                                        _wmakepath_s((wchar_t *)lpData, MaxCount, drive, dir, L"wuaueng", ext);
                                        *lpcbData = (DWORD)((wcslen((wchar_t *)lpData) + 1) * sizeof(wchar_t));
                                        trace(L"Fixed wuauserv %ls path: %ls", lpValueName, lpDst);
                                }
                                rtl_free(lpDst);
                        }
                }
L_free_pkni:
                rtl_free(pkni);
        } else {
                // handle normally
                result = (*g_plpfnRegQueryValueExW)(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
        }
L_ret:
        return result;
}

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpFileName, HANDLE hFile, DWORD dwFlags)
{
        HMODULE result = (*g_plpfnLoadLibraryExW)(lpFileName, hFile, dwFlags);
        if ( !result ) {
                trace(L"Failed to load library: %ls (error code=%08X)", lpFileName, GetLastError());
                goto L_ret;
        }

        trace(L"Loaded library: %ls", lpFileName);
        DWORD dwLen = GetFileVersionInfoSizeW(lpFileName, NULL);
        if ( !dwLen )
                goto L_ret;
        
        LPVOID pBlock = rtl_malloc(dwLen);

        PLANGANDCODEPAGE ptl;
        UINT cb;
        if ( !GetFileVersionInfoW(lpFileName, 0, dwLen, pBlock)
                || !VerQueryValueW(pBlock, L"\\VarFileInfo\\Translation", (LPVOID *)&ptl, &cb) )
                goto L_free_pBlock;

        wchar_t lpSubBlock[38];
        for ( size_t i = 0; i < (cb / sizeof(LANGANDCODEPAGE)); i++ ) {
                swprintf_s(lpSubBlock, _countof(lpSubBlock),
                        L"\\StringFileInfo\\%04x%04x\\InternalName", 
                        ptl[i].wLanguage, 
                        ptl[i].wCodePage);
                
                wchar_t *lpszInternalName;
                UINT uLen;
                if ( VerQueryValueW(pBlock, lpSubBlock, (LPVOID *)&lpszInternalName, &uLen)
                        && !_wcsicmp(lpszInternalName, L"wuaueng.dll") ) {

                        VS_FIXEDFILEINFO *pffi;
                        VerQueryValueW(pBlock, L"\\", (LPVOID *)&pffi, &uLen);
                        WORD wMajor = HIWORD(pffi->dwProductVersionMS);
                        WORD wMinor = LOWORD(pffi->dwProductVersionMS);
                        WORD wBuild = HIWORD(pffi->dwProductVersionLS);
                        WORD wRevision = LOWORD(pffi->dwProductVersionLS);

                        wchar_t path[MAX_PATH];
                        GetModuleFileNameW(result, path, _countof(path));
                        wchar_t *fname = find_fname(path);

                        if ( (verify_win7() && compare_versions(wMajor, wMinor, wBuild, wRevision, 7, 6, 7601, 23714) != -1)
                                || (verify_win81() && compare_versions(wMajor, wMinor, wBuild, wRevision, 7, 9, 9600, 18621) != -1) ) {

                                trace(L"%ls version: %d.%d.%d.%d", fname, wMajor, wMinor, wBuild, wRevision);
                                MODULEINFO modinfo;
                                if ( GetModuleInformation(GetCurrentProcess(), result, &modinfo, sizeof(MODULEINFO)) ) {
                                        if ( !patch_wua(modinfo.lpBaseOfDll, modinfo.SizeOfImage, fname) )
                                                trace(L"Failed to patch %ls!", fname);
                                } else trace(L"Failed to get module information for %ls (%p) (couldn't patch)", fname, result);
                        } else trace(L"Unsupported %ls version: %d.%d.%d.%d (patching skipped)", fname, wMajor, wMinor, wBuild, wRevision);
                        break;
                }
        }
L_free_pBlock:
        rtl_free(pBlock);
L_ret:
        return result;
}
