#pragma once

// Subset of the Win32 API used by the shared Toolscreen sources, implemented on
// top of macOS/GLFW. Behaviour follows the Windows documentation closely enough
// for Toolscreen's use; it is not a general-purpose Win32 layer.
//
// HWND values are the GLFW window pointers of the game window. Window messages
// are delivered by the macOS GLFW bridge on the game's main thread.

#include "platform/macos/win32/win32_types.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// Compiler / CRT compatibility
// ---------------------------------------------------------------------------
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#define _ReturnAddress() __builtin_return_address(0)
#define __debugbreak() __builtin_trap()
#define __try if (true)
#define __except(filter) else if ((void)(filter), false)
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define _TRUNCATE (static_cast<size_t>(-1))
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _strdup strdup
#define _wcsdup wcsdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _snwprintf swprintf
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#ifndef _MAX_PATH
#define _MAX_PATH 260
#endif

typedef int errno_t;

struct _EXCEPTION_RECORD;
struct _CONTEXT;
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct _CONTEXT {
    DWORD64 Rip;
    DWORD64 Rsp;
    DWORD64 Rbp;
    DWORD64 Pc;
    DWORD64 Sp;
    DWORD64 Fp;
} CONTEXT, *PCONTEXT;
typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;
typedef LONG(WINAPI* LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
typedef LPTOP_LEVEL_EXCEPTION_FILTER PTOP_LEVEL_EXCEPTION_FILTER;
typedef void (*_se_translator_function)(unsigned int, EXCEPTION_POINTERS*);
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_BREAKPOINT 0x80000003u

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_FREE 0x00010000
#define MEM_IMAGE 0x01000000
#define MEM_PRIVATE 0x00020000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100

typedef struct _MODULEINFO {
    LPVOID lpBaseOfDll;
    DWORD SizeOfImage;
    LPVOID EntryPoint;
} MODULEINFO, *LPMODULEINFO;

#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE 0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define LANG_NEUTRAL 0x00
#define LANG_ENGLISH 0x09
#define SUBLANG_DEFAULT 0x01
#define SUBLANG_ENGLISH_US 0x01
#define SORT_DEFAULT 0x0
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define PRIMARYLANGID(lgid) ((WORD)(lgid) & 0x3ff)
#define SUBLANGID(lgid) ((WORD)(lgid) >> 10)
#define MAKELCID(lgid, srtid) ((DWORD)((((DWORD)((WORD)(srtid))) << 16) | ((DWORD)((WORD)(lgid)))))
#define LOCALE_IDEFAULTANSICODEPAGE 0x00001004
#define LOCALE_RETURN_NUMBER 0x20000000
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_CHINESE 0x04

#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x1F0003
#define EVENT_ALL_ACCESS 0x1F0003
#define SYNCHRONIZE 0x00100000L
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_VM_READ 0x0010
#define PROCESS_TERMINATE 0x0001
#define THREAD_ALL_ACCESS 0x1FFFFF
#define STILL_ACTIVE 259

// ---------------------------------------------------------------------------
// user32 / gdi32 structures
// ---------------------------------------------------------------------------
typedef struct tagWNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXW;
typedef WNDCLASSEXW WNDCLASSEX;

typedef struct tagTRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
} TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;
#define TME_HOVER 0x00000001
#define TME_LEAVE 0x00000002
#define TME_NONCLIENT 0x00000010
#define TME_QUERY 0x40000000
#define TME_CANCEL 0x80000000
#define HOVER_DEFAULT 0xFFFFFFFF

typedef struct _ICONINFO {
    BOOL fIcon;
    DWORD xHotspot;
    DWORD yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
} ICONINFO, *PICONINFO;

typedef struct _ICONINFOEXW {
    DWORD cbSize;
    BOOL fIcon;
    DWORD xHotspot;
    DWORD yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
    WORD wResID;
    WCHAR szModName[MAX_PATH];
    WCHAR szResName[MAX_PATH];
} ICONINFOEXW, *PICONINFOEXW;

typedef struct tagBITMAP {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    WORD bmPlanes;
    WORD bmBitsPixel;
    LPVOID bmBits;
} BITMAP, *PBITMAP, *LPBITMAP;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagRGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} RGBQUAD;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

#define BI_RGB 0L
#define BI_BITFIELDS 3L
#define DIB_RGB_COLORS 0
#define SRCCOPY 0x00CC0020
#define CAPTUREBLT 0x40000000
#define R2_COPYPEN 13
#define R2_NOT 6
#define R2_XORPEN 7
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define WHITE_BRUSH 0
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define PW_CLIENTONLY 0x00000001
#define PW_RENDERFULLCONTENT 0x00000002

#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define LR_DEFAULTCOLOR 0x00000000
#define LR_LOADFROMFILE 0x00000010
#define LR_DEFAULTSIZE 0x00000040
#define LR_SHARED 0x00008000
#define LR_COPYFROMRESOURCE 0x00004000
#define LR_CREATEDIBSECTION 0x00002000
#define LR_COPYRETURNORG 0x00000004

#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define IDC_IBEAM MAKEINTRESOURCEW(32513)
#define IDC_WAIT MAKEINTRESOURCEW(32514)
#define IDC_CROSS MAKEINTRESOURCEW(32515)
#define IDC_UPARROW MAKEINTRESOURCEW(32516)
#define IDC_SIZENWSE MAKEINTRESOURCEW(32642)
#define IDC_SIZENESW MAKEINTRESOURCEW(32643)
#define IDC_SIZEWE MAKEINTRESOURCEW(32644)
#define IDC_SIZENS MAKEINTRESOURCEW(32645)
#define IDC_SIZEALL MAKEINTRESOURCEW(32646)
#define IDC_NO MAKEINTRESOURCEW(32648)
#define IDC_HAND MAKEINTRESOURCEW(32649)
#define IDC_APPSTARTING MAKEINTRESOURCEW(32650)
#define IDC_HELP MAKEINTRESOURCEW(32651)
#define OCR_NORMAL 32512
#define OCR_IBEAM 32513
#define OCR_HAND 32649

#define HTCLIENT 1
#define HTNOWHERE 0

#define CF_TEXT 1
#define CF_UNICODETEXT 13
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)

#define RDW_INVALIDATE 0x0001
#define RDW_ERASE 0x0004
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define RDW_FRAME 0x0400

#define SMTO_NORMAL 0x0000
#define SMTO_BLOCK 0x0001
#define SMTO_ABORTIFHUNG 0x0002

#define SW_SHOWNORMAL_EX SW_SHOWNORMAL
#define SE_ERR_NOASSOC 31

typedef UINT_PTR(CALLBACK* LPOFNHOOKPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct tagOFNW {
    DWORD lStructSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCWSTR lpstrFilter;
    LPWSTR lpstrCustomFilter;
    DWORD nMaxCustFilter;
    DWORD nFilterIndex;
    LPWSTR lpstrFile;
    DWORD nMaxFile;
    LPWSTR lpstrFileTitle;
    DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir;
    LPCWSTR lpstrTitle;
    DWORD Flags;
    WORD nFileOffset;
    WORD nFileExtension;
    LPCWSTR lpstrDefExt;
    LPARAM lCustData;
    LPOFNHOOKPROC lpfnHook;
    LPCWSTR lpTemplateName;
    void* pvReserved;
    DWORD dwReserved;
    DWORD FlagsEx;
} OPENFILENAMEW, *LPOPENFILENAMEW;
typedef OPENFILENAMEW OPENFILENAME;
#define OFN_READONLY 0x00000001
#define OFN_OVERWRITEPROMPT 0x00000002
#define OFN_HIDEREADONLY 0x00000004
#define OFN_NOCHANGEDIR 0x00000008
#define OFN_PATHMUSTEXIST 0x00000800
#define OFN_FILEMUSTEXIST 0x00001000
#define OFN_EXPLORER 0x00080000
#define OFN_DONTADDTORECENT 0x02000000
#define OFN_ENABLESIZING 0x00800000

#define CSIDL_APPDATA 0x001a
#define CSIDL_LOCAL_APPDATA 0x001c
#define CSIDL_PROFILE 0x0028
#define CSIDL_PERSONAL 0x0005
#define CSIDL_DESKTOPDIRECTORY 0x0010
#define CSIDL_FONTS 0x0014
#define CSIDL_FLAG_CREATE 0x8000
#define SHGFP_TYPE_CURRENT 0
#define KF_FLAG_DEFAULT 0x00000000
#define KF_FLAG_CREATE 0x00008000

// Known folders are identified by their Data1 value only.
extern const GUID FOLDERID_Downloads;
extern const GUID FOLDERID_Documents;
extern const GUID FOLDERID_Desktop;
extern const GUID FOLDERID_Pictures;
extern const GUID FOLDERID_RoamingAppData;
extern const GUID FOLDERID_LocalAppData;
extern const GUID FOLDERID_Profile;
extern const GUID FOLDERID_Fonts;

#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define COINIT_DISABLE_OLE1DDE 0x4

// DWM
typedef struct _DWM_BLURBEHIND {
    DWORD dwFlags;
    BOOL fEnable;
    HRGN hRgnBlur;
    BOOL fTransitionOnMaximized;
} DWM_BLURBEHIND;
#define DWM_BB_ENABLE 0x00000001
#define DWM_BB_BLURREGION 0x00000002
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#define DWMWA_CLOAKED 14

// DPI awareness (only referenced through GetProcAddress in imgui_impl_win32)
#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__* name
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#define _DPI_AWARENESS_CONTEXTS_
#define DPI_AWARENESS_CONTEXT_UNAWARE (reinterpret_cast<DPI_AWARENESS_CONTEXT>(-1))
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE (reinterpret_cast<DPI_AWARENESS_CONTEXT>(-2))
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE (reinterpret_cast<DPI_AWARENESS_CONTEXT>(-3))
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4))
#define USER_DEFAULT_SCREEN_DPI 96

typedef struct _OSVERSIONINFOEXW {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXW, *POSVERSIONINFOEXW, *LPOSVERSIONINFOEXW, RTL_OSVERSIONINFOEXW, *PRTL_OSVERSIONINFOEXW;
#define VER_MINORVERSION 0x0000001
#define VER_MAJORVERSION 0x0000002
#define VER_BUILDNUMBER 0x0000004
#define VER_SERVICEPACKMAJOR 0x0000020
#define VER_GREATER_EQUAL 3
#define VER_SET_CONDITION(_m_, _t_, _c_) ((void)((_m_) = VerSetConditionMask((_m_), (_t_), (_c_))))
#define _WIN32_WINNT_WIN8 0x0602
#define _WIN32_WINNT_WINBLUE 0x0603
#define _WIN32_WINNT_WIN10 0x0A00
#define _WIN32_WINNT_VISTA 0x0600
#define _WIN32_WINNT_WIN7 0x0601
#define HIBYTE_WORD(x) HIBYTE(x)
#define LOBYTE_WORD(x) LOBYTE(x)

// IME (imm.h)
#define GCS_COMPSTR 0x0008
#define GCS_RESULTSTR 0x0800
#define CFS_FORCE_POSITION 0x0020
#define CFS_CANDIDATEPOS 0x0040
#define CFS_EXCLUDE 0x0080

// WinHTTP
typedef WORD INTERNET_PORT;
typedef struct _URL_COMPONENTS {
    DWORD dwStructSize;
    LPWSTR lpszScheme;
    DWORD dwSchemeLength;
    int nScheme;
    LPWSTR lpszHostName;
    DWORD dwHostNameLength;
    INTERNET_PORT nPort;
    LPWSTR lpszUserName;
    DWORD dwUserNameLength;
    LPWSTR lpszPassword;
    DWORD dwPasswordLength;
    LPWSTR lpszUrlPath;
    DWORD dwUrlPathLength;
    LPWSTR lpszExtraInfo;
    DWORD dwExtraInfoLength;
} URL_COMPONENTS, *LPURL_COMPONENTS;
typedef URL_COMPONENTS URL_COMPONENTSW;
#define INTERNET_SCHEME_HTTP 1
#define INTERNET_SCHEME_HTTPS 2
#define INTERNET_DEFAULT_PORT 0
#define INTERNET_DEFAULT_HTTP_PORT 80
#define INTERNET_DEFAULT_HTTPS_PORT 443
#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
#define WINHTTP_ACCESS_TYPE_NO_PROXY 1
#define WINHTTP_ACCESS_TYPE_NAMED_PROXY 3
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#define WINHTTP_NO_PROXY_NAME nullptr
#define WINHTTP_NO_PROXY_BYPASS nullptr
#define WINHTTP_NO_REFERER nullptr
#define WINHTTP_DEFAULT_ACCEPT_TYPES nullptr
#define WINHTTP_NO_ADDITIONAL_HEADERS nullptr
#define WINHTTP_NO_REQUEST_DATA nullptr
#define WINHTTP_HEADER_NAME_BY_INDEX nullptr
#define WINHTTP_NO_OUTPUT_BUFFER nullptr
#define WINHTTP_NO_HEADER_INDEX nullptr
#define WINHTTP_FLAG_SECURE 0x00800000
#define WINHTTP_FLAG_REFRESH 0x00000100
#define WINHTTP_QUERY_STATUS_CODE 19
#define WINHTTP_QUERY_CONTENT_LENGTH 5
#define WINHTTP_QUERY_CONTENT_TYPE 1
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000
#define WINHTTP_ADDREQ_FLAG_ADD 0x20000000
#define WINHTTP_ADDREQ_FLAG_REPLACE 0x80000000
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#define WINHTTP_OPTION_SECURE_PROTOCOLS 84
#define WINHTTP_OPTION_DECOMPRESSION 118
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#define ICU_ESCAPE 0x80000000
#define ICU_DECODE 0x10000000

// DbgHelp
#define MAX_SYM_NAME 2000
#define SYMOPT_UNDNAME 0x00000002
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMOPT_LOAD_LINES 0x00000010
#define SYMOPT_FAIL_CRITICAL_ERRORS 0x00000200
typedef uint64_t ULONG64;
typedef DWORD64* PDWORD64;
typedef const char* PCSTR;
typedef struct _SYMBOL_INFO {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    CHAR Name[1];
} SYMBOL_INFO, *PSYMBOL_INFO;
typedef char* PCHAR;
typedef struct _IMAGEHLP_LINE64 {
    DWORD SizeOfStruct;
    PVOID Key;
    DWORD LineNumber;
    PCHAR FileName;
    DWORD64 Address;
} IMAGEHLP_LINE64, *PIMAGEHLP_LINE64;

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------
extern "C" {

// --- errors ---------------------------------------------------------------
DWORD GetLastError(void);
void SetLastError(DWORD code);
DWORD FormatMessageA(DWORD flags, LPCVOID source, DWORD messageId, DWORD languageId, LPSTR buffer, DWORD size, va_list* args);
DWORD FormatMessageW(DWORD flags, LPCVOID source, DWORD messageId, DWORD languageId, LPWSTR buffer, DWORD size, va_list* args);
HANDLE LocalFree(HANDLE mem);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
_se_translator_function _set_se_translator(_se_translator_function fn);
void OutputDebugStringA(LPCSTR text);
void OutputDebugStringW(LPCWSTR text);
BOOL IsDebuggerPresent(void);
USHORT RtlCaptureStackBackTrace(ULONG framesToSkip, ULONG framesToCapture, PVOID* backTrace, PDWORD backTraceHash);
#define CaptureStackBackTrace RtlCaptureStackBackTrace
PVOID RtlPcToFileHeader(PVOID pcValue, PVOID* baseOfImage);
ULONGLONG VerSetConditionMask(ULONGLONG conditionMask, DWORD typeMask, BYTE condition);

// --- time ------------------------------------------------------------------
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER* count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency);
void Sleep(DWORD milliseconds);
DWORD SleepEx(DWORD milliseconds, BOOL alertable);
void GetLocalTime(LPSYSTEMTIME st);
void GetSystemTime(LPSYSTEMTIME st);
void GetSystemTimeAsFileTime(LPFILETIME ft);
BOOL FileTimeToLocalFileTime(const FILETIME* fileTime, LPFILETIME localFileTime);
BOOL FileTimeToSystemTime(const FILETIME* fileTime, LPSYSTEMTIME systemTime);
BOOL SystemTimeToFileTime(const SYSTEMTIME* systemTime, LPFILETIME fileTime);
LONG CompareFileTime(const FILETIME* a, const FILETIME* b);

// --- processes / threads / sync ---------------------------------------------
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentProcess(void);
HANDLE GetCurrentThread(void);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD processId);
BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exitTime, LPFILETIME kernel, LPFILETIME user);
BOOL GetExitCodeProcess(HANDLE process, LPDWORD exitCode);
BOOL TerminateProcess(HANDLE process, UINT exitCode);
BOOL QueryFullProcessImageNameA(HANDLE process, DWORD flags, LPSTR name, PDWORD size);
BOOL QueryFullProcessImageNameW(HANDLE process, DWORD flags, LPWSTR name, PDWORD size);
void ExitProcess(UINT exitCode);
BOOL CloseHandle(HANDLE handle);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES attrs, SIZE_T stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
                    LPDWORD threadId);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
HRESULT SetThreadDescription(HANDLE thread, PCWSTR description);
BOOL DisableThreadLibraryCalls(HMODULE module);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attrs, BOOL manualReset, BOOL initialState, LPCWSTR name);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES attrs, BOOL manualReset, BOOL initialState, LPCSTR name);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES attrs, BOOL manualReset, LPCWSTR name);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attrs, LPCWSTR name, DWORD flags, DWORD access);
typedef void(CALLBACK* PTIMERAPCROUTINE)(LPVOID, DWORD, DWORD);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* dueTime, LONG period, PTIMERAPCROUTINE completion, LPVOID arg, BOOL resume);
BOOL CancelWaitableTimer(HANDLE timer);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds);
void MemoryBarrier(void);
void YieldProcessor(void);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size);
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size);
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);
LPWSTR GetCommandLineW(void);
LPSTR GetCommandLineA(void);

// --- memory ------------------------------------------------------------------
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length);
HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes);
LPVOID GlobalLock(HGLOBAL mem);
BOOL GlobalUnlock(HGLOBAL mem);
HGLOBAL GlobalFree(HGLOBAL mem);
SIZE_T GlobalSize(HGLOBAL mem);

// --- modules / resources -----------------------------------------------------
HMODULE GetModuleHandleW(LPCWSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size);
DWORD GetModuleFileNameA(HMODULE module, LPSTR filename, DWORD size);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW(LPCWSTR name);
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
BOOL FreeLibrary(HMODULE module);
BOOL GetModuleInformation(HANDLE process, HMODULE module, LPMODULEINFO info, DWORD size);
BOOL EnumProcessModules(HANDLE process, HMODULE* modules, DWORD size, LPDWORD needed);
HRSRC FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type);
HRSRC FindResourceA(HMODULE module, LPCSTR name, LPCSTR type);
HGLOBAL LoadResource(HMODULE module, HRSRC resource);
LPVOID LockResource(HGLOBAL resData);
DWORD SizeofResource(HMODULE module, HRSRC resource);
BOOL FreeResource(HGLOBAL resData);
DWORD GetFileVersionInfoSizeW(LPCWSTR filename, LPDWORD handle);
BOOL GetFileVersionInfoW(LPCWSTR filename, DWORD handle, DWORD len, LPVOID data);
BOOL VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT len);

// --- strings / locale --------------------------------------------------------
int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR multiByte, int multiByteCount, LPWSTR wide, int wideCount);
int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR wide, int wideCount, LPSTR multiByte, int multiByteCount,
                        LPCSTR defaultChar, LPBOOL usedDefaultChar);
BOOL IsDBCSLeadByte(BYTE testChar);
int GetLocaleInfoA(LCID locale, DWORD type, LPSTR data, int dataSize);
LCID GetUserDefaultLCID(void);
LANGID GetUserDefaultUILanguage(void);

// --- files -------------------------------------------------------------------
HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                   HANDLE templateFile);
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                   HANDLE templateFile);
BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD toRead, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD toWrite, LPDWORD written, LPOVERLAPPED overlapped);
BOOL FlushFileBuffers(HANDLE file);
BOOL SetEndOfFile(HANDLE file);
DWORD SetFilePointer(HANDLE file, LONG distance, PLONG distanceHigh, DWORD method);
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER newPointer, DWORD method);
DWORD GetFileSize(HANDLE file, LPDWORD high);
BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size);
BOOL GetFileTime(HANDLE file, LPFILETIME creation, LPFILETIME access, LPFILETIME write);
DWORD GetFileAttributesW(LPCWSTR name);
DWORD GetFileAttributesA(LPCSTR name);
BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL SetFileAttributesW(LPCWSTR name, DWORD attributes);
BOOL DeleteFileW(LPCWSTR name);
BOOL DeleteFileA(LPCSTR name);
BOOL MoveFileW(LPCWSTR from, LPCWSTR to);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL RemoveDirectoryW(LPCWSTR path);
DWORD GetTempPathW(DWORD size, LPWSTR buffer);
DWORD GetTempPathA(DWORD size, LPSTR buffer);
DWORD GetCurrentDirectoryW(DWORD size, LPWSTR buffer);
DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer);
UINT GetWindowsDirectoryW(LPWSTR buffer, UINT size);
DWORD GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buffer, LPWSTR* filePart);
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD sizeHigh, DWORD sizeLow, LPCWSTR name);
HANDLE OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name);
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytes);
BOOL UnmapViewOfFile(LPCVOID base);

// shlwapi / shell32 / ole32
BOOL PathIsRelativeW(LPCWSTR path);
BOOL PathIsRelativeA(LPCSTR path);
BOOL PathFileExistsW(LPCWSTR path);
HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID rfid, DWORD flags, HANDLE token, PWSTR* path);
HRESULT SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path);
int SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, const SECURITY_ATTRIBUTES* sa);
void CoTaskMemFree(LPVOID pv);
HRESULT CoInitializeEx(LPVOID reserved, DWORD coInit);
HRESULT CoInitialize(LPVOID reserved);
void CoUninitialize(void);
HINSTANCE ShellExecuteW(HWND hwnd, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT showCmd);
HINSTANCE ShellExecuteA(HWND hwnd, LPCSTR operation, LPCSTR file, LPCSTR parameters, LPCSTR directory, INT showCmd);

// --- windows -------------------------------------------------------------------
BOOL IsWindow(HWND hwnd);
BOOL IsWindowVisible(HWND hwnd);
BOOL IsWindowUnicode(HWND hwnd);
BOOL IsIconic(HWND hwnd);
BOOL IsZoomed(HWND hwnd);
BOOL IsChild(HWND parent, HWND hwnd);
BOOL GetClientRect(HWND hwnd, LPRECT rect);
BOOL GetWindowRect(HWND hwnd, LPRECT rect);
BOOL ClientToScreen(HWND hwnd, LPPOINT point);
BOOL ScreenToClient(HWND hwnd, LPPOINT point);
int MapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count);
BOOL AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD exStyle);
HWND GetForegroundWindow(void);
BOOL SetForegroundWindow(HWND hwnd);
HWND GetActiveWindow(void);
HWND GetFocus(void);
HWND SetFocus(HWND hwnd);
HWND GetAncestor(HWND hwnd, UINT flags);
HWND GetParent(HWND hwnd);
HWND GetWindow(HWND hwnd, UINT cmd);
HWND GetDesktopWindow(void);
DWORD GetWindowThreadProcessId(HWND hwnd, LPDWORD processId);
LONG_PTR GetWindowLongPtrW(HWND hwnd, int index);
LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR value);
LONG GetWindowLongW(HWND hwnd, int index);
LONG SetWindowLongW(HWND hwnd, int index, LONG value);
BOOL SetWindowPos(HWND hwnd, HWND insertAfter, int x, int y, int cx, int cy, UINT flags);
BOOL MoveWindow(HWND hwnd, int x, int y, int width, int height, BOOL repaint);
BOOL ShowWindow(HWND hwnd, int cmd);
BOOL UpdateWindow(HWND hwnd);
BOOL InvalidateRect(HWND hwnd, const RECT* rect, BOOL erase);
BOOL RedrawWindow(HWND hwnd, const RECT* rect, HRGN region, UINT flags);
BOOL DestroyWindow(HWND hwnd);
ATOM RegisterClassExW(const WNDCLASSEXW* wc);
BOOL UnregisterClassW(LPCWSTR className, HINSTANCE instance);
HWND CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style, int x, int y, int width, int height,
                     HWND parent, HMENU menu, HINSTANCE instance, LPVOID param);
LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
HANDLE GetPropW(HWND hwnd, LPCWSTR name);
BOOL SetPropW(HWND hwnd, LPCWSTR name, HANDLE data);
HANDLE RemovePropW(HWND hwnd, LPCWSTR name);
BOOL EnumWindows(WNDENUMPROC proc, LPARAM lParam);
int GetWindowTextA(HWND hwnd, LPSTR text, int maxCount);
int GetWindowTextW(HWND hwnd, LPWSTR text, int maxCount);
int GetWindowTextLengthW(HWND hwnd);
BOOL SetWindowTextW(HWND hwnd, LPCWSTR text);
int GetClassNameA(HWND hwnd, LPSTR className, int maxCount);
int GetClassNameW(HWND hwnd, LPWSTR className, int maxCount);
HWND WindowFromDC(HDC hdc);
HWND WindowFromPoint(POINT point);
HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
HMONITOR MonitorFromRect(LPCRECT rect, DWORD flags);
HMONITOR MonitorFromPoint(POINT point, DWORD flags);
BOOL GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info);
BOOL GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info);
BOOL EnumDisplayMonitors(HDC hdc, LPCRECT clip, MONITORENUMPROC proc, LPARAM data);
int GetSystemMetrics(int index);
int GetSystemMetricsForDpi(int index, UINT dpi);
UINT GetDpiForWindow(HWND hwnd);
UINT GetDpiForSystem(void);
BOOL SetProcessDPIAware(void);
BOOL SystemParametersInfoW(UINT action, UINT param, PVOID data, UINT winIni);
BOOL SystemParametersInfoA(UINT action, UINT param, PVOID data, UINT winIni);
int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type);
int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type);
BOOL FlashWindow(HWND hwnd, BOOL invert);

// --- messages ------------------------------------------------------------------
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT SendMessageTimeoutW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT flags, UINT timeout, PDWORD_PTR result);
BOOL SendNotifyMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL PostThreadMessageW(DWORD threadId, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL GetMessageW(LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax);
BOOL PeekMessageW(LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax, UINT removeMsg);
BOOL TranslateMessage(const MSG* msg);
LRESULT DispatchMessageW(const MSG* msg);
void PostQuitMessage(int exitCode);
UINT RegisterWindowMessageA(LPCSTR name);
UINT RegisterWindowMessageW(LPCWSTR name);
UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, void* timerProc);
BOOL KillTimer(HWND hwnd, UINT_PTR id);
LPARAM GetMessageExtraInfo(void);
LONG GetMessageTime(void);
DWORD GetQueueStatus(UINT flags);

// --- keyboard / mouse ----------------------------------------------------------
SHORT GetAsyncKeyState(int vk);
SHORT GetKeyState(int vk);
BOOL GetKeyboardState(PBYTE keyState);
BOOL SetKeyboardState(PBYTE keyState);
UINT MapVirtualKeyW(UINT code, UINT mapType);
UINT MapVirtualKeyA(UINT code, UINT mapType);
UINT MapVirtualKeyExW(UINT code, UINT mapType, HKL layout);
int ToUnicodeEx(UINT vk, UINT scanCode, const BYTE* keyState, LPWSTR buffer, int bufferSize, UINT flags, HKL layout);
int ToUnicode(UINT vk, UINT scanCode, const BYTE* keyState, LPWSTR buffer, int bufferSize, UINT flags);
SHORT VkKeyScanA(CHAR ch);
SHORT VkKeyScanW(WCHAR ch);
SHORT VkKeyScanExW(WCHAR ch, HKL layout);
int GetKeyNameTextA(LONG lParam, LPSTR buffer, int size);
int GetKeyNameTextW(LONG lParam, LPWSTR buffer, int size);
HKL GetKeyboardLayout(DWORD threadId);
UINT SendInput(UINT count, LPINPUT inputs, int size);
void keybd_event(BYTE vk, BYTE scan, DWORD flags, ULONG_PTR extraInfo);
HHOOK SetWindowsHookExW(int idHook, HOOKPROC proc, HINSTANCE module, DWORD threadId);
BOOL UnhookWindowsHookEx(HHOOK hook);
LRESULT CallNextHookEx(HHOOK hook, int code, WPARAM wParam, LPARAM lParam);
BOOL GetCursorPos(LPPOINT point);
BOOL SetCursorPos(int x, int y);
BOOL GetPhysicalCursorPos(LPPOINT point);
BOOL ClipCursor(const RECT* rect);
BOOL GetClipCursor(LPRECT rect);
HCURSOR SetCursor(HCURSOR cursor);
HCURSOR GetCursor(void);
int ShowCursor(BOOL show);
BOOL GetCursorInfo(PCURSORINFO info);
HWND SetCapture(HWND hwnd);
BOOL ReleaseCapture(void);
HWND GetCapture(void);
BOOL TrackMouseEvent(LPTRACKMOUSEEVENT eventTrack);
UINT GetDoubleClickTime(void);
UINT GetCaretBlinkTime(void);
BOOL SwapMouseButton(BOOL swap);
UINT GetRawInputData(HRAWINPUT rawInput, UINT command, LPVOID data, PUINT size, UINT headerSize);

// --- cursors / icons / gdi -------------------------------------------------------
HCURSOR LoadCursorW(HINSTANCE instance, LPCWSTR name);
HCURSOR LoadCursorA(HINSTANCE instance, LPCSTR name);
HANDLE LoadImageW(HINSTANCE instance, LPCWSTR name, UINT type, int cx, int cy, UINT flags);
HANDLE CopyImage(HANDLE image, UINT type, int cx, int cy, UINT flags);
BOOL DestroyCursor(HCURSOR cursor);
BOOL DestroyIcon(HICON icon);
HICON CreateIconIndirect(PICONINFO info);
BOOL GetIconInfo(HICON icon, PICONINFO info);
BOOL GetIconInfoExW(HICON icon, PICONINFOEXW info);
HDC GetDC(HWND hwnd);
HDC GetWindowDC(HWND hwnd);
int ReleaseDC(HWND hwnd, HDC hdc);
HDC CreateCompatibleDC(HDC hdc);
BOOL DeleteDC(HDC hdc);
HBITMAP CreateCompatibleBitmap(HDC hdc, int width, int height);
HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO* bmi, UINT usage, void** bits, HANDLE section, DWORD offset);
HBITMAP CreateBitmap(int width, int height, UINT planes, UINT bitCount, const void* bits);
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj);
BOOL DeleteObject(HGDIOBJ obj);
int GetObjectW(HANDLE obj, int size, LPVOID out);
int GetObjectA(HANDLE obj, int size, LPVOID out);
int GetDIBits(HDC hdc, HBITMAP bitmap, UINT start, UINT lines, LPVOID bits, LPBITMAPINFO bmi, UINT usage);
LONG GetBitmapBits(HBITMAP bitmap, LONG count, LPVOID bits);
BOOL BitBlt(HDC dst, int x, int y, int cx, int cy, HDC src, int x1, int y1, DWORD rop);
BOOL PrintWindow(HWND hwnd, HDC hdc, UINT flags);
int FillRect(HDC hdc, const RECT* rect, HBRUSH brush);
HGDIOBJ GetStockObject(int index);
HBRUSH CreateSolidBrush(COLORREF color);
int SetROP2(HDC hdc, int mode);
BOOL DrawIconEx(HDC hdc, int x, int y, HICON icon, int cx, int cy, UINT step, HBRUSH brush, UINT flags);
int GetDeviceCaps(HDC hdc, int index);
HRGN CreateRectRgn(int x1, int y1, int x2, int y2);
#define DI_NORMAL 0x0003
#define DI_MASK 0x0001
#define DI_IMAGE 0x0002

// --- clipboard -------------------------------------------------------------------
BOOL OpenClipboard(HWND owner);
BOOL CloseClipboard(void);
BOOL EmptyClipboard(void);
HANDLE SetClipboardData(UINT format, HANDLE mem);
HANDLE GetClipboardData(UINT format);
BOOL IsClipboardFormatAvailable(UINT format);

// --- common dialogs ----------------------------------------------------------------
BOOL GetOpenFileNameW(LPOPENFILENAMEW ofn);
BOOL GetSaveFileNameW(LPOPENFILENAMEW ofn);
DWORD CommDlgExtendedError(void);

// --- dwm -------------------------------------------------------------------------
HRESULT DwmGetWindowAttribute(HWND hwnd, DWORD attribute, PVOID value, DWORD size);
HRESULT DwmEnableBlurBehindWindow(HWND hwnd, const DWM_BLURBEHIND* blurBehind);
HRESULT DwmIsCompositionEnabled(BOOL* enabled);
HRESULT DwmGetColorizationColor(DWORD* color, BOOL* opaqueBlend);

// --- dbghelp -----------------------------------------------------------------------
DWORD SymGetOptions(void);
DWORD SymSetOptions(DWORD options);
BOOL SymInitialize(HANDLE process, PCSTR searchPath, BOOL invadeProcess);
BOOL SymCleanup(HANDLE process);
BOOL SymFromAddr(HANDLE process, DWORD64 address, PDWORD64 displacement, PSYMBOL_INFO symbol);
BOOL SymGetLineFromAddr64(HANDLE process, DWORD64 address, PDWORD displacement, PIMAGEHLP_LINE64 line);
DWORD64 SymLoadModuleEx(HANDLE process, HANDLE file, PCSTR imageName, PCSTR moduleName, DWORD64 baseOfDll, DWORD dllSize,
                        PVOID data, DWORD flags);
BOOL SymRefreshModuleList(HANDLE process);
BOOL SymSetSearchPath(HANDLE process, PCSTR searchPath);

// --- winhttp -------------------------------------------------------------------------
HINTERNET WinHttpOpen(LPCWSTR agent, DWORD accessType, LPCWSTR proxy, LPCWSTR proxyBypass, DWORD flags);
BOOL WinHttpSetTimeouts(HINTERNET handle, int resolveTimeout, int connectTimeout, int sendTimeout, int receiveTimeout);
BOOL WinHttpSetOption(HINTERNET handle, DWORD option, LPVOID buffer, DWORD length);
HINTERNET WinHttpConnect(HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD reserved);
HINTERNET WinHttpOpenRequest(HINTERNET connect, LPCWSTR verb, LPCWSTR objectName, LPCWSTR version, LPCWSTR referrer,
                             LPCWSTR* acceptTypes, DWORD flags);
BOOL WinHttpAddRequestHeaders(HINTERNET request, LPCWSTR headers, DWORD headersLength, DWORD modifiers);
BOOL WinHttpSendRequest(HINTERNET request, LPCWSTR headers, DWORD headersLength, LPVOID optional, DWORD optionalLength,
                        DWORD totalLength, DWORD_PTR context);
BOOL WinHttpReceiveResponse(HINTERNET request, LPVOID reserved);
BOOL WinHttpQueryHeaders(HINTERNET request, DWORD infoLevel, LPCWSTR name, LPVOID buffer, LPDWORD bufferLength, LPDWORD index);
BOOL WinHttpQueryDataAvailable(HINTERNET request, LPDWORD available);
BOOL WinHttpReadData(HINTERNET request, LPVOID buffer, DWORD toRead, LPDWORD read);
BOOL WinHttpCloseHandle(HINTERNET handle);
BOOL WinHttpCrackUrl(LPCWSTR url, DWORD urlLength, DWORD flags, LPURL_COMPONENTS components);

// --- wgl ---------------------------------------------------------------------------------
// wglGetCurrentContext returns the current CGL context. Toolscreen renders in its
// own core-profile context, so this is that context during the swap hook.
HGLRC wglGetCurrentContext(void);
HDC wglGetCurrentDC(void);
FARPROC wglGetProcAddress(LPCSTR name);
BOOL wglMakeCurrent(HDC hdc, HGLRC context);
BOOL SwapBuffers(HDC hdc);

// --- registry (always reports "not found") --------------------------------------------
LONG RegOpenKeyExA(HKEY key, LPCSTR subKey, DWORD options, DWORD desired, HKEY* result);
LONG RegCloseKey(HKEY key);

// --- MSVC CRT ---------------------------------------------------------------------------
int sprintf_s(char* buffer, size_t size, const char* format, ...);
int swprintf_s(wchar_t* buffer, size_t size, const wchar_t* format, ...);
int vsprintf_s(char* buffer, size_t size, const char* format, va_list args);
errno_t strcpy_s(char* dest, size_t size, const char* src);
errno_t strncpy_s(char* dest, size_t size, const char* src, size_t count);
errno_t strcat_s(char* dest, size_t size, const char* src);
errno_t wcscpy_s(wchar_t* dest, size_t size, const wchar_t* src);
errno_t wcsncpy_s(wchar_t* dest, size_t size, const wchar_t* src, size_t count);
errno_t wcscat_s(wchar_t* dest, size_t size, const wchar_t* src);
size_t strnlen_s(const char* str, size_t size);
size_t wcsnlen_s(const wchar_t* str, size_t size);
errno_t localtime_s(struct tm* result, const time_t* time);
errno_t gmtime_s(struct tm* result, const time_t* time);
errno_t getenv_s(size_t* required, char* buffer, size_t size, const char* name);
errno_t _wgetenv_s(size_t* required, wchar_t* buffer, size_t size, const wchar_t* name);
errno_t _dupenv_s(char** buffer, size_t* size, const char* name);
errno_t fopen_s(FILE** file, const char* name, const char* mode);
errno_t _wfopen_s(FILE** file, const wchar_t* name, const wchar_t* mode);
FILE* _wfopen(const wchar_t* name, const wchar_t* mode);
int _wremove(const wchar_t* name);
int _wrename(const wchar_t* from, const wchar_t* to);

} // extern "C"

// Templated array overloads provided by the MSVC CRT.
template <size_t N> inline int sprintf_s(char (&buffer)[N], const char* format, ...) {
    va_list args;
    va_start(args, format);
    int r = vsnprintf(buffer, N, format, args);
    va_end(args);
    return r;
}
template <size_t N> inline int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    int r = vswprintf(buffer, N, format, args);
    va_end(args);
    return r;
}
template <size_t N> inline errno_t strcpy_s(char (&dest)[N], const char* src) { return strcpy_s(dest, N, src); }
template <size_t N> inline errno_t strncpy_s(char (&dest)[N], const char* src, size_t count) { return strncpy_s(dest, N, src, count); }
template <size_t N> inline errno_t wcscpy_s(wchar_t (&dest)[N], const wchar_t* src) { return wcscpy_s(dest, N, src); }
template <size_t N> inline errno_t wcsncpy_s(wchar_t (&dest)[N], const wchar_t* src, size_t count) {
    return wcsncpy_s(dest, N, src, count);
}
template <size_t N> inline errno_t strcat_s(char (&dest)[N], const char* src) { return strcat_s(dest, N, src); }

// ANSI/Unicode name mapping (UNICODE is defined for the build).
#define GetModuleHandle GetModuleHandleW
#define GetModuleHandleEx GetModuleHandleExW
#define GetModuleFileName GetModuleFileNameW
#define LoadLibrary LoadLibraryW
#define LoadLibraryEx LoadLibraryExW
#define CreateFile CreateFileW
#define DeleteFile DeleteFileW
#define MoveFile MoveFileW
#define MoveFileEx MoveFileExW
#define CopyFile CopyFileW
#define CreateDirectory CreateDirectoryW
#define GetFileAttributes GetFileAttributesW
#define GetFileAttributesEx GetFileAttributesExW
#define GetTempPath GetTempPathW
#define GetCurrentDirectory GetCurrentDirectoryW
#define GetEnvironmentVariable GetEnvironmentVariableW
#define CreateEvent CreateEventW
#define CreateWaitableTimer CreateWaitableTimerW
#define FindResource FindResourceW
#define FormatMessage FormatMessageA
#define OutputDebugString OutputDebugStringW
#define GetWindowLongPtr GetWindowLongPtrW
#define SetWindowLongPtr SetWindowLongPtrW
#define GetWindowLong GetWindowLongW
#define SetWindowLong SetWindowLongW
#define DefWindowProc DefWindowProcW
#define CallWindowProc CallWindowProcW
#define PostMessage PostMessageW
#define SendMessage SendMessageW
#define SendMessageTimeout SendMessageTimeoutW
#define PostThreadMessage PostThreadMessageW
#define GetMessage GetMessageW
#define PeekMessage PeekMessageW
#define DispatchMessage DispatchMessageW
#define RegisterWindowMessage RegisterWindowMessageW
#define RegisterClassEx RegisterClassExW
#define UnregisterClass UnregisterClassW
#define CreateWindowEx CreateWindowExW
#define GetWindowText GetWindowTextW
#define GetClassName GetClassNameW
#define GetProp GetPropW
#define SetProp SetPropW
#define RemoveProp RemovePropW
#define GetMonitorInfo GetMonitorInfoW
#define SystemParametersInfo SystemParametersInfoW
#define MessageBox MessageBoxW
#define MapVirtualKey MapVirtualKeyW
#define MapVirtualKeyEx MapVirtualKeyExW
#define VkKeyScan VkKeyScanW
#define VkKeyScanEx VkKeyScanExW
#define GetKeyNameText GetKeyNameTextW
#define SetWindowsHookEx SetWindowsHookExW
#define LoadCursor LoadCursorW
#define LoadImage LoadImageW
#define GetIconInfoEx GetIconInfoExW
#define GetObject GetObjectW
#define GetOpenFileName GetOpenFileNameW
#define GetSaveFileName GetSaveFileNameW
#define ShellExecute ShellExecuteW
#define SHGetFolderPath SHGetFolderPathW
#define PathIsRelative PathIsRelativeW
#define PathFileExists PathFileExistsW
#define WinHttpCrackUrlW WinHttpCrackUrl
