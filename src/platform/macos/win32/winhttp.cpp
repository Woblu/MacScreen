// WinHTTP subset on top of NSURLSession (MacOs::HttpRequest). Requests run
// synchronously when sent; the response is then read back through the usual
// WinHttpQueryHeaders / WinHttpReadData calls.

#include "platform/macos/win32/win32.h"

#include "platform/macos/os/mac_os.h"
#include "platform/macos/win32/win32_internal.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

enum class HttpType : uint32_t { Session = 1, Connection, Request };

struct HttpObject {
    static constexpr uint32_t kMagic = 0x54534854; // 'TSHT'
    uint32_t magic = kMagic;
    HttpType type;
    explicit HttpObject(HttpType t) : type(t) {}
    virtual ~HttpObject() { magic = 0; }
};

struct Session : HttpObject {
    std::string agent;
    int timeoutMs = 30000;
    Session() : HttpObject(HttpType::Session) {}
};

struct Connection : HttpObject {
    Session* session = nullptr;
    std::string host;
    INTERNET_PORT port = 0;
    Connection() : HttpObject(HttpType::Connection) {}
};

struct Request : HttpObject {
    Connection* connection = nullptr;
    std::string verb;
    std::string path;
    bool secure = false;
    int timeoutMs = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    MacOs::HttpResponse response;
    bool completed = false;
    size_t readOffset = 0;
    Request() : HttpObject(HttpType::Request) {}
};

template <typename T> T* As(HINTERNET h, HttpType type) {
    if (!h) return nullptr;
    HttpObject* obj = static_cast<HttpObject*>(h);
    return (obj->magic == HttpObject::kMagic && obj->type == type) ? static_cast<T*>(obj) : nullptr;
}

void AddHeaders(Request* r, const std::string& block) {
    size_t start = 0;
    while (start < block.size()) {
        size_t end = block.find("\r\n", start);
        if (end == std::string::npos) end = block.size();
        const std::string line = block.substr(start, end - start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            r->headers.emplace_back(name, value);
        }
        start = end + 2;
    }
}

std::string HeaderValue(const Request* r, const char* name) {
    for (const auto& h : r->response.headers) {
        if (strcasecmp(h.first.c_str(), name) == 0) return h.second;
    }
    return {};
}

std::string WideArg(LPCWSTR s, DWORD length) {
    if (!s) return {};
    if (length == static_cast<DWORD>(-1) || length == 0) return MacWin32::WideToUtf8(s);
    return MacWin32::WideToUtf8(std::wstring(s, length));
}

} // namespace

extern "C" {

HINTERNET WinHttpOpen(LPCWSTR agent, DWORD, LPCWSTR, LPCWSTR, DWORD) {
    Session* s = new Session();
    s->agent = MacWin32::WideToUtf8(agent ? agent : L"");
    return s;
}

BOOL WinHttpSetTimeouts(HINTERNET handle, int resolveTimeout, int connectTimeout, int sendTimeout, int receiveTimeout) {
    const int total = std::max(0, resolveTimeout) + std::max(0, connectTimeout) + std::max(sendTimeout, receiveTimeout);
    if (Session* s = As<Session>(handle, HttpType::Session)) {
        s->timeoutMs = total > 0 ? total : 30000;
        return TRUE;
    }
    if (Request* r = As<Request>(handle, HttpType::Request)) {
        r->timeoutMs = total;
        return TRUE;
    }
    return FALSE;
}

BOOL WinHttpSetOption(HINTERNET, DWORD, LPVOID, DWORD) { return TRUE; }

HINTERNET WinHttpConnect(HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD) {
    Session* s = As<Session>(session, HttpType::Session);
    if (!s || !server) return nullptr;
    Connection* c = new Connection();
    c->session = s;
    c->host = MacWin32::WideToUtf8(server);
    c->port = port;
    return c;
}

HINTERNET WinHttpOpenRequest(HINTERNET connect, LPCWSTR verb, LPCWSTR objectName, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD flags) {
    Connection* c = As<Connection>(connect, HttpType::Connection);
    if (!c) return nullptr;
    Request* r = new Request();
    r->connection = c;
    r->verb = verb ? MacWin32::WideToUtf8(verb) : std::string("GET");
    r->path = objectName ? MacWin32::WideToUtf8(objectName) : std::string("/");
    if (r->path.empty() || r->path[0] != '/') r->path.insert(r->path.begin(), '/');
    r->secure = (flags & WINHTTP_FLAG_SECURE) != 0;
    return r;
}

BOOL WinHttpAddRequestHeaders(HINTERNET request, LPCWSTR headers, DWORD headersLength, DWORD) {
    Request* r = As<Request>(request, HttpType::Request);
    if (!r || !headers) return FALSE;
    AddHeaders(r, WideArg(headers, headersLength));
    return TRUE;
}

BOOL WinHttpSendRequest(HINTERNET request, LPCWSTR headers, DWORD headersLength, LPVOID optional, DWORD optionalLength, DWORD, DWORD_PTR) {
    Request* r = As<Request>(request, HttpType::Request);
    if (!r) return FALSE;
    if (headers) AddHeaders(r, WideArg(headers, headersLength));
    if (!r->connection->session->agent.empty()) r->headers.emplace_back("User-Agent", r->connection->session->agent);

    std::string url = (r->secure ? "https://" : "http://") + r->connection->host;
    const INTERNET_PORT defaultPort = r->secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (r->connection->port != 0 && r->connection->port != defaultPort) url += ":" + std::to_string(r->connection->port);
    url += r->path;

    std::vector<uint8_t> body;
    if (optional && optionalLength > 0) {
        const uint8_t* bytes = static_cast<const uint8_t*>(optional);
        body.assign(bytes, bytes + optionalLength);
    }
    const int timeout = r->timeoutMs > 0 ? r->timeoutMs : r->connection->session->timeoutMs;
    r->completed = MacOs::HttpRequest(r->verb, url, r->headers, body, timeout, r->response);
    if (!r->completed) SetLastError(12002); // ERROR_WINHTTP_TIMEOUT / generic failure
    return r->completed ? TRUE : FALSE;
}

BOOL WinHttpReceiveResponse(HINTERNET request, LPVOID) {
    Request* r = As<Request>(request, HttpType::Request);
    return (r && r->completed) ? TRUE : FALSE;
}

BOOL WinHttpQueryHeaders(HINTERNET request, DWORD infoLevel, LPCWSTR, LPVOID buffer, LPDWORD bufferLength, LPDWORD) {
    Request* r = As<Request>(request, HttpType::Request);
    if (!r || !r->completed || !bufferLength) return FALSE;
    const DWORD level = infoLevel & 0xFFFF;
    std::string value;
    if (level == WINHTTP_QUERY_STATUS_CODE) {
        value = std::to_string(r->response.status);
    } else if (level == WINHTTP_QUERY_CONTENT_LENGTH) {
        value = HeaderValue(r, "Content-Length");
        if (value.empty()) value = std::to_string(r->response.body.size());
    } else if (level == WINHTTP_QUERY_CONTENT_TYPE) {
        value = HeaderValue(r, "Content-Type");
    } else {
        SetLastError(12150); // ERROR_WINHTTP_HEADER_NOT_FOUND
        return FALSE;
    }
    if (infoLevel & WINHTTP_QUERY_FLAG_NUMBER) {
        if (*bufferLength < sizeof(DWORD) || !buffer) {
            *bufferLength = sizeof(DWORD);
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *static_cast<DWORD*>(buffer) = static_cast<DWORD>(std::strtoul(value.c_str(), nullptr, 10));
        *bufferLength = sizeof(DWORD);
        return TRUE;
    }
    const std::wstring wide = MacWin32::Utf8ToWide(value);
    const DWORD needed = static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t));
    if (!buffer || *bufferLength < needed) {
        *bufferLength = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::wmemcpy(static_cast<wchar_t*>(buffer), wide.c_str(), wide.size() + 1);
    *bufferLength = needed - sizeof(wchar_t);
    return TRUE;
}

BOOL WinHttpQueryDataAvailable(HINTERNET request, LPDWORD available) {
    Request* r = As<Request>(request, HttpType::Request);
    if (!r || !r->completed) return FALSE;
    if (available) *available = static_cast<DWORD>(r->response.body.size() - r->readOffset);
    return TRUE;
}

BOOL WinHttpReadData(HINTERNET request, LPVOID buffer, DWORD toRead, LPDWORD read) {
    Request* r = As<Request>(request, HttpType::Request);
    if (!r || !r->completed) return FALSE;
    const size_t remaining = r->response.body.size() - r->readOffset;
    const size_t n = std::min<size_t>(remaining, toRead);
    if (n > 0 && buffer) std::memcpy(buffer, r->response.body.data() + r->readOffset, n);
    r->readOffset += n;
    if (read) *read = static_cast<DWORD>(n);
    return TRUE;
}

BOOL WinHttpCloseHandle(HINTERNET handle) {
    if (!handle) return FALSE;
    HttpObject* obj = static_cast<HttpObject*>(handle);
    if (obj->magic != HttpObject::kMagic) return FALSE;
    delete obj;
    return TRUE;
}

BOOL WinHttpCrackUrl(LPCWSTR url, DWORD urlLength, DWORD, LPURL_COMPONENTS components) {
    if (!url || !components) return FALSE;
    const size_t length = (urlLength == 0 || urlLength == static_cast<DWORD>(-1)) ? std::wcslen(url) : urlLength;
    const std::wstring s(url, length);
    const size_t schemeEnd = s.find(L"://");
    if (schemeEnd == std::wstring::npos) {
        SetLastError(12006); // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
        return FALSE;
    }
    const std::wstring scheme = s.substr(0, schemeEnd);
    const size_t hostStart = schemeEnd + 3;
    size_t pathStart = s.find(L'/', hostStart);
    if (pathStart == std::wstring::npos) pathStart = s.size();
    std::wstring hostPort = s.substr(hostStart, pathStart - hostStart);
    INTERNET_PORT port = 0;
    const size_t colon = hostPort.find(L':');
    size_t hostLength = hostPort.size();
    if (colon != std::wstring::npos) {
        port = static_cast<INTERNET_PORT>(std::wcstoul(hostPort.c_str() + colon + 1, nullptr, 10));
        hostLength = colon;
    }
    const bool https = scheme == L"https";
    components->nScheme = https ? INTERNET_SCHEME_HTTPS : INTERNET_SCHEME_HTTP;
    components->nPort = port ? port : (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

    size_t queryStart = s.find(L'?', pathStart);
    if (queryStart == std::wstring::npos) queryStart = s.size();

    auto assign = [&](LPWSTR& ptr, DWORD& len, size_t start, size_t count) {
        if (ptr && len > 0) {
            const size_t n = std::min<size_t>(count, len - 1);
            std::wmemcpy(ptr, url + start, n);
            ptr[n] = L'\0';
            len = static_cast<DWORD>(n);
        } else if (len != 0) {
            ptr = const_cast<LPWSTR>(url + start);
            len = static_cast<DWORD>(count);
        }
    };
    assign(components->lpszScheme, components->dwSchemeLength, 0, schemeEnd);
    assign(components->lpszHostName, components->dwHostNameLength, hostStart, hostLength);
    assign(components->lpszUrlPath, components->dwUrlPathLength, pathStart, queryStart - pathStart);
    assign(components->lpszExtraInfo, components->dwExtraInfoLength, queryStart, s.size() - queryStart);
    if (components->dwUserNameLength) components->dwUserNameLength = 0;
    if (components->dwPasswordLength) components->dwPasswordLength = 0;
    return TRUE;
}

} // extern "C"
