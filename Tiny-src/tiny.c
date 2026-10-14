#define WIN32_LEAN_AND_MEAN
#define JSMN_STATIC
#define JSMN_STRICT
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "third_party/jsmn.h"

#define WM_QUOTA (WM_APP + 1)
#define WM_TRAY (WM_APP + 2)
#define REFRESH_MS 60000
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    int remaining;
    int64_t resets_at;
} QuotaWindow;

typedef struct {
    QuotaWindow week, hour;
    int credits;
    WCHAR error[80];
} Quota;

typedef struct {
    const char *text;
    jsmntok_t tokens[2048];
    int count;
} Json;

static HWND app_window;
static HANDLE stop_event, worker;
static Quota pending;
static NOTIFYICONDATAW tray;
static UINT taskbar_created;
static const WCHAR run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

/* 初始化未知值：缺失数据与真实的 0 额度、0 次数保持区分。 */
static Quota empty_quota(void) {
    return (Quota){.week = {-1, -1}, .hour = {-1, -1}, .credits = -1};
}

/* 只遍历当前对象的直接字段，跳过整个子树，避免混入其他额度桶。 */
static int member(const Json *json, int object, const char *key) {
    if (object < 0 || json->tokens[object].type != JSMN_OBJECT) return -1;
    for (int i = object + 1; i + 1 < json->count && json->tokens[i].start < json->tokens[object].end;) {
        const jsmntok_t *token = &json->tokens[i];
        int value = i + 1;
        if ((size_t)(token->end - token->start) == strlen(key) &&
            memcmp(json->text + token->start, key, strlen(key)) == 0) return value;
        i = value + 1;
        while (i < json->count && json->tokens[i].start < json->tokens[value].end) ++i;
    }
    return -1;
}

/* App Server 协议中的百分比、周期和时间戳均为整数。 */
static int64_t number(const Json *json, int index) {
    if (index < 0 || json->tokens[index].type != JSMN_PRIMITIVE) return -1;
    const jsmntok_t *token = &json->tokens[index];
    char text[32];
    int length = token->end - token->start;
    if (length <= 0 || length >= (int)sizeof(text)) return -1;
    memcpy(text, json->text + token->start, (size_t)length);
    text[length] = '\0';
    char *end;
    int64_t value = strtoll(text, &end, 10);
    return end != text && *end == '\0' ? value : -1;
}

/* 0 表示通知或其他请求的回复；1 表示成功；-1 表示协议或服务错误。 */
static int parse_reply(const char *text, int id, Quota *quota) {
    Json json = {.text = text};
    jsmn_parser parser;
    jsmn_init(&parser);
    json.count = jsmn_parse(&parser, text, strlen(text), json.tokens, ARRAY_COUNT(json.tokens));
    if (json.count <= 0 || json.tokens[0].type != JSMN_OBJECT) {
        wcscpy(quota->error, L"额度响应格式错误");
        return -1;
    }
    if (number(&json, member(&json, 0, "id")) != id) return 0;
    int error = member(&json, 0, "error");
    if (error >= 0) {
        wcscpy(quota->error, L"Codex 登录或额度服务异常");
        int message = member(&json, error, "message");
        if (message >= 0 && json.tokens[message].type == JSMN_STRING) {
            int bytes = json.tokens[message].end - json.tokens[message].start;
            if (bytes > 79) bytes = 79;
            int written = MultiByteToWideChar(CP_UTF8, 0, text + json.tokens[message].start,
                                              bytes, quota->error, 79);
            if (written > 0) quota->error[written] = L'\0';
        }
        return -1;
    }
    int result = member(&json, 0, "result");
    if (result < 0 || json.tokens[result].type != JSMN_OBJECT) {
        wcscpy(quota->error, L"额度响应缺少 result");
        return -1;
    }
    if (id == 1) return 1;
    int buckets = member(&json, result, "rateLimitsByLimitId");
    int bucket = buckets >= 0 && json.tokens[buckets].type == JSMN_OBJECT
        ? member(&json, buckets, "codex") : member(&json, result, "rateLimits");
    int limit_id = member(&json, bucket, "limitId");
    if (limit_id >= 0 && json.tokens[limit_id].type == JSMN_STRING &&
        (json.tokens[limit_id].end - json.tokens[limit_id].start != 5 ||
         memcmp(text + json.tokens[limit_id].start, "codex", 5) != 0)) bucket = -1;
    const char *names[] = {"primary", "secondary"};
    for (size_t i = 0; i < ARRAY_COUNT(names); ++i) {
        int window = member(&json, bucket, names[i]);
        int64_t minutes = number(&json, member(&json, window, "windowDurationMins"));
        QuotaWindow *target = minutes == 300 ? &quota->hour : minutes == 10080 ? &quota->week : NULL;
        if (!target) continue;
        int64_t used = number(&json, member(&json, window, "usedPercent"));
        if (used < 0) continue;
        target->remaining = used >= 100 ? 0 : 100 - (int)used;
        target->resets_at = number(&json, member(&json, window, "resetsAt"));
    }
    int credits = member(&json, result, "rateLimitResetCredits");
    int64_t count = number(&json, member(&json, credits, "availableCount"));
    if (count >= 0 && count <= INT32_MAX) quota->credits = (int)count;
    return 1;
}

/* 与确认的预览使用完全相同的 32px 点阵，两行独立居中。 */
static void render_pixels(uint32_t pixels[1024], const Quota *quota) {
    static const unsigned char glyphs[11][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
        {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2}, {7,5,7,5,7}, {7,5,7,1,7}, {0,0,2,0,0}
    };
    for (int i = 0; i < 1024; ++i) pixels[i] = 0xff000000;
    int values[] = {quota->week.remaining, quota->hour.remaining};
    for (int line = 0; line < 2; ++line) {
        char text[4];
        if (values[line] < 0) strcpy(text, "...");
        else snprintf(text, sizeof(text), "%d", values[line]);
        int length = (int)strlen(text), advance = values[line] < 0 ? 4 : 8;
        int left = (32 - ((length - 1) * advance + 6)) / 2, top = 4 + line * 14;
        for (int digit = 0; digit < length; ++digit) {
            int glyph = text[digit] == '.' ? 10 : text[digit] - '0';
            for (int y = 0; y < 5; ++y) for (int x = 0; x < 3; ++x) {
                if (!(glyphs[glyph][y] & (4 >> x))) continue;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
                    pixels[(top + y * 2 + dy) * 32 + left + digit * advance + x * 2 + dx] = 0xffffffff;
            }
        }
    }
}

static HICON create_icon(const Quota *quota) {
    BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = 32,
        .biHeight = -32, .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
    void *pixels;
    HBITMAP color = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!color) return NULL;
    render_pixels(pixels, quota);
    unsigned char mask_bits[128] = {0};
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, mask_bits);
    ICONINFO icon = {.fIcon = TRUE, .hbmMask = mask, .hbmColor = color};
    HICON result = mask ? CreateIconIndirect(&icon) : NULL;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return result;
}

/* 重置时间按 Windows 当前时区转换，不推算缺失的时间。 */
static void reset_text(int64_t seconds, WCHAR text[32]) {
    wcscpy(text, L"...");
    if (seconds < 0 || seconds > 253402300799LL) return;
    ULARGE_INTEGER ticks = {.QuadPart = ((uint64_t)seconds + 11644473600ULL) * 10000000ULL};
    FILETIME utc = {ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc_time, local;
    if (FileTimeToSystemTime(&utc, &utc_time) && SystemTimeToTzSpecificLocalTime(NULL, &utc_time, &local))
        swprintf(text, 32, L"%02u-%02u %02u:%02u", local.wMonth, local.wDay, local.wHour, local.wMinute);
}

static void tooltip_text(const Quota *quota, WCHAR text[128]) {
    WCHAR week[32], hour[32], credits[24];
    reset_text(quota->week.resets_at, week);
    reset_text(quota->hour.resets_at, hour);
    if (quota->credits < 0) wcscpy(credits, L"...");
    else swprintf(credits, ARRAY_COUNT(credits), L"%d 次", quota->credits);
    swprintf(text, 128, L"week：%ls\n5 h：%ls\n可用重置：%ls%ls%.*ls", week, hour, credits,
        quota->error[0] ? L"（" : L"", 35, quota->error);
    if (quota->error[0]) wcscat(text, L"）");
}

/* 优先使用桌面应用管理的最新 CLI；显式指定路径时直接使用它。 */
static BOOL find_codex(WCHAR path[MAX_PATH]) {
    DWORD length = GetEnvironmentVariableW(L"TINY_CODEX_PATH", path, MAX_PATH);
    if (length) return length < MAX_PATH && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    WCHAR local[MAX_PATH], pattern[MAX_PATH], candidate[MAX_PATH];
    length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length && length < MAX_PATH &&
        swprintf(pattern, MAX_PATH, L"%ls\\OpenAI\\Codex\\bin\\*", local) >= 0) {
        WIN32_FIND_DATAW entry;
        HANDLE search = FindFirstFileW(pattern, &entry);
        FILETIME newest = {0};
        BOOL found = FALSE;
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry.cFileName[0] == '.') continue;
                if (swprintf(candidate, MAX_PATH, L"%ls\\OpenAI\\Codex\\bin\\%ls\\codex.exe", local, entry.cFileName) < 0) continue;
                WIN32_FILE_ATTRIBUTE_DATA data;
                if (GetFileAttributesExW(candidate, GetFileExInfoStandard, &data) &&
                    !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    (!found || CompareFileTime(&data.ftLastWriteTime, &newest) > 0)) {
                    wcscpy(path, candidate);
                    newest = data.ftLastWriteTime;
                    found = TRUE;
                }
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }
        if (found) return TRUE;
    }
    length = SearchPathW(NULL, L"codex.exe", NULL, MAX_PATH, path, NULL);
    return length > 0 && length < MAX_PATH;
}

typedef struct {
    HANDLE pipe, stop;
    char buffer[65536];
    DWORD used;
    ULONGLONG deadline;
} Reader;

/* 同一缓冲区连续处理 JSON 行；只在额度请求进行中等待管道。 */
static BOOL read_reply(Reader *reader, int id, Quota *quota) {
    for (;;) {
        char *newline = memchr(reader->buffer, '\n', reader->used);
        if (newline) {
            *newline = '\0';
            int parsed = parse_reply(reader->buffer, id, quota);
            DWORD consumed = (DWORD)(newline - reader->buffer) + 1;
            reader->used -= consumed;
            memmove(reader->buffer, newline + 1, reader->used);
            if (parsed) return parsed > 0;
            continue;
        }
        if (WaitForSingleObject(reader->stop, 0) == WAIT_OBJECT_0) return FALSE;
        if (GetTickCount64() >= reader->deadline) { wcscpy(quota->error, L"读取超时"); return FALSE; }
        DWORD available, received;
        if (!PeekNamedPipe(reader->pipe, NULL, 0, NULL, &available, NULL)) {
            wcscpy(quota->error, L"Codex 读取进程已关闭"); return FALSE;
        }
        if (available) {
            DWORD room = (DWORD)sizeof(reader->buffer) - reader->used - 1;
            if (!room) { wcscpy(quota->error, L"额度响应过大"); return FALSE; }
            if (!ReadFile(reader->pipe, reader->buffer + reader->used,
                          available < room ? available : room, &received, NULL) || !received) {
                wcscpy(quota->error, L"额度管道读取失败"); return FALSE;
            }
            reader->used += received;
        } else WaitForSingleObject(reader->stop, 100);
    }
}

static BOOL send_request(HANDLE pipe, const char *request) {
    DWORD written, length = (DWORD)strlen(request);
    return WriteFile(pipe, request, length, &written, NULL) && written == length;
}

/* 每次只发初始化和额度读取请求；Job 保证退出、超时后无遗留子进程。 */
static BOOL fetch_quota(HANDLE stop, Quota *quota) {
    *quota = empty_quota();
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return FALSE;
    WCHAR executable[MAX_PATH], command[MAX_PATH + 64];
    if (!find_codex(executable)) { wcscpy(quota->error, L"未找到 Codex CLI"); return FALSE; }
    HANDLE input_read = NULL, input_write = NULL, output_read = NULL, output_write = NULL, null_file = NULL;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    PROCESS_INFORMATION process = {0};
    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    BOOL success = FALSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !CreatePipe(&input_read, &input_write, &security, 0) ||
        !CreatePipe(&output_read, &output_write, &security, 0) ||
        !SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0)) goto cleanup;
    null_file = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
    if (null_file == INVALID_HANDLE_VALUE) goto cleanup;
    STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = input_read, .hStdOutput = output_write, .hStdError = null_file};
    swprintf(command, ARRAY_COUNT(command), L"\"%ls\" app-server", executable);
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, NULL, &startup, &process)) goto cleanup;
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); goto cleanup;
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) goto cleanup;
    CloseHandle(input_read); input_read = NULL;
    CloseHandle(output_write); output_write = NULL;
    Reader reader = {.pipe = output_read, .stop = stop, .deadline = GetTickCount64() + 20000};
    const char *initialize = "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"tiny\",\"title\":\"Tiny\",\"version\":\"1.0.1\"}}}\n";
    const char *usage = "{\"method\":\"initialized\",\"params\":{}}\n{\"id\":2,\"method\":\"account/rateLimits/read\"}\n";
    success = send_request(input_write, initialize) && read_reply(&reader, 1, quota) &&
              send_request(input_write, usage) && read_reply(&reader, 2, quota);
cleanup:
    if (!success && !quota->error[0] && WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)
        swprintf(quota->error, ARRAY_COUNT(quota->error), L"读取失败（错误 %lu）", GetLastError());
    HANDLE handles[] = {input_read, input_write, output_read, output_write, null_file, job};
    for (size_t i = 0; i < ARRAY_COUNT(handles); ++i)
        if (handles[i] && handles[i] != INVALID_HANDLE_VALUE) CloseHandle(handles[i]);
    if (process.hProcess) { WaitForSingleObject(process.hProcess, INFINITE); CloseHandle(process.hProcess); }
    if (process.hThread) CloseHandle(process.hThread);
    return success;
}

static DWORD WINAPI refresh_worker(void *unused) {
    (void)unused;
    fetch_quota(stop_event, &pending);
    if (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) PostMessageW(app_window, WM_QUOTA, 0, 0);
    return 0;
}

static void start_refresh(void) {
    if (!worker) {
        worker = CreateThread(NULL, 0, refresh_worker, NULL, 0, NULL);
        if (!worker) MessageBoxW(app_window, L"无法创建额度读取线程。", L"Tiny", MB_ICONERROR);
    }
}

static BOOL startup_command(WCHAR command[MAX_PATH + 3]) {
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return FALSE;
    swprintf(command, MAX_PATH + 3, L"\"%ls\"", path);
    return TRUE;
}

static BOOL startup_enabled(void) {
    WCHAR current[MAX_PATH + 3], command[MAX_PATH + 3];
    DWORD bytes = sizeof(current), type;
    return startup_command(command) &&
        RegGetValueW(HKEY_CURRENT_USER, run_key, L"Tiny", RRF_RT_REG_SZ, &type, current, &bytes) == ERROR_SUCCESS &&
        _wcsicmp(current, command) == 0;
}

static LSTATUS set_startup(BOOL enabled) {
    WCHAR command[MAX_PATH + 3];
    if (!enabled) return RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, L"Tiny");
    if (!startup_command(command)) return ERROR_FILENAME_EXCED_RANGE;
    HKEY key;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) return status;
    status = RegSetValueExW(key, L"Tiny", 0, REG_SZ, (const BYTE *)command,
                          (DWORD)((wcslen(command) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
    return status;
}

static void show_menu(HWND window) {
    POINT cursor;
    GetCursorPos(&cursor);
    /* 隐藏的菜单所属窗口跟随鼠标所在屏幕，使系统按该屏幕的 DPI 绘制菜单。 */
    SetWindowPos(window, NULL, cursor.x, cursor.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    HMENU menu = CreatePopupMenu();
    BOOL enabled = startup_enabled();
    AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : 0), 1, L"开机启动");
    AppendMenuW(menu, MF_STRING, 2, L"退出");
    SetForegroundWindow(window);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, NULL);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command == 2) DestroyWindow(window);
    else if (command == 1) {
        LSTATUS status = set_startup(!enabled);
        if (status != ERROR_SUCCESS) {
            WCHAR text[80];
            swprintf(text, ARRAY_COUNT(text), L"开机启动设置失败（错误 %ld）。", status);
            MessageBoxW(window, text, L"Tiny", MB_ICONERROR);
        }
    }
}

static BOOL add_tray(void) {
    if (!Shell_NotifyIconW(NIM_ADD, &tray)) return FALSE;
    tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray);
    return TRUE;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    static Quota displayed = {.week = {-1, -1}, .hour = {-1, -1}, .credits = -1};
    if (message == taskbar_created) { add_tray(); return 0; }
    switch (message) {
    case WM_TIMER: start_refresh(); return 0;
    case WM_QUOTA: {
        CloseHandle(worker); worker = NULL;
        if (displayed.week.remaining != pending.week.remaining || displayed.hour.remaining != pending.hour.remaining) {
            HICON icon = create_icon(&pending);
            if (!icon) {
                MessageBoxW(window, L"额度图标创建失败。", L"Tiny", MB_ICONERROR);
                DestroyWindow(window);
                return 0;
            }
            DestroyIcon(tray.hIcon); tray.hIcon = icon;
        }
        displayed = pending;
        tooltip_text(&pending, tray.szTip);
        Shell_NotifyIconW(NIM_MODIFY, &tray);
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) show_menu(window);
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wparam) DestroyWindow(window); return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        SetEvent(stop_event);
        if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); worker = NULL; }
        Shell_NotifyIconW(NIM_DELETE, &tray);
        if (tray.hIcon) DestroyIcon(tray.hIcon);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

#ifndef TINY_TEST
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    (void)previous; (void)command; (void)show;
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\Tiny.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_event) { CloseHandle(mutex); return 1; }
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW type = {.lpfnWndProc = window_proc, .hInstance = instance, .lpszClassName = L"Tiny"};
    RegisterClassW(&type);
    app_window = CreateWindowExW(0, type.lpszClassName, L"Tiny", WS_POPUP, 0, 0, 1, 1, NULL, NULL, instance, NULL);
    Quota initial = empty_quota();
    tray = (NOTIFYICONDATAW){.cbSize = sizeof(tray), .hWnd = app_window, .uID = 1,
        .uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP, .uCallbackMessage = WM_TRAY,
        .hIcon = create_icon(&initial)};
    tooltip_text(&initial, tray.szTip);
    if (!app_window || !tray.hIcon || !add_tray() || !SetTimer(app_window, 1, REFRESH_MS, NULL)) {
        MessageBoxW(NULL, L"无法创建额度托盘图标。", L"Tiny", MB_ICONERROR);
        if (app_window) DestroyWindow(app_window);
        CloseHandle(stop_event); CloseHandle(mutex);
        return 1;
    }
    start_refresh();
    MSG message;
    int result;
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(stop_event); CloseHandle(mutex);
    return result < 0 ? 1 : 0;
}
#endif
