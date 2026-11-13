// kernel32 / shell32 / shlwapi / ole32 / dbghelp / CRT subset for macOS.

#include "platform/macos/win32/win32.h"

#include "platform/macos/os/mac_os.h"
#include "platform/macos/win32/resources.h"
#include "platform/macos/win32/win32_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <copyfile.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

thread_local DWORD t_lastError = 0;

constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ull;

uint64_t NowSteadyNs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// --- kernel objects ------------------------------------------------------------

enum class ObjType : uint32_t { File = 1, Event, Timer, Thread, Process, Global };

struct KObject {
    static constexpr uint32_t kMagic = 0x54534B4F; // 'TSKO'
    uint32_t magic = kMagic;
    ObjType type;
    explicit KObject(ObjType t) : type(t) {}
    virtual ~KObject() { magic = 0; }
};

struct FileObj : KObject {
    int fd = -1;
    std::string path;
    FileObj() : KObject(ObjType::File) {}
    ~FileObj() override {
        if (fd >= 0) close(fd);
    }
};

struct Waitable : KObject {
    std::mutex mutex;
    std::condition_variable cv;
    bool signaled = false;
    bool manualReset = true;
    explicit Waitable(ObjType t) : KObject(t) {}
};

struct EventObj : Waitable {
    EventObj() : Waitable(ObjType::Event) {}
};

struct TimerObj : Waitable {
    uint64_t dueNs = 0;
    bool armed = false;
    bool highResolution = false;
    TimerObj() : Waitable(ObjType::Timer) {}
};

struct ThreadObj : Waitable {
    pthread_t thread{};
    DWORD id = 0;
    LPTHREAD_START_ROUTINE start = nullptr;
    LPVOID param = nullptr;
    std::atomic<int> refs{ 2 }; // handle + running thread
    ThreadObj() : Waitable(ObjType::Thread) {}
};

struct ProcessObj : KObject {
    int pid = 0;
    ProcessObj() : KObject(ObjType::Process) {}
};

struct GlobalObj {
    size_t size = 0;
    std::vector<uint8_t> data;
};

KObject* AsObject(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    const uintptr_t v = reinterpret_cast<uintptr_t>(h);
    if (v < 0x1000 || v > (uintptr_t(-1) - 0x1000)) return nullptr;
    KObject* obj = static_cast<KObject*>(h);
    return obj->magic == KObject::kMagic ? obj : nullptr;
}

template <typename T> T* As(HANDLE h, ObjType type) {
    KObject* obj = AsObject(h);
    return (obj && obj->type == type) ? static_cast<T*>(obj) : nullptr;
}

DWORD ErrnoToWin32(int err) {
    switch (err) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EBUSY: return ERROR_SHARING_VIOLATION;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    default: return ERROR_INVALID_PARAMETER;
    }
}

void SetErrno(int err) { t_lastError = ErrnoToWin32(err); }

FILETIME ToFileTime(uint64_t value) {
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return ft;
}

uint64_t FromFileTime(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

FILETIME TimespecToFileTime(const struct timespec& ts) {
    const uint64_t v = static_cast<uint64_t>(ts.tv_sec) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100ull + kUnixEpochAsFileTime;
    return ToFileTime(v);
}

void FillSystemTime(const struct tm& t, int ms, LPSYSTEMTIME st) {
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = static_cast<WORD>(ms);
}

std::string NarrowPath(LPCSTR path) {
    std::string s = path ? path : "";
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

std::string NarrowPath(LPCWSTR path) { return NarrowPath(MacWin32::WideToUtf8(path ? path : L"").c_str()); }

DWORD CopyOut(const std::wstring& value, LPWSTR buffer, DWORD size) {
    const DWORD needed = static_cast<DWORD>(value.size());
    if (!buffer || size <= needed) return needed + 1;
    std::wmemcpy(buffer, value.c_str(), needed + 1);
    return needed;
}

DWORD CopyOut(const std::string& value, LPSTR buffer, DWORD size) {
    const DWORD needed = static_cast<DWORD>(value.size());
    if (!buffer || size <= needed) return needed + 1;
    std::memcpy(buffer, value.c_str(), needed + 1);
    return needed;
}

bool SpecialEnvironment(const std::string& name, std::string& out) {
    if (name == "USERPROFILE" || name == "HOME") {
        out = MacOs::HomeDirectory();
        return true;
    }
    if (name == "TEMP" || name == "TMP") {
        out = MacOs::TemporaryDirectory();
        if (out.size() > 1 && out.back() == '/') out.pop_back();
        return true;
    }
    if (name == "APPDATA" || name == "LOCALAPPDATA") {
        out = MacOs::ApplicationSupportDirectory();
        return !out.empty();
    }
    if (name == "USERNAME") {
        const char* user = getenv("USER");
        if (!user) return false;
        out = user;
        return true;
    }
    return false;
}

bool GetEnv(const std::string& name, std::string& out) {
    if (const char* v = getenv(name.c_str())) {
        out = v;
        return true;
    }
    return SpecialEnvironment(name, out);
}

// --- images ---------------------------------------------------------------------

std::string ImagePathForHeader(const void* header) {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (_dyld_get_image_header(i) == header) {
            const char* name = _dyld_get_image_name(i);
            return name ? name : "";
        }
    }
    return {};
}

std::string Basename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) == 0; }

const MacEmbeddedResource* FindEmbedded(unsigned id) {
    for (unsigned i = 0; i < g_macEmbeddedResourceCount; ++i) {
        if (g_macEmbeddedResources[i].id == id) return &g_macEmbeddedResources[i];
    }
    return nullptr;
}

std::string KnownFolder(uint32_t data1) {
    switch (data1) {
    case 0x374DE290: return MacOs::DownloadsDirectory();
    case 0xFDD39AD0: return MacOs::DocumentsDirectory();
    case 0xB4BFCC3A: return MacOs::DesktopDirectory();
    case 0x33E28130: return MacOs::PicturesDirectory();
    case 0x3EB685DB: return MacOs::ApplicationSupportDirectory();
    case 0xF1B32785: return MacOs::ApplicationSupportDirectory();
    case 0x5E6C858F: return MacOs::HomeDirectory();
    case 0xFD228CB7: return "/System/Library/Fonts";
    default: return {};
    }
}

void* ThreadEntry(void* arg) {
    ThreadObj* t = static_cast<ThreadObj*>(arg);
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    t->start(t->param);
    {
        std::lock_guard<std::mutex> lock(t->mutex);
        t->signaled = true;
    }
    t->cv.notify_all();
    if (t->refs.fetch_sub(1) == 1) delete t;
    return nullptr;
}

bool WaitOn(Waitable* w, DWORD milliseconds) {
    if (TimerObj* timer = dynamic_cast<TimerObj*>(w)) {
        std::unique_lock<std::mutex> lock(timer->mutex);
        if (!timer->armed && !timer->signaled) {
            if (milliseconds == INFINITE) {
                timer->cv.wait(lock, [&] { return timer->armed || timer->signaled; });
            } else if (!timer->cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return timer->armed || timer->signaled; })) {
                return false;
            }
        }
        if (timer->armed) {
            const uint64_t due = timer->dueNs;
            const uint64_t deadline = milliseconds == INFINITE ? UINT64_MAX : NowSteadyNs() + uint64_t(milliseconds) * 1000000ull;
            lock.unlock();
            const uint64_t target = due < deadline ? due : deadline;
            // Sleep most of the way, then spin for sub-millisecond precision.
            for (;;) {
                const uint64_t now = NowSteadyNs();
                if (now >= target) break;
                const uint64_t remaining = target - now;
                if (remaining > 1500000ull) {
                    struct timespec ts { 0, static_cast<long>(remaining - 1000000ull) };
                    nanosleep(&ts, nullptr);
                } else if (remaining > 200000ull) {
                    struct timespec ts { 0, 100000 };
                    nanosleep(&ts, nullptr);
                }
            }
            lock.lock();
            if (due > deadline) return false;
            timer->armed = false;
            if (timer->manualReset) timer->signaled = true;
            return true;
        }
        if (!timer->manualReset) timer->signaled = false;
        return true;
    }

    std::unique_lock<std::mutex> lock(w->mutex);
    if (milliseconds == INFINITE) {
        w->cv.wait(lock, [&] { return w->signaled; });
    } else if (!w->cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return w->signaled; })) {
        return false;
    }
    if (!w->manualReset && w->type == ObjType::Event) w->signaled = false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Helpers shared with the other Win32 files
// ---------------------------------------------------------------------------------------------
namespace MacWin32 {

std::string WideToUtf8(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (size_t i = 0; i < wide.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(wide[i]);
        // Accept UTF-16 surrogate pairs stored in wchar_t as well.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size()) {
            const uint32_t low = static_cast<uint32_t>(wide[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool Utf8ToWide(const char* data, size_t length, std::wstring& out, bool strict) {
    out.clear();
    out.reserve(length);
    const unsigned char* s = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;
    bool valid = true;
    while (i < length) {
        uint32_t cp = s[i];
        size_t extra = 0;
        if (cp < 0x80) {
            extra = 0;
        } else if ((cp & 0xE0) == 0xC0) {
            cp &= 0x1F;
            extra = 1;
        } else if ((cp & 0xF0) == 0xE0) {
            cp &= 0x0F;
            extra = 2;
        } else if ((cp & 0xF8) == 0xF0) {
            cp &= 0x07;
            extra = 3;
        } else {
            valid = false;
            out.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            if (i + k >= length || (s[i + k] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (!ok) {
            valid = false;
            out.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }
        out.push_back(static_cast<wchar_t>(cp));
        i += extra + 1;
    }
    return valid || !strict;
}

std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    Utf8ToWide(s.data(), s.size(), out, false);
    return out;
}

void SetLastErrorFromErrno(int err) { SetErrno(err); }

} // namespace MacWin32

extern "C" {

// --- errors ---------------------------------------------------------------------------------

DWORD GetLastError(void) { return t_lastError; }
void SetLastError(DWORD code) { t_lastError = code; }

DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD messageId, DWORD, LPSTR buffer, DWORD size, va_list*) {
    std::string text = "Error " + std::to_string(messageId);
    switch (messageId) {
    case ERROR_SUCCESS: text = "The operation completed successfully."; break;
    case ERROR_FILE_NOT_FOUND: text = "The system cannot find the file specified."; break;
    case ERROR_PATH_NOT_FOUND: text = "The system cannot find the path specified."; break;
    case ERROR_ACCESS_DENIED: text = "Access is denied."; break;
    case ERROR_FILE_EXISTS: text = "The file exists."; break;
    case ERROR_ALREADY_EXISTS: text = "Cannot create a file when that file already exists."; break;
    default: break;
    }
    text += "\r\n";
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        char* copy = static_cast<char*>(malloc(text.size() + 1));
        if (!copy) return 0;
        std::memcpy(copy, text.c_str(), text.size() + 1);
        *reinterpret_cast<LPSTR*>(buffer) = copy;
        return static_cast<DWORD>(text.size());
    }
    if (!buffer || size == 0) return 0;
    const size_t n = std::min<size_t>(text.size(), size - 1);
    std::memcpy(buffer, text.c_str(), n);
    buffer[n] = '\0';
    return static_cast<DWORD>(n);
}

DWORD FormatMessageW(DWORD flags, LPCVOID source, DWORD messageId, DWORD languageId, LPWSTR buffer, DWORD size, va_list* args) {
    char narrow[512];
    const DWORD n = FormatMessageA(flags & ~FORMAT_MESSAGE_ALLOCATE_BUFFER, source, messageId, languageId, narrow, sizeof(narrow), args);
    const std::wstring wide = MacWin32::Utf8ToWide(std::string(narrow, n));
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        wchar_t* copy = static_cast<wchar_t*>(malloc((wide.size() + 1) * sizeof(wchar_t)));
        if (!copy) return 0;
        std::wmemcpy(copy, wide.c_str(), wide.size() + 1);
        *reinterpret_cast<LPWSTR*>(buffer) = copy;
        return static_cast<DWORD>(wide.size());
    }
    if (!buffer || size == 0) return 0;
    const size_t count = std::min<size_t>(wide.size(), size - 1);
    std::wmemcpy(buffer, wide.c_str(), count);
    buffer[count] = L'\0';
    return static_cast<DWORD>(count);
}

HANDLE LocalFree(HANDLE mem) {
    free(mem);
    return nullptr;
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_topLevelFilter = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    // Never installed as a signal handler: the JVM owns SIGSEGV/SIGBUS.
    LPTOP_LEVEL_EXCEPTION_FILTER previous = g_topLevelFilter;
    g_topLevelFilter = filter;
    return previous;
}

static thread_local _se_translator_function t_seTranslator = nullptr;
_se_translator_function _set_se_translator(_se_translator_function fn) {
    _se_translator_function previous = t_seTranslator;
    t_seTranslator = fn;
    return previous;
}

void OutputDebugStringA(LPCSTR) {}
void OutputDebugStringW(LPCWSTR) {}
BOOL IsDebuggerPresent(void) { return FALSE; }

USHORT RtlCaptureStackBackTrace(ULONG framesToSkip, ULONG framesToCapture, PVOID* backTrace, PDWORD backTraceHash) {
    void* frames[128];
    const int total = backtrace(frames, 128);
    const int skip = static_cast<int>(framesToSkip) + 1;
    int count = 0;
    DWORD hash = 0;
    for (int i = skip; i < total && count < static_cast<int>(framesToCapture); ++i) {
        backTrace[count++] = frames[i];
        hash += static_cast<DWORD>(reinterpret_cast<uintptr_t>(frames[i]));
    }
    if (backTraceHash) *backTraceHash = hash;
    return static_cast<USHORT>(count);
}

PVOID RtlPcToFileHeader(PVOID pcValue, PVOID* baseOfImage) {
    Dl_info info{};
    if (!pcValue || dladdr(pcValue, &info) == 0 || !info.dli_fbase) {
        if (baseOfImage) *baseOfImage = nullptr;
        return nullptr;
    }
    // LWJGL's JNI stubs forward GL calls on behalf of Java code. Treat them like
    // the JIT code they serve so Toolscreen's GL hooks see Minecraft's calls
    // the same way they do when the stubs tail-call on Windows.
    const std::string base = Basename(info.dli_fname ? info.dli_fname : "");
    if (base.rfind("liblwjgl", 0) == 0) {
        if (baseOfImage) *baseOfImage = nullptr;
        return nullptr;
    }
    if (baseOfImage) *baseOfImage = info.dli_fbase;
    return info.dli_fbase;
}

ULONGLONG VerSetConditionMask(ULONGLONG, DWORD, BYTE) { return 0; }

// --- time ---------------------------------------------------------------------------------------

DWORD GetTickCount(void) { return static_cast<DWORD>(NowSteadyNs() / 1000000ull); }
ULONGLONG GetTickCount64(void) { return NowSteadyNs() / 1000000ull; }

BOOL QueryPerformanceCounter(LARGE_INTEGER* count) {
    if (!count) return FALSE;
    count->QuadPart = static_cast<LONGLONG>(NowSteadyNs());
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
    if (!frequency) return FALSE;
    frequency->QuadPart = 1000000000LL;
    return TRUE;
}

void Sleep(DWORD milliseconds) {
    if (milliseconds == 0) {
        sched_yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

DWORD SleepEx(DWORD milliseconds, BOOL) {
    Sleep(milliseconds);
    return 0;
}

void GetLocalTime(LPSYSTEMTIME st) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    FillSystemTime(t, static_cast<int>(tv.tv_usec / 1000), st);
}

void GetSystemTime(LPSYSTEMTIME st) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    gmtime_r(&tv.tv_sec, &t);
    FillSystemTime(t, static_cast<int>(tv.tv_usec / 1000), st);
}

void GetSystemTimeAsFileTime(LPFILETIME ft) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const uint64_t v = static_cast<uint64_t>(tv.tv_sec) * 10000000ull + static_cast<uint64_t>(tv.tv_usec) * 10ull + kUnixEpochAsFileTime;
    *ft = ToFileTime(v);
}

BOOL FileTimeToLocalFileTime(const FILETIME* fileTime, LPFILETIME localFileTime) {
    if (!fileTime || !localFileTime) return FALSE;
    const uint64_t v = FromFileTime(*fileTime);
    const time_t seconds = static_cast<time_t>((v - kUnixEpochAsFileTime) / 10000000ull);
    struct tm t;
    localtime_r(&seconds, &t);
    const int64_t offset = static_cast<int64_t>(t.tm_gmtoff) * 10000000LL;
    *localFileTime = ToFileTime(static_cast<uint64_t>(static_cast<int64_t>(v) + offset));
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME* fileTime, LPSYSTEMTIME systemTime) {
    if (!fileTime || !systemTime) return FALSE;
    const uint64_t v = FromFileTime(*fileTime);
    if (v < kUnixEpochAsFileTime) return FALSE;
    const uint64_t since = v - kUnixEpochAsFileTime;
    const time_t seconds = static_cast<time_t>(since / 10000000ull);
    const int ms = static_cast<int>((since % 10000000ull) / 10000ull);
    struct tm t;
    gmtime_r(&seconds, &t);
    FillSystemTime(t, ms, systemTime);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft) {
    if (!st || !ft) return FALSE;
    struct tm t {};
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    const time_t seconds = timegm(&t);
    *ft = ToFileTime(static_cast<uint64_t>(seconds) * 10000000ull + st->wMilliseconds * 10000ull + kUnixEpochAsFileTime);
    return TRUE;
}

LONG CompareFileTime(const FILETIME* a, const FILETIME* b) {
    const uint64_t x = FromFileTime(*a);
    const uint64_t y = FromFileTime(*b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// --- processes / threads / sync ----------------------------------------------------------------------

DWORD GetCurrentThreadId(void) {
    thread_local DWORD id = [] {
        uint64_t tid = 0;
        pthread_threadid_np(nullptr, &tid);
        return static_cast<DWORD>(tid);
    }();
    return id;
}

DWORD GetCurrentProcessId(void) { return static_cast<DWORD>(getpid()); }
HANDLE GetCurrentProcess(void) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)); }
HANDLE GetCurrentThread(void) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)); }

HANDLE OpenProcess(DWORD, BOOL, DWORD processId) {
    if (!MacOs::ProcessAlive(static_cast<int>(processId))) {
        t_lastError = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    ProcessObj* p = new ProcessObj();
    p->pid = static_cast<int>(processId);
    return p;
}

static int PidForHandle(HANDLE process) {
    if (process == GetCurrentProcess()) return getpid();
    ProcessObj* p = As<ProcessObj>(process, ObjType::Process);
    return p ? p->pid : -1;
}

BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exitTime, LPFILETIME kernel, LPFILETIME user) {
    const int pid = PidForHandle(process);
    uint64_t startUs = 0;
    if (pid <= 0 || !MacOs::ProcessStartTimeUs(pid, startUs)) {
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (creation) *creation = ToFileTime(startUs * 10ull + kUnixEpochAsFileTime);
    if (exitTime) *exitTime = ToFileTime(0);
    if (kernel) *kernel = ToFileTime(0);
    if (user) *user = ToFileTime(0);
    return TRUE;
}

BOOL GetExitCodeProcess(HANDLE process, LPDWORD exitCode) {
    const int pid = PidForHandle(process);
    if (pid <= 0) return FALSE;
    if (exitCode) *exitCode = MacOs::ProcessAlive(pid) ? STILL_ACTIVE : 0;
    return TRUE;
}

BOOL TerminateProcess(HANDLE process, UINT exitCode) {
    const int pid = PidForHandle(process);
    if (pid <= 0) return FALSE;
    if (pid == getpid()) _exit(static_cast<int>(exitCode));
    return kill(pid, SIGKILL) == 0;
}

void ExitProcess(UINT exitCode) { exit(static_cast<int>(exitCode)); }

BOOL QueryFullProcessImageNameA(HANDLE process, DWORD, LPSTR name, PDWORD size) {
    const int pid = PidForHandle(process);
    const std::string path = pid > 0 ? MacOs::ProcessPath(pid) : std::string();
    if (path.empty() || !name || !size || *size <= path.size()) {
        t_lastError = path.empty() ? ERROR_INVALID_HANDLE : ERROR_INSUFFICIENT_BUFFER;
        return FALSE;
    }
    std::memcpy(name, path.c_str(), path.size() + 1);
    *size = static_cast<DWORD>(path.size());
    return TRUE;
}

BOOL QueryFullProcessImageNameW(HANDLE process, DWORD, LPWSTR name, PDWORD size) {
    const int pid = PidForHandle(process);
    const std::wstring path = pid > 0 ? MacWin32::Utf8ToWide(MacOs::ProcessPath(pid)) : std::wstring();
    if (path.empty() || !name || !size || *size <= path.size()) {
        t_lastError = path.empty() ? ERROR_INVALID_HANDLE : ERROR_INSUFFICIENT_BUFFER;
        return FALSE;
    }
    std::wmemcpy(name, path.c_str(), path.size() + 1);
    *size = static_cast<DWORD>(path.size());
    return TRUE;
}

BOOL CloseHandle(HANDLE handle) {
    KObject* obj = AsObject(handle);
    if (!obj) {
        if (handle == GetCurrentProcess() || handle == GetCurrentThread()) return TRUE;
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (obj->type == ObjType::Thread) {
        ThreadObj* t = static_cast<ThreadObj*>(obj);
        if (t->refs.fetch_sub(1) == 1) delete t;
        return TRUE;
    }
    delete obj;
    return TRUE;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD, LPDWORD threadId) {
    if (!start) return nullptr;
    ThreadObj* t = new ThreadObj();
    t->start = start;
    t->param = param;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stackSize > 0) pthread_attr_setstacksize(&attr, std::max<SIZE_T>(stackSize, PTHREAD_STACK_MIN));
    const int rc = pthread_create(&t->thread, &attr, &ThreadEntry, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete t;
        SetErrno(rc);
        return nullptr;
    }
    uint64_t tid = 0;
    pthread_threadid_np(t->thread, &tid);
    t->id = static_cast<DWORD>(tid);
    if (threadId) *threadId = t->id;
    return t;
}

BOOL SetThreadPriority(HANDLE thread, int priority) {
    if (thread != GetCurrentThread()) return TRUE;
    qos_class_t qos = QOS_CLASS_DEFAULT;
    if (priority >= THREAD_PRIORITY_HIGHEST) qos = QOS_CLASS_USER_INTERACTIVE;
    else if (priority == THREAD_PRIORITY_ABOVE_NORMAL) qos = QOS_CLASS_USER_INITIATED;
    else if (priority < THREAD_PRIORITY_NORMAL) qos = QOS_CLASS_UTILITY;
    pthread_set_qos_class_self_np(qos, 0);
    return TRUE;
}

int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }

HRESULT SetThreadDescription(HANDLE thread, PCWSTR description) {
    if (thread == GetCurrentThread() && description) pthread_setname_np(MacWin32::WideToUtf8(description).c_str());
    return S_OK;
}

BOOL DisableThreadLibraryCalls(HMODULE) { return TRUE; }

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manualReset, BOOL initialState, LPCWSTR) {
    EventObj* e = new EventObj();
    e->manualReset = manualReset != FALSE;
    e->signaled = initialState != FALSE;
    return e;
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR) {
    return CreateEventW(sa, manualReset, initialState, nullptr);
}

BOOL SetEvent(HANDLE event) {
    EventObj* e = As<EventObj>(event, ObjType::Event);
    if (!e) return FALSE;
    {
        std::lock_guard<std::mutex> lock(e->mutex);
        e->signaled = true;
    }
    if (e->manualReset) e->cv.notify_all();
    else e->cv.notify_one();
    return TRUE;
}

BOOL ResetEvent(HANDLE event) {
    EventObj* e = As<EventObj>(event, ObjType::Event);
    if (!e) return FALSE;
    std::lock_guard<std::mutex> lock(e->mutex);
    e->signaled = false;
    return TRUE;
}

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES, BOOL manualReset, LPCWSTR) {
    TimerObj* t = new TimerObj();
    t->manualReset = manualReset != FALSE;
    return t;
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD) {
    TimerObj* t = new TimerObj();
    t->manualReset = (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    t->highResolution = (flags & CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) != 0;
    return t;
}

BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* dueTime, LONG, PTIMERAPCROUTINE, LPVOID, BOOL) {
    TimerObj* t = As<TimerObj>(timer, ObjType::Timer);
    if (!t || !dueTime) return FALSE;
    uint64_t due = NowSteadyNs();
    if (dueTime->QuadPart < 0) {
        due += static_cast<uint64_t>(-dueTime->QuadPart) * 100ull;
    } else {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const uint64_t nowFt = FromFileTime(now);
        const uint64_t target = static_cast<uint64_t>(dueTime->QuadPart);
        if (target > nowFt) due += (target - nowFt) * 100ull;
    }
    {
        std::lock_guard<std::mutex> lock(t->mutex);
        t->dueNs = due;
        t->armed = true;
        t->signaled = false;
    }
    t->cv.notify_all();
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE timer) {
    TimerObj* t = As<TimerObj>(timer, ObjType::Timer);
    if (!t) return FALSE;
    std::lock_guard<std::mutex> lock(t->mutex);
    t->armed = false;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    KObject* obj = AsObject(handle);
    if (!obj) {
        t_lastError = ERROR_INVALID_HANDLE;
        return WAIT_FAILED;
    }
    if (obj->type == ObjType::Process) {
        const int pid = static_cast<ProcessObj*>(obj)->pid;
        const uint64_t deadline = milliseconds == INFINITE ? UINT64_MAX : NowSteadyNs() + uint64_t(milliseconds) * 1000000ull;
        while (MacOs::ProcessAlive(pid)) {
            if (NowSteadyNs() >= deadline) return WAIT_TIMEOUT;
            Sleep(10);
        }
        return WAIT_OBJECT_0;
    }
    if (obj->type == ObjType::Event || obj->type == ObjType::Timer || obj->type == ObjType::Thread) {
        return WaitOn(static_cast<Waitable*>(obj), milliseconds) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
    t_lastError = ERROR_INVALID_HANDLE;
    return WAIT_FAILED;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds) {
    const uint64_t deadline = milliseconds == INFINITE ? UINT64_MAX : NowSteadyNs() + uint64_t(milliseconds) * 1000000ull;
    for (;;) {
        DWORD signaledCount = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) {
                if (!waitAll) return WAIT_OBJECT_0 + i;
                ++signaledCount;
            }
        }
        if (waitAll && signaledCount == count) return WAIT_OBJECT_0;
        if (NowSteadyNs() >= deadline) return WAIT_TIMEOUT;
        Sleep(1);
    }
}

void MemoryBarrier(void) { std::atomic_thread_fence(std::memory_order_seq_cst); }

void YieldProcessor(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause");
#endif
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) {
    std::string value;
    if (!name || !GetEnv(MacWin32::WideToUtf8(name), value)) {
        t_lastError = 203; // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    return CopyOut(MacWin32::Utf8ToWide(value), buffer, size);
}

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) {
    std::string value;
    if (!name || !GetEnv(name, value)) {
        t_lastError = 203;
        return 0;
    }
    return CopyOut(value, buffer, size);
}

BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
    if (!name) return FALSE;
    const std::string n = MacWin32::WideToUtf8(name);
    if (!value) return unsetenv(n.c_str()) == 0;
    return setenv(n.c_str(), MacWin32::WideToUtf8(value).c_str(), 1) == 0;
}

static std::string BuildCommandLine() {
    std::string line;
    for (const std::string& arg : MacOs::CommandLineArguments()) {
        if (!line.empty()) line.push_back(' ');
        if (arg.find_first_of(" \t\"") != std::string::npos) {
            line.push_back('"');
            for (char c : arg) {
                if (c == '"') line.push_back('\\');
                line.push_back(c);
            }
            line.push_back('"');
        } else {
            line += arg;
        }
    }
    return line;
}

LPWSTR GetCommandLineW(void) {
    static std::wstring line = MacWin32::Utf8ToWide(BuildCommandLine());
    return line.data();
}

LPSTR GetCommandLineA(void) {
    static std::string line = BuildCommandLine();
    return line.data();
}

// --- memory -----------------------------------------------------------------------------------

SIZE_T VirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T) { return 0; }

HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes) {
    GlobalObj* g = new GlobalObj();
    g->size = bytes;
    g->data.assign(bytes, 0);
    (void)flags;
    return g;
}

LPVOID GlobalLock(HGLOBAL mem) {
    GlobalObj* g = static_cast<GlobalObj*>(mem);
    return g ? g->data.data() : nullptr;
}

BOOL GlobalUnlock(HGLOBAL) { return TRUE; }

HGLOBAL GlobalFree(HGLOBAL mem) {
    delete static_cast<GlobalObj*>(mem);
    return nullptr;
}

SIZE_T GlobalSize(HGLOBAL mem) {
    GlobalObj* g = static_cast<GlobalObj*>(mem);
    return g ? g->size : 0;
}

// --- modules / resources ------------------------------------------------------------------------

HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name) return reinterpret_cast<HMODULE>(const_cast<mach_header*>(_dyld_get_image_header(0)));
    const std::string wanted = name;
    if (EqualsIgnoreCase(wanted, "Toolscreen.dll")) return MacWin32::ToolscreenModule();
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char* image = _dyld_get_image_name(i);
        if (image && EqualsIgnoreCase(Basename(image), wanted)) {
            return reinterpret_cast<HMODULE>(const_cast<mach_header*>(_dyld_get_image_header(i)));
        }
    }
    t_lastError = 126; // ERROR_MOD_NOT_FOUND
    return nullptr;
}

HMODULE GetModuleHandleW(LPCWSTR name) {
    if (!name) return GetModuleHandleA(nullptr);
    return GetModuleHandleA(MacWin32::WideToUtf8(name).c_str());
}

BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module) {
    if (!module) return FALSE;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        Dl_info info{};
        if (!name || dladdr(name, &info) == 0 || !info.dli_fbase) {
            *module = nullptr;
            t_lastError = 126;
            return FALSE;
        }
        *module = static_cast<HMODULE>(info.dli_fbase);
        return TRUE;
    }
    *module = GetModuleHandleA(name);
    return *module != nullptr;
}

BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(name), module);
    if (!module) return FALSE;
    *module = GetModuleHandleW(name);
    return *module != nullptr;
}

DWORD GetModuleFileNameA(HMODULE module, LPSTR filename, DWORD size) {
    std::string path;
    if (!module) {
        path = MacOs::ProcessPath(getpid());
    } else {
        path = ImagePathForHeader(module);
    }
    if (path.empty() || !filename || size == 0) {
        t_lastError = ERROR_INVALID_HANDLE;
        return 0;
    }
    if (path.size() >= size) {
        std::memcpy(filename, path.c_str(), size - 1);
        filename[size - 1] = '\0';
        t_lastError = ERROR_INSUFFICIENT_BUFFER;
        return size;
    }
    std::memcpy(filename, path.c_str(), path.size() + 1);
    return static_cast<DWORD>(path.size());
}

DWORD GetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    std::string path = !module ? MacOs::ProcessPath(getpid()) : ImagePathForHeader(module);
    if (path.empty() || !filename || size == 0) {
        t_lastError = ERROR_INVALID_HANDLE;
        return 0;
    }
    const std::wstring wide = MacWin32::Utf8ToWide(path);
    if (wide.size() >= size) {
        std::wmemcpy(filename, wide.c_str(), size - 1);
        filename[size - 1] = L'\0';
        t_lastError = ERROR_INSUFFICIENT_BUFFER;
        return size;
    }
    std::wmemcpy(filename, wide.c_str(), wide.size() + 1);
    return static_cast<DWORD>(wide.size());
}

FARPROC GetProcAddress(HMODULE module, LPCSTR name) {
    if (!module || !name || IS_INTRESOURCE(name)) return nullptr;
    const std::string path = ImagePathForHeader(module);
    if (path.empty()) return nullptr;
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) return nullptr;
    void* symbol = dlsym(handle, name);
    dlclose(handle);
    return reinterpret_cast<FARPROC>(symbol);
}

HMODULE LoadLibraryA(LPCSTR) {
    // Windows DLLs never exist here; callers handle the failure.
    t_lastError = 126;
    return nullptr;
}

HMODULE LoadLibraryW(LPCWSTR) {
    t_lastError = 126;
    return nullptr;
}

HMODULE LoadLibraryExW(LPCWSTR, HANDLE, DWORD) {
    t_lastError = 126;
    return nullptr;
}

HMODULE LoadLibraryExA(LPCSTR, HANDLE, DWORD) {
    t_lastError = 126;
    return nullptr;
}

BOOL FreeLibrary(HMODULE) { return TRUE; }

BOOL GetModuleInformation(HANDLE, HMODULE module, LPMODULEINFO info, DWORD size) {
    if (!module || !info || size < sizeof(MODULEINFO)) return FALSE;
    const mach_header_64* header = reinterpret_cast<const mach_header_64*>(module);
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0;
    const uint8_t* cmd = reinterpret_cast<const uint8_t*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const load_command* lc = reinterpret_cast<const load_command*>(cmd);
        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (std::strcmp(seg->segname, "__PAGEZERO") != 0) {
                lowest = std::min<uint64_t>(lowest, seg->vmaddr);
                highest = std::max<uint64_t>(highest, seg->vmaddr + seg->vmsize);
            }
        }
        cmd += lc->cmdsize;
    }
    info->lpBaseOfDll = module;
    info->SizeOfImage = (lowest < highest) ? static_cast<DWORD>(highest - lowest) : 0;
    info->EntryPoint = nullptr;
    return TRUE;
}

BOOL EnumProcessModules(HANDLE, HMODULE* modules, DWORD size, LPDWORD needed) {
    const uint32_t count = _dyld_image_count();
    if (needed) *needed = count * sizeof(HMODULE);
    const DWORD capacity = size / sizeof(HMODULE);
    for (uint32_t i = 0; i < count && i < capacity; ++i) {
        modules[i] = reinterpret_cast<HMODULE>(const_cast<mach_header*>(_dyld_get_image_header(i)));
    }
    return TRUE;
}

HRSRC FindResourceW(HMODULE, LPCWSTR name, LPCWSTR) {
    if (!IS_INTRESOURCE(name)) {
        t_lastError = 1813; // ERROR_RESOURCE_TYPE_NOT_FOUND
        return nullptr;
    }
    const MacEmbeddedResource* r = FindEmbedded(static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)));
    if (!r) t_lastError = 1814; // ERROR_RESOURCE_NAME_NOT_FOUND
    return reinterpret_cast<HRSRC>(const_cast<MacEmbeddedResource*>(r));
}

HRSRC FindResourceA(HMODULE module, LPCSTR name, LPCSTR type) {
    return FindResourceW(module, reinterpret_cast<LPCWSTR>(name), reinterpret_cast<LPCWSTR>(type));
}

HGLOBAL LoadResource(HMODULE, HRSRC resource) { return resource; }

LPVOID LockResource(HGLOBAL resData) {
    const MacEmbeddedResource* r = static_cast<const MacEmbeddedResource*>(resData);
    return r ? const_cast<unsigned char*>(r->data) : nullptr;
}

DWORD SizeofResource(HMODULE, HRSRC resource) {
    const MacEmbeddedResource* r = reinterpret_cast<const MacEmbeddedResource*>(resource);
    return r ? r->size : 0;
}

BOOL FreeResource(HGLOBAL) { return TRUE; }

DWORD GetFileVersionInfoSizeW(LPCWSTR, LPDWORD handle) {
    if (handle) *handle = 0;
    return 0;
}

BOOL GetFileVersionInfoW(LPCWSTR, DWORD, DWORD, LPVOID) { return FALSE; }
BOOL VerQueryValueW(LPCVOID, LPCWSTR, LPVOID*, PUINT) { return FALSE; }

// --- strings / locale -----------------------------------------------------------------------------

int MultiByteToWideChar(UINT, DWORD flags, LPCSTR multiByte, int multiByteCount, LPWSTR wide, int wideCount) {
    if (!multiByte) {
        t_lastError = ERROR_INVALID_PARAMETER;
        return 0;
    }
    const size_t length = multiByteCount < 0 ? std::strlen(multiByte) + 1 : static_cast<size_t>(multiByteCount);
    std::wstring converted;
    if (!MacWin32::Utf8ToWide(multiByte, length, converted, (flags & MB_ERR_INVALID_CHARS) != 0)) {
        t_lastError = 1113; // ERROR_NO_UNICODE_TRANSLATION
        return 0;
    }
    if (wideCount == 0) return static_cast<int>(converted.size());
    if (static_cast<size_t>(wideCount) < converted.size()) {
        t_lastError = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    std::wmemcpy(wide, converted.data(), converted.size());
    return static_cast<int>(converted.size());
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR wide, int wideCount, LPSTR multiByte, int multiByteCount, LPCSTR, LPBOOL usedDefaultChar) {
    if (!wide) {
        t_lastError = ERROR_INVALID_PARAMETER;
        return 0;
    }
    const size_t length = wideCount < 0 ? std::wcslen(wide) + 1 : static_cast<size_t>(wideCount);
    const std::string converted = MacWin32::WideToUtf8(std::wstring(wide, length));
    if (usedDefaultChar) *usedDefaultChar = FALSE;
    if (multiByteCount == 0) return static_cast<int>(converted.size());
    if (static_cast<size_t>(multiByteCount) < converted.size()) {
        t_lastError = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    std::memcpy(multiByte, converted.data(), converted.size());
    return static_cast<int>(converted.size());
}

BOOL IsDBCSLeadByte(BYTE) { return FALSE; }

int GetLocaleInfoA(LCID, DWORD type, LPSTR data, int dataSize) {
    if ((type & ~LOCALE_RETURN_NUMBER) == LOCALE_IDEFAULTANSICODEPAGE) {
        if (type & LOCALE_RETURN_NUMBER) {
            if (data && dataSize >= static_cast<int>(sizeof(DWORD))) *reinterpret_cast<DWORD*>(data) = CP_UTF8;
            return sizeof(DWORD) / sizeof(char);
        }
        const char* text = "65001";
        if (data && dataSize > 5) std::memcpy(data, text, 6);
        return 6;
    }
    return 0;
}

LCID GetUserDefaultLCID(void) { return 0x0409; }
LANGID GetUserDefaultUILanguage(void) { return 0x0409; }

// --- files ---------------------------------------------------------------------------------------------

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags, HANDLE) {
    const std::string path = NarrowPath(name);
    int oflags = O_CLOEXEC;
    const bool read = (access & GENERIC_READ) != 0;
    const bool write = (access & GENERIC_WRITE) != 0;
    oflags |= (read && write) ? O_RDWR : (write ? O_WRONLY : O_RDONLY);

    struct stat st {};
    const bool existed = stat(path.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
        t_lastError = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    switch (disposition) {
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: t_lastError = ERROR_INVALID_PARAMETER; return INVALID_HANDLE_VALUE;
    }
    if (existed && S_ISDIR(st.st_mode)) oflags = O_RDONLY | O_CLOEXEC;

    const int fd = open(path.c_str(), oflags, 0644);
    if (fd < 0) {
        SetErrno(errno);
        return INVALID_HANDLE_VALUE;
    }
    FileObj* f = new FileObj();
    f->fd = fd;
    f->path = path;
    t_lastError = (existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS;
    return f;
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templateFile) {
    return CreateFileA(NarrowPath(name).c_str(), access, share, sa, disposition, flags, templateFile);
}

BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD toRead, LPDWORD read, LPOVERLAPPED) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    if (!f) {
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    size_t total = 0;
    while (total < toRead) {
        const ssize_t n = ::read(f->fd, static_cast<uint8_t*>(buffer) + total, toRead - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            SetErrno(errno);
            if (read) *read = static_cast<DWORD>(total);
            return FALSE;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    if (read) *read = static_cast<DWORD>(total);
    return TRUE;
}

BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD toWrite, LPDWORD written, LPOVERLAPPED) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    if (!f) {
        t_lastError = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    size_t total = 0;
    while (total < toWrite) {
        const ssize_t n = ::write(f->fd, static_cast<const uint8_t*>(buffer) + total, toWrite - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            SetErrno(errno);
            if (written) *written = static_cast<DWORD>(total);
            return FALSE;
        }
        total += static_cast<size_t>(n);
    }
    if (written) *written = static_cast<DWORD>(total);
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE file) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    return f && fsync(f->fd) == 0;
}

BOOL SetEndOfFile(HANDLE file) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    if (!f) return FALSE;
    const off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return pos >= 0 && ftruncate(f->fd, pos) == 0;
}

static int Whence(DWORD method) { return method == FILE_CURRENT ? SEEK_CUR : (method == FILE_END ? SEEK_END : SEEK_SET); }

DWORD SetFilePointer(HANDLE file, LONG distance, PLONG distanceHigh, DWORD method) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    if (!f) return INVALID_FILE_SIZE;
    int64_t offset = distance;
    if (distanceHigh) offset = (static_cast<int64_t>(*distanceHigh) << 32) | static_cast<uint32_t>(distance);
    const off_t pos = lseek(f->fd, offset, Whence(method));
    if (pos < 0) {
        SetErrno(errno);
        return INVALID_FILE_SIZE;
    }
    if (distanceHigh) *distanceHigh = static_cast<LONG>(pos >> 32);
    return static_cast<DWORD>(pos & 0xFFFFFFFF);
}

BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER newPointer, DWORD method) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    if (!f) return FALSE;
    const off_t pos = lseek(f->fd, distance.QuadPart, Whence(method));
    if (pos < 0) {
        SetErrno(errno);
        return FALSE;
    }
    if (newPointer) newPointer->QuadPart = pos;
    return TRUE;
}

DWORD GetFileSize(HANDLE file, LPDWORD high) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    struct stat st {};
    if (!f || fstat(f->fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    return static_cast<DWORD>(static_cast<uint64_t>(st.st_size) & 0xFFFFFFFF);
}

BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    struct stat st {};
    if (!f || !size || fstat(f->fd, &st) != 0) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL GetFileTime(HANDLE file, LPFILETIME creation, LPFILETIME access, LPFILETIME write) {
    FileObj* f = As<FileObj>(file, ObjType::File);
    struct stat st {};
    if (!f || fstat(f->fd, &st) != 0) return FALSE;
    if (creation) *creation = TimespecToFileTime(st.st_birthtimespec);
    if (access) *access = TimespecToFileTime(st.st_atimespec);
    if (write) *write = TimespecToFileTime(st.st_mtimespec);
    return TRUE;
}

static DWORD AttributesFor(const std::string& path, const struct stat& st) {
    DWORD attributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    const std::string base = Basename(path);
    if (!base.empty() && base[0] == '.') attributes |= FILE_ATTRIBUTE_HIDDEN;
    if (access(path.c_str(), W_OK) != 0 && !S_ISDIR(st.st_mode)) attributes |= FILE_ATTRIBUTE_READONLY;
    return attributes;
}

DWORD GetFileAttributesA(LPCSTR name) {
    const std::string path = NarrowPath(name);
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        SetErrno(errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    return AttributesFor(path, st);
}

DWORD GetFileAttributesW(LPCWSTR name) { return GetFileAttributesA(NarrowPath(name).c_str()); }

BOOL GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS, LPVOID info) {
    const std::string path = NarrowPath(name);
    struct stat st {};
    if (!info || stat(path.c_str(), &st) != 0) {
        SetErrno(errno);
        return FALSE;
    }
    WIN32_FILE_ATTRIBUTE_DATA* data = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    data->dwFileAttributes = AttributesFor(path, st);
    data->ftCreationTime = TimespecToFileTime(st.st_birthtimespec);
    data->ftLastAccessTime = TimespecToFileTime(st.st_atimespec);
    data->ftLastWriteTime = TimespecToFileTime(st.st_mtimespec);
    data->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    data->nFileSizeLow = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) & 0xFFFFFFFF);
    return TRUE;
}

BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    return GetFileAttributesExA(NarrowPath(name).c_str(), level, info);
}

BOOL SetFileAttributesW(LPCWSTR, DWORD) { return TRUE; }

BOOL DeleteFileA(LPCSTR name) {
    if (unlink(NarrowPath(name).c_str()) != 0) {
        SetErrno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR name) { return DeleteFileA(NarrowPath(name).c_str()); }

static BOOL CopyPath(const std::string& from, const std::string& to, bool failIfExists) {
    struct stat st {};
    if (failIfExists && stat(to.c_str(), &st) == 0) {
        t_lastError = ERROR_FILE_EXISTS;
        return FALSE;
    }
    if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_ALL) != 0) {
        SetErrno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    const std::string src = NarrowPath(from);
    if (!to) {
        // Delete-on-reboot semantics are not supported; delete now.
        return unlink(src.c_str()) == 0;
    }
    const std::string dst = NarrowPath(to);
    struct stat st {};
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(dst.c_str(), &st) == 0) {
        t_lastError = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    if (rename(src.c_str(), dst.c_str()) == 0) {
        if (flags & MOVEFILE_WRITE_THROUGH) {
            const int fd = open(dst.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                fsync(fd);
                close(fd);
            }
        }
        return TRUE;
    }
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
        if (!CopyPath(src, dst, !(flags & MOVEFILE_REPLACE_EXISTING))) return FALSE;
        unlink(src.c_str());
        return TRUE;
    }
    SetErrno(errno);
    return FALSE;
}

BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED); }

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) { return CopyPath(NarrowPath(from), NarrowPath(to), failIfExists != FALSE); }

BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES) {
    if (mkdir(NarrowPath(path).c_str(), 0755) != 0) {
        t_lastError = (errno == EEXIST) ? ERROR_ALREADY_EXISTS : ErrnoToWin32(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa) { return CreateDirectoryA(NarrowPath(path).c_str(), sa); }

BOOL RemoveDirectoryW(LPCWSTR path) {
    if (rmdir(NarrowPath(path).c_str()) != 0) {
        SetErrno(errno);
        return FALSE;
    }
    return TRUE;
}

DWORD GetTempPathW(DWORD size, LPWSTR buffer) { return CopyOut(MacWin32::Utf8ToWide(MacOs::TemporaryDirectory()), buffer, size); }

DWORD GetTempPathA(DWORD size, LPSTR buffer) { return CopyOut(MacOs::TemporaryDirectory(), buffer, size); }

DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        SetErrno(errno);
        return 0;
    }
    return CopyOut(std::string(cwd), buffer, size);
}

DWORD GetCurrentDirectoryW(DWORD size, LPWSTR buffer) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        SetErrno(errno);
        return 0;
    }
    return CopyOut(MacWin32::Utf8ToWide(cwd), buffer, size);
}

UINT GetWindowsDirectoryW(LPWSTR buffer, UINT size) {
    // Callers append "\Fonts" to find system fonts.
    return CopyOut(std::wstring(L"/System/Library"), buffer, size);
}

DWORD GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buffer, LPWSTR* filePart) {
    std::string path = NarrowPath(name);
    if (path.empty() || path[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) path = std::string(cwd) + "/" + path;
    }
    const std::wstring wide = MacWin32::Utf8ToWide(path);
    const DWORD result = CopyOut(wide, buffer, size);
    if (filePart && buffer && result < size) {
        const size_t slash = wide.find_last_of(L'/');
        *filePart = buffer + (slash == std::wstring::npos ? 0 : slash + 1);
    }
    return result;
}

HANDLE CreateFileMappingW(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR) {
    t_lastError = ERROR_ACCESS_DENIED;
    return nullptr;
}

HANDLE OpenFileMappingW(DWORD, BOOL, LPCWSTR) {
    t_lastError = ERROR_FILE_NOT_FOUND;
    return nullptr;
}

LPVOID MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T) { return nullptr; }
BOOL UnmapViewOfFile(LPCVOID) { return FALSE; }

// --- shlwapi / shell32 / ole32 -----------------------------------------------------------------------------

BOOL PathIsRelativeA(LPCSTR path) {
    if (!path || !*path) return TRUE;
    if (path[0] == '/' || path[0] == '\\' || path[0] == '~') return FALSE;
    if (std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') return FALSE;
    return TRUE;
}

BOOL PathIsRelativeW(LPCWSTR path) { return PathIsRelativeA(MacWin32::WideToUtf8(path ? path : L"").c_str()); }

BOOL PathFileExistsW(LPCWSTR path) {
    struct stat st {};
    return stat(NarrowPath(path).c_str(), &st) == 0;
}

HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID rfid, DWORD, HANDLE, PWSTR* path) {
    if (!path) return E_INVALIDARG;
    *path = nullptr;
    const std::string folder = KnownFolder(rfid.Data1);
    if (folder.empty()) return E_FAIL;
    const std::wstring wide = MacWin32::Utf8ToWide(folder);
    wchar_t* copy = static_cast<wchar_t*>(malloc((wide.size() + 1) * sizeof(wchar_t)));
    if (!copy) return E_OUTOFMEMORY;
    std::wmemcpy(copy, wide.c_str(), wide.size() + 1);
    *path = copy;
    return S_OK;
}

HRESULT SHGetFolderPathW(HWND, int csidl, HANDLE, DWORD, LPWSTR path) {
    std::string folder;
    switch (csidl & 0xFF) {
    case CSIDL_APPDATA:
    case CSIDL_LOCAL_APPDATA: folder = MacOs::ApplicationSupportDirectory(); break;
    case CSIDL_PROFILE: folder = MacOs::HomeDirectory(); break;
    case CSIDL_PERSONAL: folder = MacOs::DocumentsDirectory(); break;
    case CSIDL_DESKTOPDIRECTORY: folder = MacOs::DesktopDirectory(); break;
    case CSIDL_FONTS: folder = "/System/Library/Fonts"; break;
    default: return E_FAIL;
    }
    if (!path) return E_INVALIDARG;
    const std::wstring wide = MacWin32::Utf8ToWide(folder);
    const size_t n = std::min<size_t>(wide.size(), MAX_PATH - 1);
    std::wmemcpy(path, wide.c_str(), n);
    path[n] = L'\0';
    return S_OK;
}

int SHCreateDirectoryExW(HWND, LPCWSTR path, const SECURITY_ATTRIBUTES*) {
    const std::string full = NarrowPath(path);
    struct stat st {};
    if (stat(full.c_str(), &st) == 0) return S_ISDIR(st.st_mode) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
    std::string current;
    size_t pos = 0;
    while (pos != std::string::npos) {
        pos = full.find('/', pos + 1);
        current = full.substr(0, pos);
        if (current.empty()) continue;
        if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return static_cast<int>(ErrnoToWin32(errno));
    }
    return ERROR_SUCCESS;
}

void CoTaskMemFree(LPVOID pv) { free(pv); }
HRESULT CoInitializeEx(LPVOID, DWORD) { return S_OK; }
HRESULT CoInitialize(LPVOID) { return S_OK; }
void CoUninitialize(void) {}

HINSTANCE ShellExecuteW(HWND, LPCWSTR, LPCWSTR file, LPCWSTR, LPCWSTR, INT) {
    if (!file) return reinterpret_cast<HINSTANCE>(static_cast<intptr_t>(SE_ERR_NOASSOC));
    std::string target = MacWin32::WideToUtf8(file);
    if (target.find("://") == std::string::npos) {
        for (char& c : target) {
            if (c == '\\') c = '/';
        }
    }
    return reinterpret_cast<HINSTANCE>(static_cast<intptr_t>(MacOs::OpenUrl(target) ? 42 : SE_ERR_NOASSOC));
}

HINSTANCE ShellExecuteA(HWND hwnd, LPCSTR operation, LPCSTR file, LPCSTR parameters, LPCSTR directory, INT showCmd) {
    const std::wstring wide = MacWin32::Utf8ToWide(file ? file : "");
    (void)operation;
    (void)parameters;
    (void)directory;
    return ShellExecuteW(hwnd, nullptr, wide.c_str(), nullptr, nullptr, showCmd);
}

// --- dbghelp --------------------------------------------------------------------------------------------

DWORD SymGetOptions(void) { return 0; }
DWORD SymSetOptions(DWORD options) { return options; }
BOOL SymInitialize(HANDLE, PCSTR, BOOL) { return TRUE; }
BOOL SymCleanup(HANDLE) { return TRUE; }

BOOL SymFromAddr(HANDLE, DWORD64 address, PDWORD64 displacement, PSYMBOL_INFO symbol) {
    Dl_info info{};
    if (!symbol || dladdr(reinterpret_cast<void*>(address), &info) == 0 || !info.dli_sname) return FALSE;
    const size_t max = symbol->MaxNameLen > 0 ? symbol->MaxNameLen : 1;
    std::strncpy(symbol->Name, info.dli_sname, max - 1);
    symbol->Name[max - 1] = '\0';
    symbol->NameLen = static_cast<ULONG>(std::strlen(symbol->Name));
    symbol->Address = reinterpret_cast<uintptr_t>(info.dli_saddr);
    if (displacement) *displacement = address - symbol->Address;
    return TRUE;
}

BOOL SymGetLineFromAddr64(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64) { return FALSE; }
DWORD64 SymLoadModuleEx(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64 base, DWORD, PVOID, DWORD) { return base; }
BOOL SymRefreshModuleList(HANDLE) { return TRUE; }
BOOL SymSetSearchPath(HANDLE, PCSTR) { return TRUE; }

// --- registry -------------------------------------------------------------------------------------------

LONG RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, HKEY* result) {
    if (result) *result = nullptr;
    return ERROR_FILE_NOT_FOUND;
}

LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }

// --- MSVC CRT ----------------------------------------------------------------------------------------------

int sprintf_s(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int r = vsnprintf(buffer, size, format, args);
    va_end(args);
    return r;
}

int swprintf_s(wchar_t* buffer, size_t size, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int r = vswprintf(buffer, size, format, args);
    va_end(args);
    return r;
}

int vsprintf_s(char* buffer, size_t size, const char* format, va_list args) { return vsnprintf(buffer, size, format, args); }

errno_t strcpy_s(char* dest, size_t size, const char* src) {
    if (!dest || size == 0) return EINVAL;
    if (!src) {
        dest[0] = '\0';
        return EINVAL;
    }
    const size_t n = std::strlen(src);
    if (n >= size) {
        dest[0] = '\0';
        return ERANGE;
    }
    std::memcpy(dest, src, n + 1);
    return 0;
}

errno_t strncpy_s(char* dest, size_t size, const char* src, size_t count) {
    if (!dest || size == 0) return EINVAL;
    if (!src) {
        dest[0] = '\0';
        return EINVAL;
    }
    size_t n = strnlen(src, count == _TRUNCATE ? size - 1 : count);
    if (n >= size) {
        if (count != _TRUNCATE) {
            dest[0] = '\0';
            return ERANGE;
        }
        n = size - 1;
    }
    std::memcpy(dest, src, n);
    dest[n] = '\0';
    return 0;
}

errno_t strcat_s(char* dest, size_t size, const char* src) {
    if (!dest || !src) return EINVAL;
    const size_t existing = strnlen(dest, size);
    if (existing >= size) return EINVAL;
    return strcpy_s(dest + existing, size - existing, src);
}

errno_t wcscpy_s(wchar_t* dest, size_t size, const wchar_t* src) {
    if (!dest || size == 0) return EINVAL;
    if (!src) {
        dest[0] = L'\0';
        return EINVAL;
    }
    const size_t n = std::wcslen(src);
    if (n >= size) {
        dest[0] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(dest, src, n + 1);
    return 0;
}

errno_t wcsncpy_s(wchar_t* dest, size_t size, const wchar_t* src, size_t count) {
    if (!dest || size == 0) return EINVAL;
    if (!src) {
        dest[0] = L'\0';
        return EINVAL;
    }
    size_t n = wcsnlen(src, count == _TRUNCATE ? size - 1 : count);
    if (n >= size) {
        if (count != _TRUNCATE) {
            dest[0] = L'\0';
            return ERANGE;
        }
        n = size - 1;
    }
    std::wmemcpy(dest, src, n);
    dest[n] = L'\0';
    return 0;
}

errno_t wcscat_s(wchar_t* dest, size_t size, const wchar_t* src) {
    if (!dest || !src) return EINVAL;
    const size_t existing = wcsnlen(dest, size);
    if (existing >= size) return EINVAL;
    return wcscpy_s(dest + existing, size - existing, src);
}

size_t strnlen_s(const char* str, size_t size) { return str ? strnlen(str, size) : 0; }
size_t wcsnlen_s(const wchar_t* str, size_t size) { return str ? wcsnlen(str, size) : 0; }

errno_t localtime_s(struct tm* result, const time_t* time) { return (result && time && localtime_r(time, result)) ? 0 : EINVAL; }
errno_t gmtime_s(struct tm* result, const time_t* time) { return (result && time && gmtime_r(time, result)) ? 0 : EINVAL; }

errno_t getenv_s(size_t* required, char* buffer, size_t size, const char* name) {
    std::string value;
    if (!name || !GetEnv(name, value)) {
        if (required) *required = 0;
        if (buffer && size > 0) buffer[0] = '\0';
        return 0;
    }
    if (required) *required = value.size() + 1;
    if (!buffer || size == 0) return 0;
    if (size <= value.size()) {
        buffer[0] = '\0';
        return ERANGE;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return 0;
}

errno_t _wgetenv_s(size_t* required, wchar_t* buffer, size_t size, const wchar_t* name) {
    std::string value;
    if (!name || !GetEnv(MacWin32::WideToUtf8(name), value)) {
        if (required) *required = 0;
        if (buffer && size > 0) buffer[0] = L'\0';
        return 0;
    }
    const std::wstring wide = MacWin32::Utf8ToWide(value);
    if (required) *required = wide.size() + 1;
    if (!buffer || size == 0) return 0;
    if (size <= wide.size()) {
        buffer[0] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(buffer, wide.c_str(), wide.size() + 1);
    return 0;
}

errno_t _dupenv_s(char** buffer, size_t* size, const char* name) {
    std::string value;
    if (!buffer) return EINVAL;
    *buffer = nullptr;
    if (size) *size = 0;
    if (!name || !GetEnv(name, value)) return 0;
    *buffer = strdup(value.c_str());
    if (size) *size = value.size() + 1;
    return 0;
}

errno_t fopen_s(FILE** file, const char* name, const char* mode) {
    if (!file) return EINVAL;
    *file = fopen(NarrowPath(name).c_str(), mode);
    return *file ? 0 : errno;
}

FILE* _wfopen(const wchar_t* name, const wchar_t* mode) {
    std::string m = MacWin32::WideToUtf8(mode ? mode : L"r");
    // Drop MSVC-only mode flags (ccs=UTF-8, N, T, D, ...).
    const size_t comma = m.find(',');
    if (comma != std::string::npos) m.erase(comma);
    std::string clean;
    for (char c : m) {
        if (c == 'r' || c == 'w' || c == 'a' || c == '+' || c == 'b' || c == 'x') clean.push_back(c);
    }
    return fopen(NarrowPath(name).c_str(), clean.empty() ? "r" : clean.c_str());
}

errno_t _wfopen_s(FILE** file, const wchar_t* name, const wchar_t* mode) {
    if (!file) return EINVAL;
    *file = _wfopen(name, mode);
    return *file ? 0 : errno;
}

int _wremove(const wchar_t* name) { return remove(NarrowPath(name).c_str()); }
int _wrename(const wchar_t* from, const wchar_t* to) { return rename(NarrowPath(from).c_str(), NarrowPath(to).c_str()); }

} // extern "C"

const GUID FOLDERID_Downloads = { 0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B } };
const GUID FOLDERID_Documents = { 0xFDD39AD0, 0x238F, 0x46AF, { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } };
const GUID FOLDERID_Desktop = { 0xB4BFCC3A, 0xDB2C, 0x424C, { 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41 } };
const GUID FOLDERID_Pictures = { 0x33E28130, 0x4E1E, 0x4676, { 0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB } };
const GUID FOLDERID_RoamingAppData = { 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } };
const GUID FOLDERID_LocalAppData = { 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };
const GUID FOLDERID_Profile = { 0x5E6C858F, 0x0E22, 0x4760, { 0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73 } };
const GUID FOLDERID_Fonts = { 0xFD228CB7, 0xAE11, 0x4AE3, { 0x86, 0x4C, 0x16, 0xF3, 0x91, 0x0A, 0xB8, 0xFE } };
