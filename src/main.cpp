#include "clip_store.h"
#include "../resources/resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr wchar_t kManagerClass[] = L"ClipNest.ManagerWindow";
constexpr wchar_t kPopupClass[] = L"ClipNest.QuickWindow";
constexpr wchar_t kSettingsClass[] = L"ClipNest.SettingsWindow";
constexpr wchar_t kInfoClass[] = L"ClipNest.InfoWindow";
constexpr wchar_t kProductName[] = L"ClipNest";
constexpr wchar_t kProductVersion[] = L"1.0.0";
constexpr float kTitleBarHeight = 44.0f;
constexpr float kCloseButtonWidth = 36.0f;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowExistingMessage = WM_APP + 2;
constexpr UINT kHotkeyId = 1;
constexpr UINT kTrayId = 1;
constexpr UINT kSearchId = 100;
constexpr UINT_PTR kPasteTimer = 10;
constexpr UINT_PTR kStatusTimer = 11;
constexpr UINT_PTR kAnimationTimer = 12;
constexpr UINT_PTR kClipboardRetryTimer = 13;
constexpr UINT_PTR kMemoryTrimTimer = 14;
constexpr UINT_PTR kPanelFlipTimer = 15;

constexpr UINT kCommandOpenManager = 200;
constexpr UINT kCommandOpenQuick = 201;
constexpr UINT kCommandPause = 202;
constexpr UINT kCommandStartup = 203;
constexpr UINT kCommandExit = 204;
constexpr UINT kCommandFavorite = 205;
constexpr UINT kCommandDelete = 206;
constexpr UINT kCommandClear = 207;
constexpr UINT kCommandSettings = 208;
constexpr UINT kCommandHelp = 209;
constexpr UINT kCommandAbout = 210;
constexpr UINT kCommandCopyPreview = 211;
constexpr UINT kCommandClearFavorites = 300;
constexpr UINT kCommandFavoriteTop = 301;
constexpr UINT kCommandFavoriteUp = 302;
constexpr UINT kCommandFavoriteDown = 303;
constexpr UINT kCommandFavoriteBottom = 304;

constexpr int kSettingsHotkey = 1;
constexpr int kSettingsAutoPaste = 2;
constexpr int kSettingsHistoryMinus = 3;
constexpr int kSettingsHistoryPlus = 4;
constexpr int kSettingsFavoriteMinus = 5;
constexpr int kSettingsFavoritePlus = 6;
constexpr int kSettingsMonitor = 7;
constexpr int kSettingsStartup = 8;
constexpr int kSettingsLaunchHidden = 9;
constexpr int kSettingsOpacity = 10;

struct Theme {
    D2D1_COLOR_F background;
    D2D1_COLOR_F surface;
    D2D1_COLOR_F text;
    D2D1_COLOR_F muted;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F selected;
    D2D1_COLOR_F selectedText;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F danger;
    COLORREF editBackground;
    COLORREF editText;
};

struct Surface {
    ID2D1HwndRenderTarget* target{};
    ID2D1SolidColorBrush* brush{};

    void Reset() {
        if (brush) brush->Release();
        if (target) target->Release();
        brush = nullptr;
        target = nullptr;
    }
};

struct ManagerLayout {
    float width{};
    float height{};
    D2D1_RECT_F favoriteButton{};
    D2D1_RECT_F deleteButton{};
    D2D1_RECT_F clearButton{};
    D2D1_RECT_F settingsButton{};
    D2D1_RECT_F favorites{};
    D2D1_RECT_F history{};
    D2D1_RECT_F preview{};
    D2D1_RECT_F previewBody{};
    D2D1_RECT_F previewCopyButton{};
    float favoriteTitleY{};
    float favoriteRowsY{};
    float historyTitleY{};
    float historyRowsY{};
    float previewTitleY{};
    float previewTextY{};
    float rowHeight{28.0f};
};

struct PopupLayout {
    float width{};
    float height{};
    float headerHeight{29.0f};
    float rowHeight{21.0f};
    float favoriteWidth{};
    float firstHistoryWidth{};
    float secondHistoryWidth{};
    int rows{};
    bool splitHistory{};
};

struct SettingsLayout {
    float width{};
    float height{};
    D2D1_RECT_F hotkeyButton{};
    D2D1_RECT_F autoPasteToggle{};
    D2D1_RECT_F historyMinus{};
    D2D1_RECT_F historyPlus{};
    D2D1_RECT_F favoriteMinus{};
    D2D1_RECT_F favoritePlus{};
    D2D1_RECT_F monitorToggle{};
    D2D1_RECT_F startupToggle{};
    D2D1_RECT_F launchHiddenToggle{};
    D2D1_RECT_F opacitySlider{};
};

struct AppSettings {
    std::size_t historyLimit{ClipStore::kDefaultHistoryLimit};
    std::size_t favoriteLimit{ClipStore::kDefaultFavoriteLimit};
    UINT hotkeyModifiers{MOD_CONTROL | MOD_ALT};
    UINT hotkeyVirtualKey{'V'};
    bool autoPaste{true};
    bool monitorClipboard{true};
    bool launchHidden{false};
    UINT windowOpacity{100};
};

enum class InfoKind {
    Help,
    About,
};

HINSTANCE g_instance{};
HANDLE g_singleInstance{};
HWND g_manager{};
HWND g_popup{};
HWND g_settingsWindow{};
HWND g_infoWindow{};
HWND g_search{};
HWND g_pasteTarget{};
ClipStore g_store;
AppSettings g_appSettings;
bool g_paused{};
bool g_quitting{};
bool g_hotkeyRegistered{};
bool g_dark{};
bool g_capturingHotkey{};
bool g_managerCloseHovered{};
bool g_managerClosePressed{};
bool g_settingsCloseHovered{};
bool g_settingsClosePressed{};
bool g_settingsBackHovered{};
bool g_settingsBackPressed{};
bool g_infoCloseHovered{};
bool g_infoClosePressed{};
InfoKind g_infoKind{InfoKind::About};
int g_selectedSection{1}; // 0 favorites, 1 history
std::size_t g_selectedIndex{};
int g_popupSection{1};
std::size_t g_popupIndex{};
std::size_t g_favoriteScroll{};
std::size_t g_managerFavoriteScroll{};
std::size_t g_managerHistoryScroll{};
UINT g_managerPressedCommand{};
int g_popupHoverSection{-1};
std::size_t g_popupHoverIndex{};
int g_popupPressedSection{-1};
std::size_t g_popupPressedIndex{};
int g_managerHoverSection{-1};
std::size_t g_managerHoverIndex{};
UINT g_managerHoverCommand{};
int g_settingsHoverControl{-1};
int g_settingsPressedControl{-1};
bool g_draggingOpacity{};
bool g_settingsFace{};
bool g_flipAnimating{};
bool g_flipTargetSettings{};
ULONGLONG g_panelFlipStarted{};
bool g_popupSplitHistory{};
std::wstring g_searchText;
std::wstring g_statusText;
std::wstring g_settingsStatus;
RECT g_popupFinalRect{};
POINT g_popupAnimationOrigin{};
ULONGLONG g_animationStarted{};
int g_clipboardRetries{};
HICON g_trayIcon{};
HFONT g_editFont{};
HBRUSH g_editBrush{};
ID2D1Factory* g_d2dFactory{};
IDWriteFactory* g_writeFactory{};
IDWriteTextFormat* g_titleFormat{};
IDWriteTextFormat* g_headerFormat{};
IDWriteTextFormat* g_bodyFormat{};
IDWriteTextFormat* g_smallFormat{};
IDWriteTextFormat* g_monoFormat{};
Surface g_managerSurface;
Surface g_popupSurface;
Surface g_settingsSurface;
Surface g_infoSurface;

LRESULT CALLBACK ManagerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SettingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK InfoProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
bool CreateTextResources();
void ReleaseGraphicsResources();
void StartPanelFlip(bool settingsFace);
void SetPanelFace(bool settingsFace);
void ApplyWindowOpacity(HWND window);

void SyncSettingsChild(HWND parent) {
    if (!parent || !g_settingsWindow) return;
    RECT client{};
    GetClientRect(parent, &client);
    SetWindowPos(g_settingsWindow, HWND_TOP, 0, 0,
                 client.right - client.left, client.bottom - client.top,
                 SWP_NOACTIVATE);
}

float DpiScale(HWND window) {
    return static_cast<float>(GetDpiForWindow(window)) / 96.0f;
}

D2D1_POINT_2F ToDip(HWND window, LPARAM lParam) {
    const float scale = DpiScale(window);
    return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                        static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
}

bool Contains(const D2D1_RECT_F& rectangle, D2D1_POINT_2F point) {
    return point.x >= rectangle.left && point.x < rectangle.right &&
           point.y >= rectangle.top && point.y < rectangle.bottom;
}

bool IsDarkTheme() {
    DWORD light = 1;
    DWORD bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    return light == 0;
}

Theme CurrentTheme() {
    if (g_dark) {
        return {
            D2D1::ColorF(0x202124), D2D1::ColorF(0x292B2F), D2D1::ColorF(0xF4F6FA),
            D2D1::ColorF(0xB7BBC3), D2D1::ColorF(0x3A3D43), D2D1::ColorF(0x32353A),
            D2D1::ColorF(0x254A78), D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0x72A0FF),
            D2D1::ColorF(0xFF8F9B), RGB(41, 43, 47), RGB(244, 246, 250)};
    }
    return {
        D2D1::ColorF(0xF2F3F5), D2D1::ColorF(0xFAFAFB), D2D1::ColorF(0x22252A),
        D2D1::ColorF(0x60646C), D2D1::ColorF(0xE3E5E9), D2D1::ColorF(0xE9EBEF),
        D2D1::ColorF(0xDDE8FF), D2D1::ColorF(0x1F355D), D2D1::ColorF(0x4F83FF),
        D2D1::ColorF(0xD63A45), RGB(250, 250, 251), RGB(34, 37, 42)};
}

DWORD ReadAppSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ClipboardTool", name,
                     RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

void LoadAppSettings() {
    g_appSettings.historyLimit = std::clamp<std::size_t>(
        ReadAppSetting(L"HistoryLimit", ClipStore::kDefaultHistoryLimit), 10, 500);
    g_appSettings.favoriteLimit = std::clamp<std::size_t>(
        ReadAppSetting(L"FavoriteLimit", ClipStore::kDefaultFavoriteLimit), 10, 500);
    g_appSettings.hotkeyModifiers =
        ReadAppSetting(L"HotkeyModifiers", MOD_CONTROL | MOD_ALT) &
        (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    g_appSettings.hotkeyVirtualKey = ReadAppSetting(L"HotkeyVirtualKey", 'V');
    if (g_appSettings.hotkeyModifiers == 0 ||
        g_appSettings.hotkeyVirtualKey < VK_SPACE || g_appSettings.hotkeyVirtualKey > 0xFE) {
        g_appSettings.hotkeyModifiers = MOD_CONTROL | MOD_ALT;
        g_appSettings.hotkeyVirtualKey = 'V';
    }
    g_appSettings.autoPaste = ReadAppSetting(L"AutoPaste", 1) != 0;
    g_appSettings.monitorClipboard = ReadAppSetting(L"MonitorClipboard", 1) != 0;
    g_appSettings.launchHidden = ReadAppSetting(L"LaunchHidden", 0) != 0;
    g_appSettings.windowOpacity = std::clamp<UINT>(
        ReadAppSetting(L"WindowOpacity", 100), 65, 100);
    g_paused = !g_appSettings.monitorClipboard;
    g_store.SetLimits(g_appSettings.historyLimit, g_appSettings.favoriteLimit);
}

void SaveAppSettings() {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ClipboardTool", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    auto write = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    write(L"HistoryLimit", static_cast<DWORD>(g_appSettings.historyLimit));
    write(L"FavoriteLimit", static_cast<DWORD>(g_appSettings.favoriteLimit));
    write(L"HotkeyModifiers", g_appSettings.hotkeyModifiers);
    write(L"HotkeyVirtualKey", g_appSettings.hotkeyVirtualKey);
    write(L"AutoPaste", g_appSettings.autoPaste ? 1 : 0);
    write(L"MonitorClipboard", g_appSettings.monitorClipboard ? 1 : 0);
    write(L"LaunchHidden", g_appSettings.launchHidden ? 1 : 0);
    write(L"WindowOpacity", g_appSettings.windowOpacity);
    RegCloseKey(key);
}

void ApplyWindowAppearance(HWND window, bool popup) {
    if (!window) return;
    const BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
    const int backdrop = popup ? 3 : 2; // transient acrylic / mica where supported
    DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
    ApplyWindowOpacity(window);
}

void ApplyWindowOpacity(HWND window) {
    if (!window || window == g_settingsWindow || GetAncestor(window, GA_ROOT) != window) return;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & WS_EX_LAYERED) == 0) {
        SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
    }
    const BYTE alpha = static_cast<BYTE>(std::clamp<UINT>(g_appSettings.windowOpacity, 65, 100) * 255 / 100);
    SetLayeredWindowAttributes(window, 0, alpha, LWA_ALPHA);
}

bool EnsureSurface(HWND window, Surface& surface) {
    if (surface.target) return true;
    RECT client{};
    GetClientRect(window, &client);
    const auto size = D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom));
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    if (FAILED(g_d2dFactory->CreateHwndRenderTarget(
            properties, D2D1::HwndRenderTargetProperties(window, size), &surface.target))) {
        return false;
    }
    surface.target->SetDpi(static_cast<float>(GetDpiForWindow(window)),
                           static_cast<float>(GetDpiForWindow(window)));
    return SUCCEEDED(surface.target->CreateSolidColorBrush(D2D1::ColorF(0), &surface.brush));
}

void ResizeSurface(HWND window, Surface& surface) {
    if (!surface.target) return;
    RECT client{};
    GetClientRect(window, &client);
    surface.target->Resize(D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom)));
    surface.target->SetDpi(static_cast<float>(GetDpiForWindow(window)),
                           static_cast<float>(GetDpiForWindow(window)));
}

void FillRect(Surface& surface, D2D1_RECT_F rectangle, D2D1_COLOR_F color) {
    surface.brush->SetColor(color);
    surface.target->FillRectangle(rectangle, surface.brush);
}

void FillRoundRect(Surface& surface, D2D1_RECT_F rectangle, float radius, D2D1_COLOR_F color) {
    surface.brush->SetColor(color);
    surface.target->FillRoundedRectangle(D2D1::RoundedRect(rectangle, radius, radius), surface.brush);
}

void DrawRoundRect(Surface& surface, D2D1_RECT_F rectangle, float radius,
                   D2D1_COLOR_F color, float width = 1.0f) {
    surface.brush->SetColor(color);
    surface.target->DrawRoundedRectangle(D2D1::RoundedRect(rectangle, radius, radius),
                                         surface.brush, width);
}

void FillCircle(Surface& surface, D2D1_POINT_2F center, float radius, D2D1_COLOR_F color) {
    surface.brush->SetColor(color);
    surface.target->FillEllipse(D2D1::Ellipse(center, radius, radius), surface.brush);
}

void DrawLine(Surface& surface, D2D1_POINT_2F first, D2D1_POINT_2F second,
              D2D1_COLOR_F color, float width = 1.0f) {
    surface.brush->SetColor(color);
    surface.target->DrawLine(first, second, surface.brush, width);
}

void DrawTextValue(Surface& surface, const std::wstring& value, IDWriteTextFormat* format,
                   D2D1_RECT_F rectangle, D2D1_COLOR_F color,
                   D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_CLIP) {
    surface.brush->SetColor(color);
    surface.target->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, rectangle,
                              surface.brush, options);
}

std::wstring Summary(const std::wstring& value, std::size_t maxCharacters) {
    std::wstring result;
    result.reserve(std::min(value.size(), maxCharacters));
    bool previousSpace = false;
    for (wchar_t character : value) {
        const bool space = std::iswspace(character) != 0;
        if (space && previousSpace) continue;
        result.push_back(space ? L' ' : character);
        previousSpace = space;
        if (result.size() >= maxCharacters) break;
    }
    if (result.size() < value.size() && result.size() >= maxCharacters && maxCharacters > 1) {
        result.back() = L'…';
    }
    return result;
}

std::wstring FavoriteAlias(const std::wstring& value) {
    std::wstring alias = Summary(value, 8);
    if (alias.empty()) return L"收藏";
    const std::size_t separator = alias.find_first_of(L" \t\r\n/:：");
    if (separator >= 2 && separator < alias.size()) alias.resize(separator);
    return alias;
}

std::wstring RowNumber(std::size_t index) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, index < 99 ? L"%02zu" : L"%zu", index + 1);
    return buffer;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::vector<std::size_t> Filtered(const std::vector<ClipEntry>& entries) {
    std::vector<std::size_t> result;
    result.reserve(entries.size());
    const std::wstring query = Lower(g_searchText);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (query.empty() || Lower(entries[index].text).find(query) != std::wstring::npos) {
            result.push_back(index);
        }
    }
    return result;
}

ManagerLayout GetManagerLayout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const float scale = DpiScale(window);
    ManagerLayout layout;
    layout.width = static_cast<float>(client.right) / scale;
    layout.height = static_cast<float>(client.bottom) / scale;
    const float closeLeft = layout.width - kCloseButtonWidth;
    layout.settingsButton = D2D1::RectF(closeLeft - 30, 8, closeLeft - 4, 36);

    constexpr float contentTop = 88.0f;
    constexpr float favoriteHeight = 142.0f;
    constexpr float historyHeight = 166.0f;
    constexpr float sectionGap = 8.0f;
    layout.favorites = D2D1::RectF(8, contentTop, layout.width - 8, contentTop + favoriteHeight);
    layout.history = D2D1::RectF(8, layout.favorites.bottom + sectionGap, layout.width - 8,
                                 layout.favorites.bottom + sectionGap + historyHeight);
    layout.preview = D2D1::RectF(8, layout.history.bottom + sectionGap,
                                 layout.width - 8, layout.height - 8);
    layout.favoriteTitleY = layout.favorites.top + 10;
    layout.favoriteRowsY = layout.favorites.top + 48;
    layout.historyTitleY = layout.history.top + 10;
    layout.historyRowsY = layout.history.top + 48;
    layout.previewTitleY = layout.preview.top + 10;
    layout.previewTextY = layout.preview.top + 42;
    layout.previewCopyButton = D2D1::RectF(layout.preview.right - 66, layout.preview.top + 8,
                                          layout.preview.right - 10, layout.preview.top + 30);
    layout.previewBody = D2D1::RectF(layout.preview.left + 10, layout.previewTextY,
                                    layout.preview.right - 10, layout.preview.bottom - 10);
    return layout;
}

PopupLayout GetPopupLayout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const float scale = DpiScale(window);
    PopupLayout layout;
    layout.width = static_cast<float>(client.right) / scale;
    layout.height = static_cast<float>(client.bottom) / scale;
    layout.rows = std::max(
        1, static_cast<int>((layout.height - layout.headerHeight - 8.0f) / layout.rowHeight));
    layout.splitHistory = g_popupSplitHistory;
    layout.favoriteWidth = layout.width * (layout.splitHistory ? 0.30f : 0.36f);
    layout.firstHistoryWidth = layout.splitHistory ? layout.width * 0.36f
                                                   : layout.width - layout.favoriteWidth;
    layout.secondHistoryWidth = layout.splitHistory
        ? layout.width - layout.favoriteWidth - layout.firstHistoryWidth : 0.0f;
    return layout;
}

SettingsLayout GetSettingsLayout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const float scale = DpiScale(window);
    SettingsLayout layout;
    layout.width = static_cast<float>(client.right) / scale;
    layout.height = static_cast<float>(client.bottom) / scale;
    layout.hotkeyButton = D2D1::RectF(16, 72, layout.width - 16, 116);
    layout.autoPasteToggle = D2D1::RectF(16, 132, layout.width - 16, 180);
    layout.historyMinus = D2D1::RectF(layout.width - 112, 188, layout.width - 88, 212);
    layout.historyPlus = D2D1::RectF(layout.width - 40, 188, layout.width - 16, 212);
    layout.favoriteMinus = D2D1::RectF(layout.width - 112, 228, layout.width - 88, 252);
    layout.favoritePlus = D2D1::RectF(layout.width - 40, 228, layout.width - 16, 252);
    layout.opacitySlider = D2D1::RectF(16, 260, layout.width - 16, 304);
    layout.monitorToggle = D2D1::RectF(16, 338, layout.width - 16, 382);
    layout.startupToggle = D2D1::RectF(16, 382, layout.width - 16, 426);
    layout.launchHiddenToggle = D2D1::RectF(16, 426, layout.width - 16, 470);
    return layout;
}

D2D1_RECT_F CloseButtonRect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const float width = static_cast<float>(client.right) / DpiScale(window);
    return D2D1::RectF(width - kCloseButtonWidth, 0, width, kTitleBarHeight);
}

D2D1_RECT_F SettingsBackButtonRect(HWND window) {
    const auto close = CloseButtonRect(window);
    return D2D1::RectF(close.left - 34, 8, close.left - 6, 36);
}

void DrawWindowTitleBar(Surface& surface, HWND window, const std::wstring& title,
                        bool hovered, bool pressed, const Theme& theme) {
    const auto close = CloseButtonRect(window);
    DrawTextValue(surface, title, g_headerFormat,
                  D2D1::RectF(16, 13, close.left - 8, kTitleBarHeight - 4), theme.text);
    FillRect(surface, close, pressed ? theme.selected : (hovered ? theme.hover : theme.background));
    const float centerX = (close.left + close.right) * 0.5f;
    const float centerY = (close.top + close.bottom) * 0.5f;
    const D2D1_COLOR_F closeColor = pressed ? theme.selectedText : theme.text;
    DrawLine(surface, D2D1::Point2F(centerX - 5, centerY - 5),
             D2D1::Point2F(centerX + 5, centerY + 5), closeColor, 1.0f);
    DrawLine(surface, D2D1::Point2F(centerX + 5, centerY - 5),
             D2D1::Point2F(centerX - 5, centerY + 5), closeColor, 1.0f);
    DrawLine(surface, D2D1::Point2F(0, kTitleBarHeight - 0.5f),
             D2D1::Point2F(close.right, kTitleBarHeight - 0.5f), theme.divider);
}

void DrawButton(Surface& surface, D2D1_RECT_F rectangle, const std::wstring& label,
                const Theme& theme, bool enabled = true, bool hovered = false,
                bool pressed = false) {
    FillRoundRect(surface, rectangle, 6.0f,
                  !enabled ? theme.background : (pressed ? theme.selected :
                  (hovered ? theme.selected : theme.hover)));
    DrawRoundRect(surface, rectangle, 6.0f, theme.divider);
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawTextValue(surface, label, g_monoFormat, rectangle,
                  enabled ? (pressed ? theme.selectedText : theme.text) : theme.muted);
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void DrawToolbarButton(Surface& surface, D2D1_RECT_F rectangle, const std::wstring& glyph,
                       UINT command, const Theme& theme, bool enabled = true) {
    const bool pressed = g_managerPressedCommand == command;
    const bool hovered = g_managerHoverCommand == command;
    if (pressed || hovered) {
        FillRoundRect(surface, rectangle, 6.0f, pressed ? theme.selected : theme.hover);
    }
    g_headerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawTextValue(surface, glyph, g_headerFormat, rectangle,
                  enabled ? (pressed ? theme.selectedText : theme.text) : theme.muted);
    g_headerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void DrawSettingsIconButton(Surface& surface, D2D1_RECT_F rectangle, bool hovered,
                            bool pressed, const Theme& theme) {
    if (hovered || pressed) {
        FillRoundRect(surface, rectangle, 6, pressed ? theme.selected : theme.hover);
    }
    const D2D1_COLOR_F color = pressed ? theme.selectedText : theme.text;
    const float left = rectangle.left + 6;
    const float right = rectangle.right - 6;
    const std::array<float, 3> ys{rectangle.top + 7, rectangle.top + 14, rectangle.top + 21};
    const std::array<float, 3> knobs{left + 4, right - 4, left + 7};
    for (std::size_t index = 0; index < ys.size(); ++index) {
        DrawLine(surface, D2D1::Point2F(left, ys[index]), D2D1::Point2F(right, ys[index]),
                 color, 1.2f);
        FillCircle(surface, D2D1::Point2F(knobs[index], ys[index]), 2.2f, color);
    }
}

void DrawBackIconButton(Surface& surface, D2D1_RECT_F rectangle, bool hovered,
                        bool pressed, const Theme& theme) {
    if (hovered || pressed) {
        FillRoundRect(surface, rectangle, 6, pressed ? theme.selected : theme.hover);
    }
    const D2D1_COLOR_F color = pressed ? theme.selectedText : theme.text;
    const float centerY = (rectangle.top + rectangle.bottom) * 0.5f;
    DrawLine(surface, D2D1::Point2F(rectangle.left + 8, centerY),
             D2D1::Point2F(rectangle.right - 7, centerY), color, 1.4f);
    DrawLine(surface, D2D1::Point2F(rectangle.left + 8, centerY),
             D2D1::Point2F(rectangle.left + 14, centerY - 6), color, 1.4f);
    DrawLine(surface, D2D1::Point2F(rectangle.left + 8, centerY),
             D2D1::Point2F(rectangle.left + 14, centerY + 6), color, 1.4f);
}

void DrawToggleControl(Surface& surface, D2D1_RECT_F rectangle, bool checked,
                       const Theme& theme, bool hovered) {
    const auto track = D2D1::RectF(rectangle.right - 42, rectangle.top + 14,
                                   rectangle.right - 2, rectangle.top + 36);
    FillRoundRect(surface, track, 11.0f,
                  checked ? theme.accent : (hovered ? theme.muted : theme.divider));
    const float center = checked ? track.right - 11.0f : track.left + 11.0f;
    FillRoundRect(surface, D2D1::RectF(center - 8, track.top + 3, center + 8, track.bottom - 3),
                  8.0f, D2D1::ColorF(0xFFFFFF));
}

void SetStatus(std::wstring text) {
    g_statusText = std::move(text);
    InvalidateRect(g_manager, nullptr, FALSE);
    KillTimer(g_manager, kStatusTimer);
    SetTimer(g_manager, kStatusTimer, 2200, nullptr);
}

std::optional<std::wstring> ReadClipboardText(HWND owner) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return std::nullopt;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return std::nullopt;
    }
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) {
        CloseClipboard();
        return std::nullopt;
    }
    std::wstring result(text);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
}

bool SetClipboardText(const std::wstring& text) {
    if (!OpenClipboard(g_manager)) return false;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

std::wstring ModulePath() {
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

bool IsStartupEnabled() {
    wchar_t value[2048]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     L"ClipNest", RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    const std::wstring expected = L"\"" + ModulePath() + L"\" --startup";
    return _wcsicmp(value, expected.c_str()) == 0;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring value = L"\"" + ModulePath() + L"\" --startup";
        result = RegSetValueExW(key, L"ClipNest", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"ClipNest");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    const LONG legacyResult = RegDeleteValueW(key, L"ClipboardTool");
    if (result == ERROR_SUCCESS && legacyResult != ERROR_SUCCESS &&
        legacyResult != ERROR_FILE_NOT_FOUND) {
        result = legacyResult;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void MigrateLegacyStartup() {
    if (IsStartupEnabled()) return;
    wchar_t legacyValue[2048]{};
    DWORD bytes = sizeof(legacyValue);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     L"ClipboardTool", RRF_RT_REG_SZ, nullptr, legacyValue, &bytes) == ERROR_SUCCESS) {
        SetStartupEnabled(true);
    }
}

std::wstring HotkeyText(UINT modifiers, UINT virtualKey) {
    std::wstring text;
    auto append = [&](const wchar_t* part) {
        if (!text.empty()) text += L" + ";
        text += part;
    };
    if (modifiers & MOD_CONTROL) append(L"Ctrl");
    if (modifiers & MOD_ALT) append(L"Alt");
    if (modifiers & MOD_SHIFT) append(L"Shift");
    if (modifiers & MOD_WIN) append(L"Win");

    wchar_t keyName[64]{};
    if ((virtualKey >= 'A' && virtualKey <= 'Z') ||
        (virtualKey >= '0' && virtualKey <= '9')) {
        keyName[0] = static_cast<wchar_t>(virtualKey);
        keyName[1] = L'\0';
    } else if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        swprintf_s(keyName, L"F%u", virtualKey - VK_F1 + 1);
    } else {
        const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        GetKeyNameTextW(static_cast<LONG>(scanCode << 16), keyName, 64);
    }
    append(keyName[0] ? keyName : L"?");
    return text;
}

std::vector<std::wstring> HotkeyParts(UINT modifiers, UINT virtualKey) {
    std::vector<std::wstring> parts;
    if (modifiers & MOD_CONTROL) parts.emplace_back(L"Ctrl");
    if (modifiers & MOD_ALT) parts.emplace_back(L"Alt");
    if (modifiers & MOD_SHIFT) parts.emplace_back(L"Shift");
    if (modifiers & MOD_WIN) parts.emplace_back(L"Win");
    const std::wstring full = HotkeyText(0, virtualKey);
    parts.push_back(full.empty() ? L"?" : full);
    return parts;
}

void DrawHotkeyCaps(Surface& surface, D2D1_RECT_F row, const Theme& theme) {
    if (g_capturingHotkey) {
        const auto badge = D2D1::RectF(row.right - 116, row.top + 12, row.right - 12, row.bottom - 12);
        FillRoundRect(surface, badge, 6, theme.selected);
        DrawRoundRect(surface, badge, 6, theme.divider);
        g_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawTextValue(surface, L"请按下组合键", g_smallFormat, badge, theme.accent);
        g_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return;
    }

    const auto parts = HotkeyParts(g_appSettings.hotkeyModifiers, g_appSettings.hotkeyVirtualKey);
    float right = row.right - 12;
    const float top = row.top + 12;
    const float bottom = row.bottom - 12;
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (std::size_t reverse = parts.size(); reverse > 0; --reverse) {
        const std::wstring& part = parts[reverse - 1];
        const float width = std::max(24.0f, 12.0f + static_cast<float>(part.size()) * 7.0f);
        const auto key = D2D1::RectF(right - width, top, right, bottom);
        FillRoundRect(surface, key, 5, theme.background);
        DrawRoundRect(surface, key, 5, theme.divider);
        DrawLine(surface, D2D1::Point2F(key.left + 4, key.bottom - 1.5f),
                 D2D1::Point2F(key.right - 4, key.bottom - 1.5f), theme.divider, 1.0f);
        DrawTextValue(surface, part, g_monoFormat, key, theme.muted);
        right = key.left;
        if (reverse > 1) {
            const auto plus = D2D1::RectF(right - 16, top, right, bottom);
            DrawTextValue(surface, L"+", g_monoFormat, plus, theme.muted);
            right -= 16;
        }
    }
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

HICON CreateTrayIcon() {
    HICON icon = static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    return icon ? icon : CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
}

void AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_manager;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g_trayIcon;
    wcscpy_s(data.szTip, kProductName);
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_manager;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void ShowTrayMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kCommandOpenManager, L"打开主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, L"退出");
    SetForegroundWindow(g_manager);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   point.x, point.y, 0, g_manager, nullptr);
    DestroyMenu(menu);
}

void ShowManager() {
    KillTimer(g_manager, kMemoryTrimTimer);
    if (!CreateTextResources()) return;
    g_dark = IsDarkTheme();
    if (g_flipAnimating) {
        KillTimer(g_manager, kPanelFlipTimer);
        g_flipAnimating = false;
    }
    SetPanelFace(false);
    ApplyWindowAppearance(g_manager, false);
    ShowWindow(g_manager, SW_SHOW);
    SetForegroundWindow(g_manager);
    InvalidateRect(g_manager, nullptr, FALSE);
}

void ShowSettings() {
    if (!g_manager || !g_settingsWindow) return;
    StartPanelFlip(true);
}

void HideSettings() {
    if (g_settingsFace) StartPanelFlip(false);
}

void SetPanelFace(bool settingsFace) {
    g_settingsFace = settingsFace;
    if (g_search) ShowWindow(g_search, settingsFace ? SW_HIDE : SW_SHOW);
    if (g_settingsWindow) {
        SyncSettingsChild(g_manager);
        ShowWindow(g_settingsWindow, settingsFace ? SW_SHOW : SW_HIDE);
        if (settingsFace) SetWindowPos(g_settingsWindow, HWND_TOP, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    g_capturingHotkey = false;
    g_settingsCloseHovered = false;
    g_settingsClosePressed = false;
    g_settingsBackHovered = false;
    g_settingsBackPressed = false;
    InvalidateRect(g_manager, nullptr, FALSE);
    if (g_settingsWindow) InvalidateRect(g_settingsWindow, nullptr, FALSE);
}

void AnimatePanelFlip() {
    constexpr float durationMs = 180.0f;
    const float elapsed = static_cast<float>(GetTickCount64() - g_panelFlipStarted);
    const float t = std::clamp(elapsed / durationMs, 0.0f, 1.0f);
    const float progress = 1.0f - std::pow(1.0f - t, 3.0f);
    RECT client{};
    GetClientRect(g_manager, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int offset = g_flipTargetSettings
        ? static_cast<int>(std::round(static_cast<float>(width) * (1.0f - progress)))
        : static_cast<int>(std::round(static_cast<float>(width) * progress));
    SetWindowPos(g_settingsWindow, HWND_TOP, offset, 0, width, height,
                 SWP_NOACTIVATE);
    InvalidateRect(g_manager, nullptr, FALSE);
    if (t >= 1.0f) {
        KillTimer(g_manager, kPanelFlipTimer);
        g_flipAnimating = false;
        SetPanelFace(g_flipTargetSettings);
        SetFocus(g_flipTargetSettings ? g_settingsWindow : g_manager);
    }
}

void StartPanelFlip(bool settingsFace) {
    if (!g_manager || !g_settingsWindow || g_flipAnimating || settingsFace == g_settingsFace) return;
    KillTimer(g_manager, kMemoryTrimTimer);
    if (!CreateTextResources()) return;
    RECT client{};
    GetClientRect(g_manager, &client);
    g_flipTargetSettings = settingsFace;
    g_flipAnimating = true;
    g_panelFlipStarted = GetTickCount64();
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (settingsFace) {
        SetWindowPos(g_settingsWindow, HWND_TOP, width, 0, width, height,
                     SWP_NOACTIVATE);
        ShowWindow(g_settingsWindow, SW_SHOWNOACTIVATE);
    } else {
        if (g_search) ShowWindow(g_search, SW_SHOW);
        SetWindowPos(g_settingsWindow, HWND_TOP, 0, 0, width, height,
                     SWP_NOACTIVATE);
        ShowWindow(g_settingsWindow, SW_SHOWNOACTIVATE);
    }
    SetTimer(g_manager, kPanelFlipTimer, 16, nullptr);
}

void ShowInfo(InfoKind kind) {
    if (!g_infoWindow) return;
    KillTimer(g_manager, kMemoryTrimTimer);
    if (!CreateTextResources()) return;
    g_infoKind = kind;
    g_dark = IsDarkTheme();
    const UINT dpi = GetDpiForWindow(g_infoWindow);
    const int width = MulDiv(kind == InfoKind::Help ? 420 : 340, dpi, 96);
    const int height = MulDiv(kind == InfoKind::Help ? 300 : 190, dpi, 96);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(g_manager, MONITOR_DEFAULTTONEAREST), &monitor);
    const int left = monitor.rcWork.left +
                     (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int top = monitor.rcWork.top +
                    (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    SetWindowTextW(g_infoWindow, kind == InfoKind::Help ? L"帮助" : L"关于");
    SetWindowPos(g_infoWindow, HWND_TOP, left, top, width, height, SWP_NOACTIVATE);
    ApplyWindowAppearance(g_infoWindow, false);
    ShowWindow(g_infoWindow, SW_SHOW);
    SetForegroundWindow(g_infoWindow);
    InvalidateRect(g_infoWindow, nullptr, FALSE);
}

void HideInfo() {
    ShowWindow(g_infoWindow, SW_HIDE);
    g_infoCloseHovered = false;
    g_infoClosePressed = false;
    g_infoSurface.Reset();
    SetTimer(g_manager, kMemoryTrimTimer, 1800, nullptr);
}

void HideQuickPanel() {
    KillTimer(g_popup, kAnimationTimer);
    ShowWindow(g_popup, SW_HIDE);
    g_popupSurface.Reset();
    SetTimer(g_manager, kMemoryTrimTimer, 1800, nullptr);
}

void ShowQuickPanel() {
    if (IsWindowVisible(g_popup)) {
        HideQuickPanel();
        return;
    }
    KillTimer(g_manager, kMemoryTrimTimer);
    if (!CreateTextResources()) return;
    g_dark = IsDarkTheme();
    ApplyWindowAppearance(g_popup, true);
    g_pasteTarget = GetForegroundWindow();
    g_popupSection = g_store.History().empty() ? 0 : 1;
    g_popupIndex = 0;
    g_popupHoverSection = -1;
    g_popupPressedSection = -1;
    g_favoriteScroll = 0;

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const int workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const int workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    const UINT dpi = GetDpiForWindow(g_popup);
    const int height = std::min(MulDiv(668, dpi, 96), workHeight - 24);
    const int heightDip = MulDiv(height, 96, dpi);
    const int targetRows = std::max(1, (heightDip - 29 - 8) / 21);
    g_popupSplitHistory = g_store.History().size() > static_cast<std::size_t>(targetRows);
    const int targetWidthDip = g_popupSplitHistory ? 920 : 680;
    const int width = std::min(MulDiv(targetWidthDip, dpi, 96), workWidth - 24);
    const int gap = MulDiv(12, dpi, 96);
    const int edge = MulDiv(8, dpi, 96);
    const bool opensRight = cursor.x + gap + width <= monitor.rcWork.right - edge;
    int left = opensRight ? cursor.x + gap : cursor.x - gap - width;
    left = std::clamp(left, static_cast<int>(monitor.rcWork.left) + edge,
                      static_cast<int>(monitor.rcWork.right) - edge - width);
    int top = cursor.y - MulDiv(20, dpi, 96);
    top = std::clamp(top, static_cast<int>(monitor.rcWork.top) + edge,
                     static_cast<int>(monitor.rcWork.bottom) - edge - height);
    g_popupFinalRect = {left, top, left + width, top + height};
    g_popupAnimationOrigin = {
        opensRight ? left : left + width,
        std::clamp(static_cast<int>(cursor.y), top, top + height)};

    constexpr float startScale = 0.98f;
    const int startLeft = static_cast<int>(g_popupAnimationOrigin.x +
        (g_popupFinalRect.left - g_popupAnimationOrigin.x) * startScale);
    const int startTop = static_cast<int>(g_popupAnimationOrigin.y +
        (g_popupFinalRect.top - g_popupAnimationOrigin.y) * startScale);
    const int startRight = static_cast<int>(g_popupAnimationOrigin.x +
        (g_popupFinalRect.right - g_popupAnimationOrigin.x) * startScale);
    const int startBottom = static_cast<int>(g_popupAnimationOrigin.y +
        (g_popupFinalRect.bottom - g_popupAnimationOrigin.y) * startScale);
    SetWindowPos(g_popup, HWND_TOPMOST, startLeft, startTop,
                 startRight - startLeft, startBottom - startTop,
                 SWP_SHOWWINDOW);
    SetForegroundWindow(g_popup);
    SetFocus(g_popup);
    g_animationStarted = GetTickCount64();
    SetTimer(g_popup, kAnimationTimer, 16, nullptr);
    InvalidateRect(g_popup, nullptr, FALSE);
}

void AnimateQuickPanel() {
    constexpr float durationMs = 170.0f;
    const float elapsed = static_cast<float>(GetTickCount64() - g_animationStarted);
    const float t = std::clamp(elapsed / durationMs, 0.0f, 1.0f);
    constexpr float omega = 7.0f;
    const float raw = 1.0f - (1.0f + omega * t) * std::exp(-omega * t);
    const float normalizer = 1.0f - (1.0f + omega) * std::exp(-omega);
    const float progress = raw / normalizer;
    const float scale = 0.98f + 0.02f * progress;
    const int left = static_cast<int>(g_popupAnimationOrigin.x +
        (g_popupFinalRect.left - g_popupAnimationOrigin.x) * scale);
    const int top = static_cast<int>(g_popupAnimationOrigin.y +
        (g_popupFinalRect.top - g_popupAnimationOrigin.y) * scale);
    const int right = static_cast<int>(g_popupAnimationOrigin.x +
        (g_popupFinalRect.right - g_popupAnimationOrigin.x) * scale);
    const int bottom = static_cast<int>(g_popupAnimationOrigin.y +
        (g_popupFinalRect.bottom - g_popupAnimationOrigin.y) * scale);
    SetWindowPos(g_popup, HWND_TOPMOST, left, top, right - left, bottom - top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (t >= 1.0f) KillTimer(g_popup, kAnimationTimer);
}

void SendPaste() {
    if (!g_pasteTarget || !IsWindow(g_pasteTarget) || g_pasteTarget == g_manager ||
        g_pasteTarget == g_settingsWindow || g_pasteTarget == g_infoWindow) return;
    SetForegroundWindow(g_pasteTarget);
    INPUT input[4]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 'V';
    input[2] = input[1];
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3] = input[0];
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, input, sizeof(INPUT));
}

void UseEntry(int section, std::size_t index, bool paste) {
    const auto& entries = section == 0 ? g_store.Favorites() : g_store.History();
    if (index >= entries.size()) return;
    const std::wstring text = entries[index].text;
    if (!SetClipboardText(text)) return;
    g_store.AddHistory(text);
    g_store.Save();
    HideQuickPanel();
    InvalidateRect(g_manager, nullptr, FALSE);
    if (paste && g_appSettings.autoPaste && g_pasteTarget && g_pasteTarget != g_manager &&
        g_pasteTarget != g_settingsWindow && g_pasteTarget != g_infoWindow) {
        SetTimer(g_manager, kPasteTimer, 45, nullptr);
    } else if (IsWindowVisible(g_manager)) {
        SetStatus(L"已复制到剪切板");
    }
}

std::wstring KeyLabel(std::size_t index) {
    if (index < 9) return std::wstring(1, static_cast<wchar_t>(L'1' + index));
    if (index == 9) return L"0";
    if (index < 36) return std::wstring(1, static_cast<wchar_t>(L'A' + index - 10));
    return {};
}

void DrawManager(HWND window) {
    if (!EnsureSurface(window, g_managerSurface)) return;
    const Theme theme = CurrentTheme();
    const ManagerLayout layout = GetManagerLayout(window);
    auto& surface = g_managerSurface;
    surface.target->BeginDraw();
    surface.target->Clear(theme.background);
    DrawWindowTitleBar(surface, window, kProductName,
                       g_managerCloseHovered, g_managerClosePressed, theme);

    DrawSettingsIconButton(surface, layout.settingsButton,
                           g_managerHoverCommand == kCommandSettings,
                           g_managerPressedCommand == kCommandSettings, theme);

    FillRoundRect(surface, D2D1::RectF(12, 52, layout.width - 12, 80), 7, theme.surface);
    DrawRoundRect(surface, D2D1::RectF(12, 52, layout.width - 12, 80), 7, theme.divider);
    FillRoundRect(surface, layout.favorites, 8, theme.surface);
    FillRoundRect(surface, layout.history, 8, theme.surface);
    FillRoundRect(surface, layout.preview, 8, theme.surface);

    const auto favoriteIndexes = Filtered(g_store.Favorites());
    const auto historyIndexes = Filtered(g_store.History());
    auto drawSectionTitle = [&](D2D1_RECT_F area, float top, const std::wstring& title,
                                std::size_t count, std::size_t limit) {
        DrawTextValue(surface, title, g_headerFormat,
                      D2D1::RectF(area.left + 12, top, area.right - 92, top + 22), theme.text);
        const auto pill = D2D1::RectF(area.right - 82, top - 1, area.right - 12, top + 19);
        FillRoundRect(surface, pill, 10, theme.background);
        g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawTextValue(surface, std::to_wstring(count) + L" / " + std::to_wstring(limit),
                      g_monoFormat, pill, theme.muted);
        g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    };
    drawSectionTitle(layout.favorites, layout.favoriteTitleY, L"收藏夹",
                     favoriteIndexes.size(), g_store.FavoriteLimit());
    drawSectionTitle(layout.history, layout.historyTitleY, L"活动列表",
                     historyIndexes.size(), g_store.HistoryLimit());
    DrawTextValue(surface, L"文本预览", g_headerFormat,
                  D2D1::RectF(layout.preview.left + 12, layout.previewTitleY,
                              layout.preview.right - 80, layout.previewTitleY + 22), theme.text);
    const bool canCopy = g_selectedIndex <
        (g_selectedSection == 0 ? g_store.Favorites().size() : g_store.History().size());
    FillRoundRect(surface, layout.previewCopyButton, 11,
                  canCopy ? theme.selected : theme.background);
    g_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawTextValue(surface, L"复制", g_smallFormat, layout.previewCopyButton,
                  canCopy ? theme.accent : theme.muted);
    g_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    auto drawColumnHeader = [&](D2D1_RECT_F area, float rowsY, bool favorites) {
        const auto band = D2D1::RectF(area.left + 10, rowsY - 18, area.right - 10, rowsY);
        const float indexRight = band.left + 30;
        const float countLeft = band.right - 62;
        DrawTextValue(surface, L"#", g_smallFormat,
                      D2D1::RectF(band.left + 4, band.top, indexRight, band.bottom), theme.muted);
        if (favorites) {
            const float aliasRight = indexRight + 76;
            DrawTextValue(surface, L"别名", g_smallFormat,
                          D2D1::RectF(indexRight + 4, band.top, aliasRight, band.bottom), theme.muted);
            DrawTextValue(surface, L"摘要", g_smallFormat,
                          D2D1::RectF(aliasRight + 8, band.top, countLeft, band.bottom), theme.muted);
        } else {
            DrawTextValue(surface, L"摘要", g_smallFormat,
                          D2D1::RectF(indexRight + 4, band.top, countLeft, band.bottom), theme.muted);
        }
        g_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawTextValue(surface, L"字数", g_smallFormat,
                      D2D1::RectF(countLeft, band.top, band.right - 24, band.bottom), theme.muted);
        g_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    drawColumnHeader(layout.favorites, layout.favoriteRowsY, true);
    drawColumnHeader(layout.history, layout.historyRowsY, false);

    const int favoriteVisible = std::max(
        0, static_cast<int>((layout.favorites.bottom - layout.favoriteRowsY - 3) / layout.rowHeight));
    const int historyVisible = std::max(
        0, static_cast<int>((layout.history.bottom - layout.historyRowsY - 3) / layout.rowHeight));
    const std::size_t favoriteMaximumScroll =
        favoriteIndexes.size() > static_cast<std::size_t>(favoriteVisible)
            ? favoriteIndexes.size() - static_cast<std::size_t>(favoriteVisible) : 0;
    const std::size_t historyMaximumScroll =
        historyIndexes.size() > static_cast<std::size_t>(historyVisible)
            ? historyIndexes.size() - static_cast<std::size_t>(historyVisible) : 0;
    g_managerFavoriteScroll = std::min(g_managerFavoriteScroll, favoriteMaximumScroll);
    g_managerHistoryScroll = std::min(g_managerHistoryScroll, historyMaximumScroll);

    auto drawRows = [&](const std::vector<ClipEntry>& entries,
                        const std::vector<std::size_t>& indexes, D2D1_RECT_F area,
                        float rowsY, int visibleRows, std::size_t scroll, int section) {
        const std::size_t count = std::min(
            indexes.size() - std::min(scroll, indexes.size()), static_cast<std::size_t>(visibleRows));
        const float innerLeft = area.left + 10;
        const float innerRight = area.right - 10;
        const float indexRight = innerLeft + 30;
        const float countLeft = innerRight - 62;
        const float aliasRight = indexRight + 76;
        const float summaryLeft = section == 0 ? aliasRight + 8 : indexRight + 4;
        for (std::size_t row = 0; row < count; ++row) {
            const std::size_t displayed = scroll + row;
            const std::size_t original = indexes[displayed];
            const float top = rowsY + static_cast<float>(row) * layout.rowHeight;
            const auto rowRect = D2D1::RectF(innerLeft, top, innerRight,
                                             top + layout.rowHeight - 4);
            const bool selected = g_selectedSection == section && g_selectedIndex == original;
            const bool hovered = g_managerHoverSection == section && g_managerHoverIndex == original;
            if (selected || hovered) {
                FillRoundRect(surface, rowRect, 7, selected ? theme.selected : theme.hover);
            }
            if (selected) {
                FillRoundRect(surface, D2D1::RectF(rowRect.left, rowRect.top + 3,
                                                   rowRect.left + 2.5f, rowRect.bottom - 3),
                              1.25f, theme.accent);
            }
            const auto color = selected ? theme.selectedText : theme.text;
            DrawTextValue(surface, RowNumber(original), g_monoFormat,
                          D2D1::RectF(innerLeft + 7, top + 5, indexRight,
                                      top + layout.rowHeight), selected ? theme.accent : theme.muted);
            if (section == 0) {
                const auto aliasPill = D2D1::RectF(indexRight + 2, top + 3,
                                                   aliasRight - 4, top + layout.rowHeight - 7);
                FillRoundRect(surface, aliasPill, 5, selected ? theme.background : theme.hover);
                DrawTextValue(surface, FavoriteAlias(entries[original].text), g_smallFormat,
                              D2D1::RectF(aliasPill.left + 5, aliasPill.top + 2,
                                          aliasPill.right - 4, aliasPill.bottom), theme.accent);
            }
            DrawTextValue(surface, Summary(entries[original].text, section == 0 ? 12 : 18), g_bodyFormat,
                          D2D1::RectF(summaryLeft, top + 4, countLeft - 6,
                                      top + layout.rowHeight - 4), color);
            g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            DrawTextValue(surface, std::to_wstring(entries[original].text.size()), g_monoFormat,
                          D2D1::RectF(countLeft, top + 5, innerRight - 26,
                                      top + layout.rowHeight), selected ? theme.selectedText : theme.muted);
            g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if (section == 0 || hovered) {
                g_headerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DrawTextValue(surface, section == 0 ? L"★" : L"☆", g_headerFormat,
                              D2D1::RectF(innerRight - 24, top + 1, innerRight,
                                          top + layout.rowHeight - 3), theme.accent);
                g_headerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                g_headerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
        if (indexes.empty()) {
            DrawTextValue(surface, section == 0 ? L"还没有收藏内容" : L"复制的文本会出现在这里",
                          g_smallFormat, D2D1::RectF(area.left + 18, rowsY + 14,
                                                    area.right - 12, rowsY + 42), theme.muted);
        }
        if (indexes.size() > static_cast<std::size_t>(visibleRows) && visibleRows > 0) {
            const float trackTop = rowsY + 3;
            const float trackBottom = area.bottom - 6;
            const float trackHeight = trackBottom - trackTop;
            const float handleHeight = std::max(
                24.0f, trackHeight * static_cast<float>(visibleRows) / static_cast<float>(indexes.size()));
            const std::size_t maximumScroll = indexes.size() - static_cast<std::size_t>(visibleRows);
            const float offset = maximumScroll == 0 ? 0.0f
                : (trackHeight - handleHeight) * static_cast<float>(scroll) /
                    static_cast<float>(maximumScroll);
            FillRoundRect(surface,
                          D2D1::RectF(area.right - 5, trackTop + offset,
                                      area.right - 2, trackTop + offset + handleHeight),
                          1.5f, theme.muted);
        }
    };
    drawRows(g_store.Favorites(), favoriteIndexes, layout.favorites, layout.favoriteRowsY,
             favoriteVisible, g_managerFavoriteScroll, 0);
    drawRows(g_store.History(), historyIndexes, layout.history, layout.historyRowsY,
             historyVisible, g_managerHistoryScroll, 1);

    FillRoundRect(surface, layout.previewBody, 7, theme.hover);
    DrawRoundRect(surface, layout.previewBody, 7, theme.divider);
    const auto& selectedEntries = g_selectedSection == 0 ? g_store.Favorites() : g_store.History();
    if (g_selectedIndex < selectedEntries.size()) {
        DrawTextValue(surface, selectedEntries[g_selectedIndex].text, g_bodyFormat,
                      D2D1::RectF(layout.previewBody.left + 10, layout.previewBody.top + 9,
                                  layout.previewBody.right - 10,
                                  layout.previewBody.bottom - (g_statusText.empty() ? 8 : 24)), theme.text);
    } else {
        g_bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawTextValue(surface, L"选择一条内容进行预览", g_bodyFormat,
                      layout.previewBody, theme.muted);
        g_bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (!g_statusText.empty()) {
        DrawTextValue(surface, g_statusText, g_smallFormat,
                      D2D1::RectF(layout.previewBody.left + 10, layout.previewBody.bottom - 22,
                                  layout.previewBody.right - 10, layout.previewBody.bottom - 4),
                      theme.accent);
    }

    const HRESULT result = surface.target->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) surface.Reset();
}

void DrawPopup(HWND window) {
    if (!EnsureSurface(window, g_popupSurface)) return;
    const Theme theme = CurrentTheme();
    const PopupLayout layout = GetPopupLayout(window);
    auto& surface = g_popupSurface;
    surface.target->BeginDraw();
    surface.target->Clear(theme.background);
    FillRect(surface, D2D1::RectF(0, 0, layout.width, layout.headerHeight), theme.surface);
    DrawTextValue(surface, L"收藏", g_headerFormat,
                  D2D1::RectF(16, 5, layout.favoriteWidth - 12, 27), theme.text);
    DrawTextValue(surface, L"最近记录", g_headerFormat,
                  D2D1::RectF(layout.favoriteWidth + 16, 5,
                               layout.favoriteWidth + layout.firstHistoryWidth - 12, 27), theme.text);
    if (layout.splitHistory) {
        DrawTextValue(surface, L"较早记录", g_headerFormat,
                      D2D1::RectF(layout.favoriteWidth + layout.firstHistoryWidth + 16, 5,
                                  layout.width - 12, 27), theme.text);
    }
    DrawLine(surface, D2D1::Point2F(0, layout.headerHeight - 0.5f),
             D2D1::Point2F(layout.width, layout.headerHeight - 0.5f), theme.divider);
    DrawLine(surface, D2D1::Point2F(layout.favoriteWidth, 0),
             D2D1::Point2F(layout.favoriteWidth, layout.height), theme.divider);
    if (layout.splitHistory) {
        DrawLine(surface, D2D1::Point2F(layout.favoriteWidth + layout.firstHistoryWidth, 0),
                 D2D1::Point2F(layout.favoriteWidth + layout.firstHistoryWidth, layout.height),
                 theme.divider);
    }

    const auto& favorites = g_store.Favorites();
    const std::size_t favoriteEnd = std::min(favorites.size(), g_favoriteScroll + layout.rows);
    for (std::size_t index = g_favoriteScroll; index < favoriteEnd; ++index) {
        const int row = static_cast<int>(index - g_favoriteScroll);
        const float top = layout.headerHeight + 4 + row * layout.rowHeight;
        const auto rectangle = D2D1::RectF(8, top, layout.favoriteWidth - 8, top + layout.rowHeight - 1);
        const bool selected = g_popupSection == 0 && g_popupIndex == index;
        const bool hover = g_popupHoverSection == 0 && g_popupHoverIndex == index;
        if (selected || hover) FillRoundRect(surface, rectangle, 5, selected ? theme.selected : theme.hover);
        DrawTextValue(surface, Summary(favorites[index].text, 42), g_bodyFormat,
                      D2D1::RectF(rectangle.left + 10, rectangle.top + 2,
                                  rectangle.right - 6, rectangle.bottom - 1),
                      selected ? theme.selectedText : theme.text);
    }

    const auto& history = g_store.History();
    const int historyColumns = layout.splitHistory ? 2 : 1;
    const std::size_t maximum = std::min(
        history.size(), static_cast<std::size_t>(layout.rows * historyColumns));
    for (std::size_t index = 0; index < maximum; ++index) {
        const int column = static_cast<int>(index / layout.rows);
        const int row = static_cast<int>(index % layout.rows);
        const float left = layout.favoriteWidth +
                           (column == 0 ? 0.0f : layout.firstHistoryWidth);
        const float columnWidth = column == 0 ? layout.firstHistoryWidth : layout.secondHistoryWidth;
        const float top = layout.headerHeight + 4 + row * layout.rowHeight;
        const auto rectangle = D2D1::RectF(left + 8, top,
                                           left + columnWidth - 8, top + layout.rowHeight - 1);
        const bool selected = g_popupSection == 1 && g_popupIndex == index;
        const bool hover = g_popupHoverSection == 1 && g_popupHoverIndex == index;
        if (selected || hover) FillRoundRect(surface, rectangle, 5, selected ? theme.selected : theme.hover);
        const std::wstring key = KeyLabel(index);
        if (!key.empty()) {
            DrawTextValue(surface, key, g_smallFormat,
                           D2D1::RectF(rectangle.left + 7, rectangle.top + 2,
                                       rectangle.left + 27, rectangle.bottom),
                          selected ? theme.selectedText : theme.accent);
        }
        DrawTextValue(surface, Summary(history[index].text, 54), g_bodyFormat,
                      D2D1::RectF(rectangle.left + (key.empty() ? 9 : 30), rectangle.top + 2,
                                  rectangle.right - 6, rectangle.bottom - 1),
                      selected ? theme.selectedText : theme.text);
    }
    if (favorites.empty()) {
        DrawTextValue(surface, L"管理器中可添加收藏", g_smallFormat,
                      D2D1::RectF(18, layout.headerHeight + 18, layout.favoriteWidth - 12,
                                  layout.headerHeight + 48), theme.muted);
    }
    if (history.empty()) {
        DrawTextValue(surface, L"复制文本后会出现在这里", g_smallFormat,
                      D2D1::RectF(layout.favoriteWidth + 18, layout.headerHeight + 18,
                                  layout.width - 12, layout.headerHeight + 48), theme.muted);
    }
    const HRESULT result = surface.target->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) surface.Reset();
}

void DrawSettingsStepper(Surface& surface, const std::wstring& label, std::size_t value,
                         D2D1_RECT_F minus, D2D1_RECT_F plus, int minusControl,
                         int plusControl, const Theme& theme) {
    DrawTextValue(surface, label, g_bodyFormat,
                  D2D1::RectF(28, minus.top + 3, minus.left - 12, plus.bottom), theme.text);
    DrawButton(surface, minus, L"−", theme, true,
               g_settingsHoverControl == minusControl, g_settingsPressedControl == minusControl);
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DrawTextValue(surface, std::to_wstring(value), g_monoFormat,
                  D2D1::RectF(minus.right + 4, minus.top, plus.left - 4, plus.bottom), theme.text);
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_monoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DrawButton(surface, plus, L"+", theme, true,
               g_settingsHoverControl == plusControl, g_settingsPressedControl == plusControl);
}

void DrawSettings(HWND window) {
    if (!EnsureSurface(window, g_settingsSurface)) return;
    const Theme theme = CurrentTheme();
    const SettingsLayout layout = GetSettingsLayout(window);
    auto& surface = g_settingsSurface;
    surface.target->BeginDraw();
    surface.target->Clear(theme.background);
    DrawWindowTitleBar(surface, window, L"设置",
                       g_settingsCloseHovered, g_settingsClosePressed, theme);
    DrawBackIconButton(surface, SettingsBackButtonRect(window), g_settingsBackHovered,
                       g_settingsBackPressed, theme);

    DrawTextValue(surface, L"快捷操作", g_smallFormat,
                  D2D1::RectF(20, 52, layout.width - 20, 70), theme.muted);
    FillRoundRect(surface, layout.hotkeyButton, 8,
                  g_settingsHoverControl == kSettingsHotkey ? theme.hover : theme.surface);
    DrawTextValue(surface, L"呼出快捷面板", g_bodyFormat,
                  D2D1::RectF(28, layout.hotkeyButton.top + 15, layout.width - 160,
                              layout.hotkeyButton.bottom - 8), theme.text);
    DrawHotkeyCaps(surface, layout.hotkeyButton, theme);

    const auto behaviorGroup = D2D1::RectF(16, 132, layout.width - 16, 304);
    FillRoundRect(surface, behaviorGroup, 8, theme.surface);
    if (g_settingsHoverControl == kSettingsAutoPaste) {
        FillRoundRect(surface, layout.autoPasteToggle, 8, theme.hover);
    } else if (g_settingsHoverControl == kSettingsOpacity) {
        FillRoundRect(surface, layout.opacitySlider, 8, theme.hover);
    }
    DrawLine(surface, D2D1::Point2F(28, 180.5f), D2D1::Point2F(layout.width - 28, 180.5f),
             theme.divider);
    DrawLine(surface, D2D1::Point2F(28, 220.5f), D2D1::Point2F(layout.width - 28, 220.5f),
             theme.divider);
    DrawLine(surface, D2D1::Point2F(28, 260.5f), D2D1::Point2F(layout.width - 28, 260.5f),
             theme.divider);
    DrawTextValue(surface, L"选中后自动粘贴", g_bodyFormat,
                  D2D1::RectF(28, 141, layout.width - 80, 163), theme.text);
    DrawTextValue(surface, L"选中记录后立即粘贴到光标处", g_smallFormat,
                  D2D1::RectF(28, 160, layout.width - 80, 177), theme.muted);
    DrawToggleControl(surface, layout.autoPasteToggle, g_appSettings.autoPaste, theme,
                      g_settingsHoverControl == kSettingsAutoPaste);
    DrawSettingsStepper(surface, L"活动记录上限", g_appSettings.historyLimit,
                        layout.historyMinus, layout.historyPlus,
                        kSettingsHistoryMinus, kSettingsHistoryPlus, theme);
    DrawSettingsStepper(surface, L"收藏上限", g_appSettings.favoriteLimit,
                        layout.favoriteMinus, layout.favoritePlus,
                        kSettingsFavoriteMinus, kSettingsFavoritePlus, theme);

    DrawTextValue(surface, L"界面背景透明度", g_bodyFormat,
                  D2D1::RectF(28, 273, layout.width - 154, 297), theme.text);
    const float sliderLeft = layout.width - 142;
    const float sliderRight = layout.width - 62;
    const float sliderY = 282.0f;
    const float opacityProgress = static_cast<float>(g_appSettings.windowOpacity - 65) / 35.0f;
    const float thumbX = sliderLeft + (sliderRight - sliderLeft) * opacityProgress;
    FillRoundRect(surface, D2D1::RectF(sliderLeft, sliderY - 2, sliderRight, sliderY + 2),
                  2, theme.divider);
    FillRoundRect(surface, D2D1::RectF(sliderLeft, sliderY - 2, thumbX, sliderY + 2),
                  2, theme.accent);
    FillCircle(surface, D2D1::Point2F(thumbX, sliderY), g_draggingOpacity ? 7.0f : 6.0f,
               D2D1::ColorF(0xFFFFFF));
    surface.brush->SetColor(theme.accent);
    surface.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, sliderY),
                                               g_draggingOpacity ? 7.0f : 6.0f,
                                               g_draggingOpacity ? 7.0f : 6.0f), surface.brush, 1.2f);
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawTextValue(surface, std::to_wstring(g_appSettings.windowOpacity) + L"%", g_monoFormat,
                  D2D1::RectF(sliderRight + 6, 274, layout.width - 24, 296), theme.muted);
    g_monoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    DrawTextValue(surface, L"系统行为", g_smallFormat,
                  D2D1::RectF(20, 316, layout.width - 20, 334), theme.muted);
    const auto systemGroup = D2D1::RectF(16, 338, layout.width - 16, 470);
    FillRoundRect(surface, systemGroup, 8, theme.surface);
    if (g_settingsHoverControl == kSettingsMonitor) {
        FillRoundRect(surface, layout.monitorToggle, 8, theme.hover);
    } else if (g_settingsHoverControl == kSettingsStartup) {
        FillRect(surface, layout.startupToggle, theme.hover);
    } else if (g_settingsHoverControl == kSettingsLaunchHidden) {
        FillRoundRect(surface, layout.launchHiddenToggle, 8, theme.hover);
    }
    DrawLine(surface, D2D1::Point2F(28, 382.5f), D2D1::Point2F(layout.width - 28, 382.5f),
             theme.divider);
    DrawLine(surface, D2D1::Point2F(28, 426.5f), D2D1::Point2F(layout.width - 28, 426.5f),
             theme.divider);
    DrawTextValue(surface, L"监听剪贴板", g_bodyFormat,
                  D2D1::RectF(28, 351, layout.width - 80, 375), theme.text);
    DrawTextValue(surface, L"开机自启", g_bodyFormat,
                  D2D1::RectF(28, 395, layout.width - 80, 419), theme.text);
    DrawTextValue(surface, L"启动时隐藏管理器", g_bodyFormat,
                  D2D1::RectF(28, 439, layout.width - 80, 463), theme.text);
    DrawToggleControl(surface, layout.monitorToggle, g_appSettings.monitorClipboard, theme,
                      g_settingsHoverControl == kSettingsMonitor);
    DrawToggleControl(surface, layout.startupToggle, IsStartupEnabled(), theme,
                      g_settingsHoverControl == kSettingsStartup);
    DrawToggleControl(surface, layout.launchHiddenToggle, g_appSettings.launchHidden, theme,
                      g_settingsHoverControl == kSettingsLaunchHidden);
    const bool hasStatus = !g_settingsStatus.empty();
    const auto status = D2D1::RectF(16, 490, layout.width - 16, layout.height - 14);
    FillRoundRect(surface, status, 7, hasStatus ? theme.selected : theme.surface);
    DrawTextValue(surface,
                  hasStatus ? L"✓  " + g_settingsStatus : L"设置会立即保存到当前用户配置。",
                  g_smallFormat,
                  D2D1::RectF(status.left + 12, status.top + 8,
                              status.right - 10, status.bottom - 4),
                  hasStatus ? theme.accent : theme.muted);
    const HRESULT result = surface.target->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) surface.Reset();
}

int SettingsControlAtPoint(HWND window, D2D1_POINT_2F point) {
    const SettingsLayout layout = GetSettingsLayout(window);
    if (Contains(layout.hotkeyButton, point)) return kSettingsHotkey;
    if (Contains(layout.autoPasteToggle, point)) return kSettingsAutoPaste;
    if (Contains(layout.historyMinus, point)) return kSettingsHistoryMinus;
    if (Contains(layout.historyPlus, point)) return kSettingsHistoryPlus;
    if (Contains(layout.favoriteMinus, point)) return kSettingsFavoriteMinus;
    if (Contains(layout.favoritePlus, point)) return kSettingsFavoritePlus;
    if (Contains(layout.opacitySlider, point)) return kSettingsOpacity;
    if (Contains(layout.monitorToggle, point)) return kSettingsMonitor;
    if (Contains(layout.startupToggle, point)) return kSettingsStartup;
    if (Contains(layout.launchHiddenToggle, point)) return kSettingsLaunchHidden;
    return 0;
}

void UpdateOpacityFromPoint(HWND window, D2D1_POINT_2F point) {
    const SettingsLayout layout = GetSettingsLayout(window);
    const float left = layout.width - 142;
    const float right = layout.width - 62;
    const float progress = std::clamp((point.x - left) / (right - left), 0.0f, 1.0f);
    const UINT value = 65 + static_cast<UINT>(std::round(progress * 35.0f));
    if (value == g_appSettings.windowOpacity) return;
    g_appSettings.windowOpacity = value;
    g_settingsStatus = L"界面背景透明度已调整为 " + std::to_wstring(value) + L"%";
    ApplyWindowOpacity(g_manager);
    ApplyWindowOpacity(g_popup);
    ApplyWindowOpacity(g_infoWindow);
    InvalidateRect(window, nullptr, FALSE);
}

void DrawInfo(HWND window) {
    if (!EnsureSurface(window, g_infoSurface)) return;
    const Theme theme = CurrentTheme();
    RECT client{};
    GetClientRect(window, &client);
    const float scale = DpiScale(window);
    const float width = static_cast<float>(client.right) / scale;
    const float height = static_cast<float>(client.bottom) / scale;
    auto& surface = g_infoSurface;
    surface.target->BeginDraw();
    surface.target->Clear(theme.background);
    DrawWindowTitleBar(surface, window, g_infoKind == InfoKind::Help ? L"帮助" : L"关于",
                       g_infoCloseHovered, g_infoClosePressed, theme);
    const auto card = D2D1::RectF(12, 44, width - 12, height - 12);
    FillRoundRect(surface, card, 10, theme.surface);

    if (g_infoKind == InfoKind::About) {
        g_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawTextValue(surface, kProductName, g_titleFormat,
                      D2D1::RectF(card.left + 12, 76, card.right - 12, 108), theme.text);
        g_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawTextValue(surface, std::wstring(L"版本 ") + kProductVersion, g_bodyFormat,
                      D2D1::RectF(card.left + 12, 118, card.right - 12, 146), theme.muted);
        g_bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        DrawTextValue(surface, L"快速开始", g_headerFormat,
                      D2D1::RectF(24, 56, width - 24, 80), theme.text);
        const std::array<std::pair<std::wstring, std::wstring>, 4> rows{{
            {HotkeyText(g_appSettings.hotkeyModifiers, g_appSettings.hotkeyVirtualKey), L"呼出快捷面板"},
            {L"Enter", L"粘贴所选内容"},
            {L"Ctrl + Enter", L"仅复制，不自动粘贴"},
            {L"右键列表", L"管理收藏和活动记录"},
        }};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const float top = 86.0f + static_cast<float>(index) * 46.0f;
            const auto badge = D2D1::RectF(24, top, 142, top + 30);
            FillRoundRect(surface, badge, 7, theme.hover);
            DrawTextValue(surface, rows[index].first, g_smallFormat,
                          D2D1::RectF(badge.left + 10, badge.top + 7,
                                      badge.right - 8, badge.bottom - 3), theme.accent);
            DrawTextValue(surface, rows[index].second, g_bodyFormat,
                          D2D1::RectF(156, top + 5, width - 24, top + 31), theme.text);
        }
    }
    const HRESULT result = surface.target->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) surface.Reset();
}

std::pair<int, std::size_t> PopupHitTest(HWND window, D2D1_POINT_2F point) {
    const PopupLayout layout = GetPopupLayout(window);
    if (point.y < layout.headerHeight + 5) return {-1, 0};
    const int row = static_cast<int>((point.y - layout.headerHeight - 5) / layout.rowHeight);
    if (row < 0 || row >= layout.rows) return {-1, 0};
    if (point.x < layout.favoriteWidth) {
        const std::size_t index = g_favoriteScroll + static_cast<std::size_t>(row);
        return index < g_store.Favorites().size() ? std::pair{0, index} : std::pair{-1, std::size_t{0}};
    }
    const int column = point.x < layout.favoriteWidth + layout.firstHistoryWidth ? 0 : 1;
    const std::size_t index = static_cast<std::size_t>(column * layout.rows + row);
    return index < g_store.History().size() ? std::pair{1, index} : std::pair{-1, std::size_t{0}};
}

std::optional<std::pair<int, std::size_t>> ManagerRowAtPoint(HWND window,
                                                             D2D1_POINT_2F point) {
    const ManagerLayout layout = GetManagerLayout(window);
    if (Contains(layout.favorites, point) && point.y >= layout.favoriteRowsY) {
        const float relative = point.y - layout.favoriteRowsY;
        const int row = static_cast<int>(relative / layout.rowHeight);
        const auto indexes = Filtered(g_store.Favorites());
        const std::size_t displayed = g_managerFavoriteScroll + static_cast<std::size_t>(row);
        if (row >= 0 && std::fmod(relative, layout.rowHeight) < layout.rowHeight - 4 &&
            displayed < indexes.size()) {
            return std::pair{0, indexes[displayed]};
        }
    } else if (Contains(layout.history, point) && point.y >= layout.historyRowsY) {
        const float relative = point.y - layout.historyRowsY;
        const int row = static_cast<int>(relative / layout.rowHeight);
        const auto indexes = Filtered(g_store.History());
        const std::size_t displayed = g_managerHistoryScroll + static_cast<std::size_t>(row);
        if (row >= 0 && std::fmod(relative, layout.rowHeight) < layout.rowHeight - 4 &&
            displayed < indexes.size()) {
            return std::pair{1, indexes[displayed]};
        }
    }
    return std::nullopt;
}

void SelectManagerRow(HWND window, D2D1_POINT_2F point) {
    if (const auto row = ManagerRowAtPoint(window, point)) {
        g_selectedSection = row->first;
        g_selectedIndex = row->second;
    }
    SetFocus(window);
    InvalidateRect(window, nullptr, FALSE);
}

void ShowManagerContextMenu(HWND window, D2D1_POINT_2F point, POINT screenPoint) {
    const ManagerLayout layout = GetManagerLayout(window);
    const int section = Contains(layout.favorites, point) ? 0
                      : Contains(layout.history, point) ? 1 : -1;
    if (section < 0) return;

    const auto row = ManagerRowAtPoint(window, point);
    const bool hasItem = row && row->first == section;
    if (hasItem) {
        g_selectedSection = row->first;
        g_selectedIndex = row->second;
        InvalidateRect(window, nullptr, FALSE);
    }

    HMENU menu = CreatePopupMenu();
    if (section == 1) {
        AppendMenuW(menu, MF_STRING | (hasItem ? 0 : MF_GRAYED), kCommandFavorite,
                    L"添加至收藏夹");
        AppendMenuW(menu, MF_STRING | (hasItem ? 0 : MF_GRAYED), kCommandDelete, L"删除");
        AppendMenuW(menu, MF_STRING | (g_store.History().empty() ? MF_GRAYED : 0),
                    kCommandClear, L"删除所有");
    } else {
        AppendMenuW(menu, MF_STRING | (hasItem ? 0 : MF_GRAYED), kCommandDelete, L"删除");
        AppendMenuW(menu, MF_STRING | (g_store.Favorites().empty() ? MF_GRAYED : 0),
                    kCommandClearFavorites, L"删除所有");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const bool canMoveUp = hasItem && g_selectedIndex > 0;
        const bool canMoveDown = hasItem && g_selectedIndex + 1 < g_store.Favorites().size();
        AppendMenuW(menu, MF_STRING | (canMoveUp ? 0 : MF_GRAYED), kCommandFavoriteTop, L"置顶");
        AppendMenuW(menu, MF_STRING | (canMoveUp ? 0 : MF_GRAYED), kCommandFavoriteUp, L"上移");
        AppendMenuW(menu, MF_STRING | (canMoveDown ? 0 : MF_GRAYED), kCommandFavoriteDown, L"下移");
        AppendMenuW(menu, MF_STRING | (canMoveDown ? 0 : MF_GRAYED), kCommandFavoriteBottom, L"置底");
    }
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   screenPoint.x, screenPoint.y, 0, window, nullptr);
    DestroyMenu(menu);
}

UINT ManagerToolbarCommandAtPoint(HWND window, D2D1_POINT_2F point) {
    const ManagerLayout layout = GetManagerLayout(window);
    if (Contains(layout.settingsButton, point)) return kCommandSettings;
    if (Contains(layout.previewCopyButton, point)) return kCommandCopyPreview;
    if (const auto row = ManagerRowAtPoint(window, point)) {
        if (row->first == 1 && point.x >= layout.history.right - 42) return kCommandFavorite;
    }
    return 0;
}

void PersistSettings() {
    g_store.SetLimits(g_appSettings.historyLimit, g_appSettings.favoriteLimit);
    SaveAppSettings();
    g_store.Save();
    InvalidateRect(g_manager, nullptr, FALSE);
    InvalidateRect(g_popup, nullptr, FALSE);
    InvalidateRect(g_settingsWindow, nullptr, FALSE);
}

bool RebindHotkey(UINT modifiers, UINT virtualKey) {
    const UINT oldModifiers = g_appSettings.hotkeyModifiers;
    const UINT oldVirtualKey = g_appSettings.hotkeyVirtualKey;
    if (g_hotkeyRegistered) {
        UnregisterHotKey(g_manager, kHotkeyId);
        g_hotkeyRegistered = false;
    }
    if (RegisterHotKey(g_manager, kHotkeyId, modifiers | MOD_NOREPEAT, virtualKey)) {
        g_hotkeyRegistered = true;
        g_appSettings.hotkeyModifiers = modifiers;
        g_appSettings.hotkeyVirtualKey = virtualKey;
        return true;
    }
    g_hotkeyRegistered = RegisterHotKey(g_manager, kHotkeyId,
                                       oldModifiers | MOD_NOREPEAT, oldVirtualKey) != FALSE;
    return false;
}

void HandleManagerCommand(UINT command) {
    switch (command) {
    case kCommandOpenManager:
        ShowManager();
        break;
    case kCommandOpenQuick:
        ShowQuickPanel();
        break;
    case kCommandPause:
        g_paused = !g_paused;
        g_appSettings.monitorClipboard = !g_paused;
        SaveAppSettings();
        SetStatus(g_paused ? L"已暂停记录" : L"已恢复记录");
        break;
    case kCommandStartup: {
        const bool enable = !IsStartupEnabled();
        SetStatus(SetStartupEnabled(enable)
                      ? (enable ? L"已开启开机自启" : L"已关闭开机自启")
                      : L"无法修改开机自启设置");
        break;
    }
    case kCommandFavorite:
        if (g_selectedSection == 1 && g_store.AddFavoriteFromHistory(g_selectedIndex)) {
            g_store.Save();
            SetStatus(L"已添加到收藏");
        }
        break;
    case kCommandCopyPreview: {
        const auto& entries = g_selectedSection == 0 ? g_store.Favorites() : g_store.History();
        if (g_selectedIndex < entries.size() && SetClipboardText(entries[g_selectedIndex].text)) {
            SetStatus(L"已复制到剪切板");
        }
        break;
    }
    case kCommandDelete:
        if (g_selectedSection == 0) {
            if (g_store.DeleteFavorite(g_selectedIndex)) {
                if (g_selectedIndex > 0) --g_selectedIndex;
                g_store.Save();
                SetStatus(L"已删除收藏");
            }
        } else if (g_store.DeleteHistory(g_selectedIndex)) {
            if (g_selectedIndex > 0) --g_selectedIndex;
            g_store.Save();
            SetStatus(L"已删除记录");
        }
        break;
    case kCommandClear:
        if (!g_store.History().empty() && MessageBoxW(g_manager, L"确定清空最近记录吗？收藏不会受到影响。",
                                                       kProductName, MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
            g_store.ClearHistory();
            g_selectedSection = 0;
            g_selectedIndex = 0;
            g_store.Save();
            SetStatus(L"最近记录已清空");
        }
        break;
    case kCommandSettings:
        ShowSettings();
        break;
    case kCommandHelp:
        ShowInfo(InfoKind::Help);
        break;
    case kCommandAbout:
        ShowInfo(InfoKind::About);
        break;
    case kCommandClearFavorites:
        if (!g_store.Favorites().empty() &&
            MessageBoxW(g_manager, L"确定删除全部收藏吗？此操作无法撤销。", kProductName,
                        MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
            g_store.ClearFavorites();
            g_selectedSection = 1;
            g_selectedIndex = 0;
            g_store.Save();
            SetStatus(L"收藏已清空");
        }
        break;
    case kCommandFavoriteTop:
    case kCommandFavoriteUp:
    case kCommandFavoriteDown:
    case kCommandFavoriteBottom:
        if (g_selectedSection == 0 && g_selectedIndex < g_store.Favorites().size()) {
            std::size_t target = g_selectedIndex;
            if (command == kCommandFavoriteTop) target = 0;
            else if (command == kCommandFavoriteUp && target > 0) --target;
            else if (command == kCommandFavoriteDown && target + 1 < g_store.Favorites().size()) ++target;
            else if (command == kCommandFavoriteBottom) target = g_store.Favorites().size() - 1;
            if (g_store.MoveFavorite(g_selectedIndex, target)) {
                g_selectedIndex = target;
                g_store.Save();
                SetStatus(L"收藏顺序已更新");
            }
        }
        break;
    case kCommandExit:
        g_quitting = true;
        DestroyWindow(g_manager);
        break;
    default:
        break;
    }
    InvalidateRect(g_manager, nullptr, FALSE);
    InvalidateRect(g_popup, nullptr, FALSE);
}

LRESULT CALLBACK ManagerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   20, 56, 300, 22, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchId)),
                                   g_instance, nullptr);
        SendMessageW(g_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索收藏和最近记录"));
        SendMessageW(g_search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        SetWindowTheme(g_search, L"Explorer", nullptr);
        AddClipboardFormatListener(window);
        return 0;
    }
    case WM_SIZE:
        ResizeSurface(window, g_managerSurface);
        if (g_search) {
            const UINT dpi = GetDpiForWindow(window);
            RECT client{};
            GetClientRect(window, &client);
            const int searchWidth = std::max(MulDiv(120, dpi, 96),
                                             static_cast<int>(client.right) - MulDiv(40, dpi, 96));
            SetWindowPos(g_search, nullptr, MulDiv(20, dpi, 96), MulDiv(56, dpi, 96),
                         searchWidth, MulDiv(22, dpi, 96),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SyncSettingsChild(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const auto* rectangle = reinterpret_cast<const RECT*>(lParam);
        const UINT dpi = HIWORD(wParam);
        SetWindowPos(window, nullptr, rectangle->left, rectangle->top,
                     MulDiv(340, dpi, 96), MulDiv(540, dpi, 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SyncSettingsChild(window);
        ResizeSurface(window, g_managerSurface);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {MulDiv(340, GetDpiForWindow(window), 96),
                                                                  MulDiv(540, GetDpiForWindow(window), 96)};
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxTrackSize =
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize;
        return 0;
    case WM_NCHITTEST: {
        POINT pixel{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &pixel);
        const float scale = DpiScale(window);
        const auto point = D2D1::Point2F(static_cast<float>(pixel.x) / scale,
                                        static_cast<float>(pixel.y) / scale);
        if (Contains(CloseButtonRect(window), point)) return HTCLIENT;
        const ManagerLayout layout = GetManagerLayout(window);
        if (Contains(layout.settingsButton, point)) {
            return HTCLIENT;
        }
        if (point.y >= 0 && point.y < kTitleBarHeight) return HTCAPTION;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        DrawManager(window);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const Theme theme = CurrentTheme();
        HDC context = reinterpret_cast<HDC>(wParam);
        SetTextColor(context, theme.editText);
        SetBkColor(context, theme.editBackground);
        return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kSearchId && HIWORD(wParam) == EN_CHANGE) {
            const int length = GetWindowTextLengthW(g_search);
            std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
            if (length > 0) GetWindowTextW(g_search, value.data(), length + 1);
            value.resize(static_cast<std::size_t>(length));
            g_searchText = std::move(value);
            g_managerFavoriteScroll = 0;
            g_managerHistoryScroll = 0;
            InvalidateRect(window, nullptr, FALSE);
        } else {
            HandleManagerCommand(LOWORD(wParam));
        }
        return 0;
    case WM_MOUSEMOVE: {
        const auto point = ToDip(window, lParam);
        const bool closeHovered = Contains(CloseButtonRect(window), point);
        const auto row = ManagerRowAtPoint(window, point);
        const int hoverSection = row ? row->first : -1;
        const std::size_t hoverIndex = row ? row->second : 0;
        const UINT hoverCommand = closeHovered || point.y >= kTitleBarHeight
            ? 0 : ManagerToolbarCommandAtPoint(window, point);
        if (closeHovered != g_managerCloseHovered || hoverSection != g_managerHoverSection ||
            hoverIndex != g_managerHoverIndex || hoverCommand != g_managerHoverCommand) {
            g_managerCloseHovered = closeHovered;
            g_managerHoverSection = hoverSection;
            g_managerHoverIndex = hoverIndex;
            g_managerHoverCommand = hoverCommand;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_managerCloseHovered = false;
        g_managerHoverSection = -1;
        g_managerHoverIndex = 0;
        g_managerHoverCommand = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const auto point = ToDip(window, lParam);
        if (Contains(CloseButtonRect(window), point)) {
            g_managerClosePressed = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (const auto row = ManagerRowAtPoint(window, point)) {
            g_selectedSection = row->first;
            g_selectedIndex = row->second;
            InvalidateRect(window, nullptr, FALSE);
        }
        g_managerPressedCommand = ManagerToolbarCommandAtPoint(window, point);
        if (g_managerPressedCommand) {
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        } else if (!ManagerRowAtPoint(window, point)) {
            SetFocus(window);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_managerClosePressed) {
            const bool close = Contains(CloseButtonRect(window), ToDip(window, lParam));
            g_managerClosePressed = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (close) PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_managerPressedCommand) {
            const UINT pending = g_managerPressedCommand;
            const UINT releasedOn = ManagerToolbarCommandAtPoint(window, ToDip(window, lParam));
            g_managerPressedCommand = 0;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (pending == releasedOn) HandleManagerCommand(pending);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_managerClosePressed) {
            g_managerClosePressed = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (g_managerPressedCommand) {
            g_managerPressedCommand = 0;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const auto point = ToDip(window, lParam);
        ClientToScreen(window, &screen);
        ShowManagerContextMenu(window, point, screen);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        SelectManagerRow(window, ToDip(window, lParam));
        const auto& entries = g_selectedSection == 0 ? g_store.Favorites() : g_store.History();
        if (g_selectedIndex < entries.size() && SetClipboardText(entries[g_selectedIndex].text)) {
            SetStatus(L"已复制到剪切板");
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pixel{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &pixel);
        const float scale = DpiScale(window);
        const auto point = D2D1::Point2F(static_cast<float>(pixel.x) / scale,
                                        static_cast<float>(pixel.y) / scale);
        const ManagerLayout layout = GetManagerLayout(window);
        const int direction = GET_WHEEL_DELTA_WPARAM(wParam) < 0 ? 3 : -3;
        if (Contains(layout.favorites, point)) {
            const auto indexes = Filtered(g_store.Favorites());
            const int visible = std::max(
                0, static_cast<int>((layout.favorites.bottom - layout.favoriteRowsY - 3) /
                                    layout.rowHeight));
            const std::size_t maximum = indexes.size() > static_cast<std::size_t>(visible)
                ? indexes.size() - static_cast<std::size_t>(visible) : 0;
            const long long next = static_cast<long long>(g_managerFavoriteScroll) + direction;
            g_managerFavoriteScroll = static_cast<std::size_t>(
                std::clamp(next, 0LL, static_cast<long long>(maximum)));
        } else if (Contains(layout.history, point)) {
            const auto indexes = Filtered(g_store.History());
            const int visible = std::max(
                0, static_cast<int>((layout.history.bottom - layout.historyRowsY - 3) /
                                    layout.rowHeight));
            const std::size_t maximum = indexes.size() > static_cast<std::size_t>(visible)
                ? indexes.size() - static_cast<std::size_t>(visible) : 0;
            const long long next = static_cast<long long>(g_managerHistoryScroll) + direction;
            g_managerHistoryScroll = static_cast<std::size_t>(
                std::clamp(next, 0LL, static_cast<long long>(maximum)));
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_DELETE) HandleManagerCommand(kCommandDelete);
        else if (wParam == VK_RETURN) {
            const auto& entries = g_selectedSection == 0 ? g_store.Favorites() : g_store.History();
            if (g_selectedIndex < entries.size() && SetClipboardText(entries[g_selectedIndex].text)) {
                SetStatus(L"已复制到剪切板");
            }
        }
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (!g_paused) {
            if (auto text = ReadClipboardText(window)) {
                g_clipboardRetries = 0;
                if (g_store.AddHistory(std::move(*text))) {
                    g_store.Save();
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(g_popup, nullptr, FALSE);
                }
            } else if (g_clipboardRetries < 5) {
                ++g_clipboardRetries;
                SetTimer(window, kClipboardRetryTimer, 30, nullptr);
            }
        }
        return 0;
    case WM_TIMER:
        if (wParam == kPasteTimer) {
            KillTimer(window, kPasteTimer);
            SendPaste();
        } else if (wParam == kStatusTimer) {
            KillTimer(window, kStatusTimer);
            g_statusText.clear();
            InvalidateRect(window, nullptr, FALSE);
        } else if (wParam == kClipboardRetryTimer) {
            KillTimer(window, kClipboardRetryTimer);
            PostMessageW(window, WM_CLIPBOARDUPDATE, 0, 0);
        } else if (wParam == kPanelFlipTimer) {
            AnimatePanelFlip();
        } else if (wParam == kMemoryTrimTimer) {
            KillTimer(window, kMemoryTrimTimer);
            if (!IsWindowVisible(g_manager) && !IsWindowVisible(g_popup) &&
                !IsWindowVisible(g_settingsWindow) && !IsWindowVisible(g_infoWindow)) {
                ReleaseGraphicsResources();
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                         static_cast<SIZE_T>(-1));
            }
        }
        return 0;
    case WM_HOTKEY:
        if (wParam == kHotkeyId) ShowQuickPanel();
        return 0;
    case kTrayMessage: {
        const UINT event = LOWORD(lParam);
        if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK ||
            event == NIN_SELECT || event == NIN_KEYSELECT) {
            ShowManager();
        }
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
        }
        return 0;
    }
    case kShowExistingMessage:
        ShowManager();
        return 0;
    case WM_SETTINGCHANGE:
        g_dark = IsDarkTheme();
        DeleteObject(g_editBrush);
        const Theme theme = CurrentTheme();
        g_editBrush = CreateSolidBrush(theme.editBackground);
        ApplyWindowAppearance(window, false);
        ApplyWindowAppearance(g_popup, true);
        ApplyWindowAppearance(g_settingsWindow, false);
        ApplyWindowAppearance(g_infoWindow, false);
        g_managerSurface.Reset();
        g_popupSurface.Reset();
        g_settingsSurface.Reset();
        g_infoSurface.Reset();
        InvalidateRect(window, nullptr, TRUE);
        InvalidateRect(g_settingsWindow, nullptr, TRUE);
        InvalidateRect(g_infoWindow, nullptr, TRUE);
        return 0;
    case WM_CLOSE:
        if (!g_quitting) {
            ShowWindow(window, SW_HIDE);
            g_managerSurface.Reset();
            SetTimer(window, kMemoryTrimTimer, 1800, nullptr);
            return 0;
        }
        break;
    case WM_DESTROY:
        RemoveClipboardFormatListener(window);
        if (g_hotkeyRegistered) UnregisterHotKey(window, kHotkeyId);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK SettingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        ResizeSurface(window, g_settingsSurface);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const auto* rectangle = reinterpret_cast<const RECT*>(lParam);
        const UINT dpi = HIWORD(wParam);
        if (GetParent(window)) {
            RECT client{};
            GetClientRect(GetParent(window), &client);
            SetWindowPos(window, nullptr, 0, 0, client.right, client.bottom,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ResizeSurface(window, g_settingsSurface);
            return 0;
        }
        SetWindowPos(window, nullptr, rectangle->left, rectangle->top,
                     MulDiv(360, dpi, 96), MulDiv(540, dpi, 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeSurface(window, g_settingsSurface);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {
            MulDiv(360, GetDpiForWindow(window), 96), MulDiv(540, GetDpiForWindow(window), 96)};
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxTrackSize =
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize;
        return 0;
    case WM_NCHITTEST: {
        POINT pixel{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &pixel);
        const float scale = DpiScale(window);
        const auto point = D2D1::Point2F(static_cast<float>(pixel.x) / scale,
                                        static_cast<float>(pixel.y) / scale);
        if (Contains(CloseButtonRect(window), point) ||
            Contains(SettingsBackButtonRect(window), point)) return HTCLIENT;
        if (GetParent(window)) return HTCLIENT;
        if (point.y >= 0 && point.y < kTitleBarHeight) return HTCAPTION;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        DrawSettings(window);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const auto point = ToDip(window, lParam);
        const bool closeHovered = Contains(CloseButtonRect(window), point);
        const bool backHovered = Contains(SettingsBackButtonRect(window), point);
        const int control = closeHovered ? 0 : SettingsControlAtPoint(window, point);
        if (g_draggingOpacity) UpdateOpacityFromPoint(window, point);
        if (closeHovered != g_settingsCloseHovered || backHovered != g_settingsBackHovered ||
            control != g_settingsHoverControl) {
            g_settingsCloseHovered = closeHovered;
            g_settingsBackHovered = backHovered;
            g_settingsHoverControl = control;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_settingsCloseHovered = false;
        g_settingsBackHovered = false;
        g_settingsHoverControl = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const auto point = ToDip(window, lParam);
        if (Contains(CloseButtonRect(window), point)) {
            g_settingsClosePressed = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (Contains(SettingsBackButtonRect(window), point)) {
            g_settingsBackPressed = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        const int control = SettingsControlAtPoint(window, point);
        if (control) {
            g_settingsPressedControl = control;
            SetCapture(window);
        }
        if (!control && point.y < kTitleBarHeight && GetParent(window)) {
            ReleaseCapture();
            SendMessageW(GetParent(window), WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        if (control == kSettingsOpacity) {
            g_draggingOpacity = true;
            UpdateOpacityFromPoint(window, point);
        } else if (control == kSettingsHotkey) {
            g_capturingHotkey = true;
            g_settingsStatus = L"请按下 Ctrl、Alt、Shift 或 Win 加任意键";
        } else if (control == kSettingsAutoPaste) {
            g_appSettings.autoPaste = !g_appSettings.autoPaste;
            g_settingsStatus = g_appSettings.autoPaste ? L"已开启自动粘贴" : L"已关闭自动粘贴";
            PersistSettings();
        } else if (control == kSettingsHistoryMinus || control == kSettingsHistoryPlus) {
            const bool increase = control == kSettingsHistoryPlus;
            const std::size_t step = 10;
            if (increase) {
                g_appSettings.historyLimit = std::min<std::size_t>(500, g_appSettings.historyLimit + step);
            } else if (g_appSettings.historyLimit > 10) {
                g_appSettings.historyLimit = std::max<std::size_t>(10, g_appSettings.historyLimit - step);
            }
            g_settingsStatus = L"活动记录上限已更新";
            PersistSettings();
        } else if (control == kSettingsFavoriteMinus || control == kSettingsFavoritePlus) {
            const bool increase = control == kSettingsFavoritePlus;
            const std::size_t step = 10;
            if (increase) {
                g_appSettings.favoriteLimit = std::min<std::size_t>(500, g_appSettings.favoriteLimit + step);
            } else if (g_appSettings.favoriteLimit > 10) {
                g_appSettings.favoriteLimit = std::max<std::size_t>(10, g_appSettings.favoriteLimit - step);
            }
            g_settingsStatus = L"收藏上限已更新";
            PersistSettings();
        } else if (control == kSettingsMonitor) {
            g_appSettings.monitorClipboard = !g_appSettings.monitorClipboard;
            g_paused = !g_appSettings.monitorClipboard;
            g_settingsStatus = g_appSettings.monitorClipboard ? L"已开启剪贴板监听" : L"已暂停剪贴板监听";
            PersistSettings();
        } else if (control == kSettingsStartup) {
            const bool enable = !IsStartupEnabled();
            if (SetStartupEnabled(enable)) {
                g_settingsStatus = enable ? L"已开启开机自启" : L"已关闭开机自启";
            } else {
                g_settingsStatus = L"无法修改开机自启设置";
            }
            InvalidateRect(window, nullptr, FALSE);
        } else if (control == kSettingsLaunchHidden) {
            g_appSettings.launchHidden = !g_appSettings.launchHidden;
            g_settingsStatus = g_appSettings.launchHidden ? L"启动时将隐藏管理器" : L"启动时显示管理器";
            PersistSettings();
        }
        SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_settingsClosePressed) {
            const bool close = Contains(CloseButtonRect(window), ToDip(window, lParam));
            g_settingsClosePressed = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (close) PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (g_settingsBackPressed) {
            const bool back = Contains(SettingsBackButtonRect(window), ToDip(window, lParam));
            g_settingsBackPressed = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (back) HideSettings();
        } else if (g_settingsPressedControl) {
            const int pending = g_settingsPressedControl;
            if (pending == kSettingsOpacity && g_draggingOpacity) {
                SaveAppSettings();
                g_settingsStatus = L"界面背景透明度已保存";
            }
            g_settingsPressedControl = 0;
            g_draggingOpacity = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_settingsClosePressed) {
            g_settingsClosePressed = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (g_settingsBackPressed) {
            g_settingsBackPressed = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (g_settingsPressedControl) {
            g_settingsPressedControl = 0;
            g_draggingOpacity = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capturingHotkey) {
            if (wParam == VK_ESCAPE) {
                g_capturingHotkey = false;
                g_settingsStatus = L"已取消快捷键修改";
            } else if (wParam != VK_LCONTROL && wParam != VK_RCONTROL && wParam != VK_CONTROL &&
                       wParam != VK_LMENU && wParam != VK_RMENU && wParam != VK_MENU &&
                       wParam != VK_LSHIFT && wParam != VK_RSHIFT && wParam != VK_SHIFT &&
                       wParam != VK_LWIN && wParam != VK_RWIN) {
                UINT modifiers = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
                if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000) modifiers |= MOD_WIN;
                if (modifiers == 0) {
                    g_settingsStatus = L"快捷键必须包含 Ctrl、Alt、Shift 或 Win";
                } else if (RebindHotkey(modifiers, static_cast<UINT>(wParam))) {
                    g_capturingHotkey = false;
                    g_settingsStatus = L"快捷键已更新为 " + HotkeyText(modifiers, static_cast<UINT>(wParam));
                    SaveAppSettings();
                } else {
                    g_settingsStatus = L"快捷键已被其他程序占用";
                }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE:
        HideSettings();
        return 0;
    case WM_SETTINGCHANGE:
        g_dark = IsDarkTheme();
        if (g_editBrush) DeleteObject(g_editBrush);
        g_editBrush = CreateSolidBrush(CurrentTheme().editBackground);
        g_settingsSurface.Reset();
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK InfoProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        ResizeSurface(window, g_infoSurface);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const auto* rectangle = reinterpret_cast<const RECT*>(lParam);
        const UINT dpi = HIWORD(wParam);
        const int width = MulDiv(g_infoKind == InfoKind::Help ? 420 : 340, dpi, 96);
        const int height = MulDiv(g_infoKind == InfoKind::Help ? 300 : 190, dpi, 96);
        SetWindowPos(window, nullptr, rectangle->left, rectangle->top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeSurface(window, g_infoSurface);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pixel{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &pixel);
        const float scale = DpiScale(window);
        const auto point = D2D1::Point2F(static_cast<float>(pixel.x) / scale,
                                        static_cast<float>(pixel.y) / scale);
        if (Contains(CloseButtonRect(window), point)) return HTCLIENT;
        if (point.y >= 0 && point.y < kTitleBarHeight) return HTCAPTION;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        DrawInfo(window);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const bool hovered = Contains(CloseButtonRect(window), ToDip(window, lParam));
        if (hovered != g_infoCloseHovered) {
            g_infoCloseHovered = hovered;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_infoCloseHovered = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (Contains(CloseButtonRect(window), ToDip(window, lParam))) {
            g_infoClosePressed = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_infoClosePressed) {
            const bool close = Contains(CloseButtonRect(window), ToDip(window, lParam));
            g_infoClosePressed = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (close) PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_infoClosePressed) {
            g_infoClosePressed = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        HideInfo();
        return 0;
    case WM_SETTINGCHANGE:
        g_dark = IsDarkTheme();
        g_infoSurface.Reset();
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        ResizeSurface(window, g_popupSurface);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        DrawPopup(window);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const auto [section, index] = PopupHitTest(window, ToDip(window, lParam));
        if (section != g_popupHoverSection || index != g_popupHoverIndex) {
            g_popupHoverSection = section;
            g_popupHoverIndex = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_popupHoverSection = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const auto [section, index] = PopupHitTest(window, ToDip(window, lParam));
        g_popupPressedSection = section;
        g_popupPressedIndex = index;
        if (section >= 0) {
            g_popupSection = section;
            g_popupIndex = index;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        const auto [section, index] = PopupHitTest(window, ToDip(window, lParam));
        if (section >= 0 && section == g_popupPressedSection && index == g_popupPressedIndex) {
            UseEntry(section, index, true);
        }
        g_popupPressedSection = -1;
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (!g_store.Favorites().empty()) {
            const PopupLayout layout = GetPopupLayout(window);
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &screen);
            if (static_cast<float>(screen.x) / DpiScale(window) < layout.favoriteWidth) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta < 0 && g_favoriteScroll + layout.rows < g_store.Favorites().size()) {
                    ++g_favoriteScroll;
                } else if (delta > 0 && g_favoriteScroll > 0) {
                    --g_favoriteScroll;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            HideQuickPanel();
            return 0;
        }
        const PopupLayout layout = GetPopupLayout(window);
        if (wParam >= '1' && wParam <= '9') {
            const std::size_t index = static_cast<std::size_t>(wParam - '1');
            if (index < g_store.History().size()) UseEntry(1, index, true);
            return 0;
        }
        if (wParam == '0') {
            if (g_store.History().size() > 9) UseEntry(1, 9, true);
            return 0;
        }
        if (wParam >= 'A' && wParam <= 'Z') {
            const std::size_t index = 10 + static_cast<std::size_t>(wParam - 'A');
            if (index < g_store.History().size()) UseEntry(1, index, true);
            return 0;
        }
        const auto& entries = g_popupSection == 0 ? g_store.Favorites() : g_store.History();
        if (wParam == VK_TAB) {
            g_popupSection = g_popupSection == 0 ? 1 : 0;
            const auto& target = g_popupSection == 0 ? g_store.Favorites() : g_store.History();
            g_popupIndex = target.empty() ? 0 : std::min(g_popupIndex, target.size() - 1);
        } else if (wParam == VK_UP && !entries.empty()) {
            g_popupIndex = g_popupIndex == 0 ? entries.size() - 1 : g_popupIndex - 1;
        } else if (wParam == VK_DOWN && !entries.empty()) {
            g_popupIndex = (g_popupIndex + 1) % entries.size();
        } else if (wParam == VK_LEFT) {
            if (layout.splitHistory && g_popupSection == 1 &&
                g_popupIndex >= static_cast<std::size_t>(layout.rows)) {
                g_popupIndex -= static_cast<std::size_t>(layout.rows);
            } else if (!g_store.Favorites().empty()) {
                g_popupSection = 0;
                g_popupIndex = 0;
            }
        } else if (wParam == VK_RIGHT) {
            if (g_popupSection == 0 && !g_store.History().empty()) {
                g_popupSection = 1;
                g_popupIndex = 0;
            } else if (layout.splitHistory && g_popupSection == 1 &&
                       g_popupIndex + static_cast<std::size_t>(layout.rows) < g_store.History().size()) {
                g_popupIndex += static_cast<std::size_t>(layout.rows);
            }
        } else if (wParam == VK_RETURN && !entries.empty()) {
            UseEntry(g_popupSection, g_popupIndex, (GetKeyState(VK_CONTROL) & 0x8000) == 0);
            return 0;
        }
        if (g_popupSection == 0 && g_popupIndex < g_favoriteScroll) g_favoriteScroll = g_popupIndex;
        if (g_popupSection == 0 && g_popupIndex >= g_favoriteScroll + layout.rows) {
            g_favoriteScroll = g_popupIndex - static_cast<std::size_t>(layout.rows) + 1;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kAnimationTimer) AnimateQuickPanel();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && g_popupPressedSection < 0 && IsWindowVisible(window)) {
            HideQuickPanel();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool CreateTextResources() {
    if (g_d2dFactory && g_writeFactory && g_bodyFormat && g_headerFormat &&
        g_titleFormat && g_smallFormat && g_monoFormat) {
        return true;
    }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_writeFactory)))) return false;
    const wchar_t* family = L"Segoe UI Variable Text";
    if (FAILED(g_writeFactory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                13.0f, L"zh-CN", &g_bodyFormat))) return false;
    if (FAILED(g_writeFactory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                13.0f, L"zh-CN", &g_headerFormat))) return false;
    if (FAILED(g_writeFactory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                20.0f, L"zh-CN", &g_titleFormat))) return false;
    if (FAILED(g_writeFactory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 12.0f, L"zh-CN", &g_smallFormat))) return false;
    if (FAILED(g_writeFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 11.0f, L"zh-CN", &g_monoFormat))) return false;
    g_bodyFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    g_headerFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_smallFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_monoFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

void ReleaseGraphicsResources() {
    g_managerSurface.Reset();
    g_popupSurface.Reset();
    g_settingsSurface.Reset();
    g_infoSurface.Reset();
    if (g_smallFormat) g_smallFormat->Release();
    if (g_bodyFormat) g_bodyFormat->Release();
    if (g_headerFormat) g_headerFormat->Release();
    if (g_titleFormat) g_titleFormat->Release();
    if (g_monoFormat) g_monoFormat->Release();
    if (g_writeFactory) g_writeFactory->Release();
    if (g_d2dFactory) g_d2dFactory->Release();
    g_smallFormat = nullptr;
    g_bodyFormat = nullptr;
    g_headerFormat = nullptr;
    g_titleFormat = nullptr;
    g_monoFormat = nullptr;
    g_writeFactory = nullptr;
    g_d2dFactory = nullptr;
}

void ReleaseResources() {
    ReleaseGraphicsResources();
    if (g_editFont) DeleteObject(g_editFont);
    if (g_editBrush) DeleteObject(g_editBrush);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
}

bool RegisterWindows() {
    WNDCLASSEXW manager{sizeof(manager)};
    manager.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    manager.lpfnWndProc = ManagerProc;
    manager.hInstance = g_instance;
    manager.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    manager.lpszClassName = kManagerClass;
    if (!RegisterClassExW(&manager)) return false;

    WNDCLASSEXW popup{sizeof(popup)};
    popup.style = CS_HREDRAW | CS_VREDRAW;
    popup.lpfnWndProc = PopupProc;
    popup.hInstance = g_instance;
    popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    popup.lpszClassName = kPopupClass;
    if (!RegisterClassExW(&popup)) return false;

    WNDCLASSEXW settings{sizeof(settings)};
    settings.style = CS_HREDRAW | CS_VREDRAW;
    settings.lpfnWndProc = SettingsProc;
    settings.hInstance = g_instance;
    settings.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settings.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&settings)) return false;

    WNDCLASSEXW info{sizeof(info)};
    info.style = CS_HREDRAW | CS_VREDRAW;
    info.lpfnWndProc = InfoProc;
    info.hInstance = g_instance;
    info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    info.lpszClassName = kInfoClass;
    return RegisterClassExW(&info) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_singleInstance = CreateMutexW(nullptr, FALSE, L"Local\\ClipNest.SingleInstance");
    if (!g_singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kManagerClass, nullptr)) {
            PostMessageW(existing, kShowExistingMessage, 0, 0);
        }
        if (g_singleInstance) CloseHandle(g_singleInstance);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    g_dark = IsDarkTheme();
    const Theme theme = CurrentTheme();
    g_editFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI Variable Text");
    g_editBrush = CreateSolidBrush(theme.editBackground);
    if (!CreateTextResources() || !RegisterWindows()) {
        MessageBoxW(nullptr, L"无法初始化界面组件。", kProductName, MB_ICONERROR);
        ReleaseResources();
        CloseHandle(g_singleInstance);
        CoUninitialize();
        return 1;
    }

    LoadAppSettings();
    MigrateLegacyStartup();
    g_store.Load();
    const UINT systemDpi = GetDpiForSystem();
    const int managerWidth = MulDiv(340, systemDpi, 96);
    const int managerHeight = MulDiv(540, systemDpi, 96);
    const int popupWidth = MulDiv(920, systemDpi, 96);
    const int popupHeight = MulDiv(668, systemDpi, 96);
    g_manager = CreateWindowExW(WS_EX_TOOLWINDOW, kManagerClass, kProductName,
                                WS_POPUP | WS_BORDER | WS_SYSMENU | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, managerWidth, managerHeight,
                                nullptr, nullptr, instance, nullptr);
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              kPopupClass, kProductName, WS_POPUP,
                              0, 0, popupWidth, popupHeight, nullptr, nullptr, instance, nullptr);
    g_settingsWindow = CreateWindowExW(0, kSettingsClass, L"设置",
                                       WS_CHILD | WS_CLIPCHILDREN,
                                       0, 0, managerWidth, managerHeight,
                                       g_manager, nullptr, instance, nullptr);
    g_infoWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kInfoClass, L"关于",
                                   WS_POPUP | WS_BORDER | WS_SYSMENU,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   MulDiv(420, systemDpi, 96), MulDiv(300, systemDpi, 96),
                                   nullptr, nullptr, instance, nullptr);
    if (!g_manager || !g_popup || !g_settingsWindow || !g_infoWindow) {
        MessageBoxW(nullptr, L"无法创建程序窗口。", kProductName, MB_ICONERROR);
        if (g_manager) DestroyWindow(g_manager);
        if (g_popup) DestroyWindow(g_popup);
        if (g_settingsWindow) DestroyWindow(g_settingsWindow);
        if (g_infoWindow) DestroyWindow(g_infoWindow);
        ReleaseResources();
        CloseHandle(g_singleInstance);
        CoUninitialize();
        return 1;
    }
    MONITORINFO primaryWork{sizeof(primaryWork)};
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(primary, &primaryWork);
    SetWindowPos(g_manager, nullptr, primaryWork.rcWork.right - managerWidth - MulDiv(16, systemDpi, 96),
                 primaryWork.rcWork.top + MulDiv(24, systemDpi, 96), managerWidth, managerHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SyncSettingsChild(g_manager);
    ApplyWindowAppearance(g_manager, false);
    ApplyWindowAppearance(g_popup, true);
    ApplyWindowAppearance(g_settingsWindow, false);
    ApplyWindowAppearance(g_infoWindow, false);
    g_trayIcon = CreateTrayIcon();
    SendMessageW(g_manager, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_trayIcon));
    SendMessageW(g_manager, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_trayIcon));
    SendMessageW(g_settingsWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_trayIcon));
    SendMessageW(g_settingsWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_trayIcon));
    SendMessageW(g_infoWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_trayIcon));
    SendMessageW(g_infoWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_trayIcon));
    AddTrayIcon();
    g_hotkeyRegistered = RegisterHotKey(g_manager, kHotkeyId,
                                        g_appSettings.hotkeyModifiers | MOD_NOREPEAT,
                                        g_appSettings.hotkeyVirtualKey) != FALSE;
    if (!g_hotkeyRegistered) {
        NOTIFYICONDATAW notification{};
        notification.cbSize = sizeof(notification);
        notification.hWnd = g_manager;
        notification.uID = kTrayId;
        notification.uFlags = NIF_INFO;
        wcscpy_s(notification.szInfoTitle, L"快捷键不可用");
        const std::wstring hotkeyMessage = HotkeyText(g_appSettings.hotkeyModifiers,
                                                      g_appSettings.hotkeyVirtualKey) +
                                            L" 已被其他程序占用。仍可点击托盘图标打开。";
        wcscpy_s(notification.szInfo, hotkeyMessage.c_str());
        notification.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    const bool startupLaunch = commandLine && wcsstr(commandLine, L"--startup");
    if (!startupLaunch && !g_appSettings.launchHidden) ShowManager();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ReleaseResources();
    CloseHandle(g_singleInstance);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
