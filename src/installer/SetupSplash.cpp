#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#undef min
#undef max
#include <tlhelp32.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")

using namespace Gdiplus;

namespace {

constexpr int kBaseWidth  = 300;
constexpr int kBaseHeight = 350;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kFrameIntervalMs = 16; // ~60 FPS

struct AppState {
    HWND hwnd = nullptr;
    HANDLE hParentProcess = nullptr;
    std::wstring progressFilePath;
    std::wstring versionText = L"v1.5.1";
    std::wstring statusStep  = L"Extracting application files\u2026";

    float scale = 1.0f;
    int width  = kBaseWidth;
    int height = kBaseHeight;

    float spinnerAngle = -90.0f;
    float pulsePhase   = 0.0f;
    float targetProgress  = 0.08f;
    float displayProgress = 0.02f;
    bool finishing = false;
    int finishTicks = 0;
};

AppState g_state;

DWORD FindParentProcessId() {
    const DWORD selfPid = GetCurrentProcessId();
    DWORD parentPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == selfPid) {
                parentPid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return parentPid;
}

void AddRoundedRect(GraphicsPath &path, REAL x, REAL y, REAL w, REAL h, REAL radius) {
    REAL d = radius * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.Reset();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void PollProgressFile() {
    if (g_state.progressFilePath.empty()) return;

    HANDLE hFile = CreateFileW(
        g_state.progressFilePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    char buf[512]{};
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead == 0) return;
    buf[bytesRead] = '\0';

    std::string content(buf, bytesRead);
    while (!content.empty() && (content.back() == '\r' || content.back() == '\n' || content.back() == ' ')) {
        content.pop_back();
    }

    const size_t sep = content.find('|');
    if (sep == std::string::npos) return;

    const std::string left = content.substr(0, sep);
    const std::string right = content.substr(sep + 1);

    if (left == "DONE") {
        g_state.targetProgress = 1.0f;
        g_state.finishing = true;
        return;
    }

    const int pct = std::atoi(left.c_str());
    if (pct > 0) {
        const float norm = std::fmin(1.0f, std::fmax(0.0f, static_cast<float>(pct) / 100.0f));
        if (norm > g_state.targetProgress) {
            g_state.targetProgress = norm;
        }
    }

    if (!right.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, right.c_str(), static_cast<int>(right.size()), nullptr, 0);
        if (wlen > 0) {
            std::wstring wstr(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, right.c_str(), static_cast<int>(right.size()), &wstr[0], wlen);
            g_state.statusStep = wstr;
        }
    }
}

void RenderSplashFrame() {
    if (!g_state.hwnd) return;

    const int w = g_state.width;
    const int h = g_state.height;
    const float s = g_state.scale;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDc, hBmp));

    {
        Graphics g(memDc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));

        // 1. Rounded Card Surface (#0A0A0D) with 1px Border (#363641)
        GraphicsPath cardPath;
        AddRoundedRect(cardPath, 0.5f * s, 0.5f * s, w - 1.0f * s, h - 1.0f * s, 16.0f * s);

        SolidBrush bgBrush(Color(255, 10, 10, 13));
        g.FillPath(&bgBrush, &cardPath);

        // Clip inside card for ambient top glow
        g.SetClip(&cardPath);
        {
            GraphicsPath glowPath;
            const REAL glowR = 130.0f * s;
            const REAL glowCx = 150.0f * s;
            const REAL glowCy = -10.0f * s;
            glowPath.AddEllipse(glowCx - glowR, glowCy - glowR, glowR * 2.0f, glowR * 2.0f);

            PathGradientBrush glowBrush(&glowPath);
            glowBrush.SetCenterColor(Color(38, 229, 9, 20));
            Color surroundColors[1] = { Color(0, 229, 9, 20) };
            INT count = 1;
            glowBrush.SetSurroundColors(surroundColors, &count);
            g.FillPath(&glowBrush, &glowPath);
        }
        g.ResetClip();

        Pen borderPen(Color(255, 54, 54, 65), 1.0f * s);
        g.DrawPath(&borderPen, &cardPath);

        // 2. Animated Brand Emblem + Spinner Ring (center at 150, 86)
        const REAL cx = 150.0f * s;
        const REAL cy = 86.0f * s;

        // Outer pulsing ring
        const float pulseScale = 1.0f + 0.05f * std::sin(g_state.pulsePhase);
        const REAL pulseR = 44.0f * s * pulseScale;
        Pen pulsePen(Color(40, 229, 9, 20), 1.0f * s);
        g.DrawEllipse(&pulsePen, cx - pulseR, cy - pulseR, pulseR * 2.0f, pulseR * 2.0f);

        // Spinner track ring
        const REAL spinR = 39.0f * s;
        Pen trackRingPen(Color(26, 255, 255, 255), 3.0f * s);
        g.DrawEllipse(&trackRingPen, cx - spinR, cy - spinR, spinR * 2.0f, spinR * 2.0f);

        // Rotating crimson arc
        Pen arcPen(Color(255, 229, 9, 20), 3.0f * s);
        arcPen.SetStartCap(LineCapRound);
        arcPen.SetEndCap(LineCapRound);
        g.DrawArc(&arcPen, cx - spinR, cy - spinR, spinR * 2.0f, spinR * 2.0f, g_state.spinnerAngle, 195.0f);

        // Inner dark circle (#15151A) + border (#363641)
        const REAL badgeR = 31.0f * s;
        SolidBrush badgeBrush(Color(255, 21, 21, 26));
        g.FillEllipse(&badgeBrush, cx - badgeR, cy - badgeR, badgeR * 2.0f, badgeR * 2.0f);
        Pen badgeBorderPen(Color(255, 54, 54, 65), 1.0f * s);
        g.DrawEllipse(&badgeBorderPen, cx - badgeR, cy - badgeR, badgeR * 2.0f, badgeR * 2.0f);

        // Center "F" monogram
        FontFamily segoeFamily(L"Segoe UI");
        StringFormat centerFormat;
        centerFormat.SetAlignment(StringAlignmentCenter);
        centerFormat.SetLineAlignment(StringAlignmentCenter);

        Font monogramFont(&segoeFamily, 30.0f * s, FontStyleBold, UnitPixel);
        SolidBrush accentBrush(Color(255, 229, 9, 20));
        RectF badgeRect(cx - badgeR, cy - badgeR + 1.0f * s, badgeR * 2.0f, badgeR * 2.0f);
        g.DrawString(L"F", -1, &monogramFont, badgeRect, &centerFormat, &accentBrush);

        // 3. Wordmark "F L U X"
        Font wordmarkFont(&segoeFamily, 21.0f * s, FontStyleBold, UnitPixel);
        SolidBrush textBrush(Color(255, 245, 245, 247));
        RectF wordmarkRect(0.0f, 144.0f * s, static_cast<REAL>(w), 30.0f * s);
        g.DrawString(L"F L U X", -1, &wordmarkFont, wordmarkRect, &centerFormat, &textBrush);

        // 4. Primary Discord-style status headline
        Font headlineFont(&segoeFamily, 11.0f * s, FontStyleBold, UnitPixel);
        RectF headlineRect(20.0f * s, 194.0f * s, (kBaseWidth - 40.0f) * s, 20.0f * s);
        const wchar_t *headline = (g_state.displayProgress >= 0.98f)
                                  ? L"STARTING\u2026"
                                  : L"INSTALLING FLUX\u2026";
        g.DrawString(headline, -1, &headlineFont, headlineRect, &centerFormat, &textBrush);

        // 5. Secondary percentage line
        const int pct = std::min(100, std::max(1, static_cast<int>(std::round(g_state.displayProgress * 100.0f))));
        wchar_t detailBuf[128]{};
        std::swprintf(detailBuf, 128, L"Setting up FLUX %ls (%d%%)", g_state.versionText.c_str(), pct);

        Font detailFont(&segoeFamily, 12.0f * s, FontStyleRegular, UnitPixel);
        SolidBrush dimBrush(Color(255, 169, 169, 182));
        RectF detailRect(20.0f * s, 218.0f * s, (kBaseWidth - 40.0f) * s, 20.0f * s);
        g.DrawString(detailBuf, -1, &detailFont, detailRect, &centerFormat, &dimBrush);

        // 6. Step subtext
        Font stepFont(&segoeFamily, 10.5f * s, FontStyleRegular, UnitPixel);
        SolidBrush muteBrush(Color(255, 108, 108, 122));
        RectF stepRect(20.0f * s, 240.0f * s, (kBaseWidth - 40.0f) * s, 18.0f * s);
        g.DrawString(g_state.statusStep.c_str(), -1, &stepFont, stepRect, &centerFormat, &muteBrush);

        // 7. Smooth Progress Bar
        const REAL barX = 28.0f * s;
        const REAL barY = 288.0f * s;
        const REAL barW = (kBaseWidth - 56.0f) * s;
        const REAL barH = 5.0f * s;
        const REAL barR = 2.5f * s;

        GraphicsPath trackPath;
        AddRoundedRect(trackPath, barX, barY, barW, barH, barR);
        SolidBrush trackBrush(Color(255, 37, 37, 46));
        g.FillPath(&trackBrush, &trackPath);

        const REAL fillW = std::fmax(6.0f * s, barW * std::fmin(1.0f, std::fmax(0.0f, g_state.displayProgress)));
        GraphicsPath fillPath;
        AddRoundedRect(fillPath, barX, barY, fillW, barH, barR);
        g.FillPath(&accentBrush, &fillPath);

        // 8. Version Footer
        FontFamily monoFamily(L"Consolas");
        Font footerFont(monoFamily.IsAvailable() ? &monoFamily : &segoeFamily, 10.0f * s, FontStyleRegular, UnitPixel);
        RectF footerRect(20.0f * s, 310.0f * s, (kBaseWidth - 40.0f) * s, 18.0f * s);
        g.DrawString(g_state.versionText.c_str(), -1, &footerFont, footerRect, &centerFormat, &muteBrush);
    }

    POINT ptSrc{ 0, 0 };
    SIZE wndSize{ w, h };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_state.hwnd, screenDc, nullptr, &wndSize, memDc, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;

    case WM_TIMER:
        if (wParam == kTimerId) {
            // Check if parent NSIS installer process has exited
            if (g_state.hParentProcess) {
                if (WaitForSingleObject(g_state.hParentProcess, 0) == WAIT_OBJECT_0) {
                    g_state.finishing = true;
                    g_state.targetProgress = 1.0f;
                }
            }

            // Poll progress file every ~100ms
            static int tickCounter = 0;
            if (++tickCounter % 6 == 0) {
                PollProgressFile();
            }

            // While extracting large files, creep targetProgress smoothly up to 0.92
            if (!g_state.finishing && g_state.targetProgress < 0.92f) {
                g_state.targetProgress += 0.0018f;
            }

            // Smooth exponential easing toward targetProgress
            const float diff = g_state.targetProgress - g_state.displayProgress;
            g_state.displayProgress += diff * (g_state.finishing ? 0.22f : 0.08f);

            // Advance animations (60 FPS)
            g_state.spinnerAngle = std::fmod(g_state.spinnerAngle + 6.0f, 360.0f);
            g_state.pulsePhase += 0.048f;

            RenderSplashFrame();

            if (g_state.finishing && g_state.displayProgress >= 0.99f) {
                if (++g_state.finishTicks > 18) { // ~300ms at 100%
                    PostQuitMessage(0);
                }
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2 && argv[1][0] != L'\0') {
            g_state.progressFilePath = argv[1];
        }
        if (argc >= 3 && argv[2][0] != L'\0') {
            std::wstring ver = argv[2];
            if (!ver.empty() && ver[0] != L'v' && ver[0] != L'V') {
                ver = L"v" + ver;
            }
            g_state.versionText = ver;
        }
        LocalFree(argv);
    }

    const DWORD parentPid = FindParentProcessId();
    if (parentPid != 0) {
        g_state.hParentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    }

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    // Compute DPI scale for primary monitor
    HDC screenDc = GetDC(nullptr);
    const int dpiX = GetDeviceCaps(screenDc, LOGPIXELSX);
    ReleaseDC(nullptr, screenDc);
    g_state.scale = (dpiX > 0) ? (static_cast<float>(dpiX) / 96.0f) : 1.0f;
    g_state.width  = static_cast<int>(std::round(kBaseWidth * g_state.scale));
    g_state.height = static_cast<int>(std::round(kBaseHeight * g_state.scale));

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int posX = (screenW - g_state.width) / 2;
    const int posY = (screenH - g_state.height) / 2;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SplashWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, L"IDI_ICON1");
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FluxDiscordSetupSplashClass";
    RegisterClassExW(&wc);

    g_state.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"FLUX Setup",
        WS_POPUP | WS_VISIBLE,
        posX, posY, g_state.width, g_state.height,
        nullptr, nullptr, hInstance, nullptr);

    if (g_state.hwnd) {
        PollProgressFile();
        RenderSplashFrame();
        ShowWindow(g_state.hwnd, SW_SHOW);
        SetTimer(g_state.hwnd, kTimerId, kFrameIntervalMs, nullptr);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    GdiplusShutdown(gdiplusToken);
    if (g_state.hParentProcess) {
        CloseHandle(g_state.hParentProcess);
    }
    return 0;
}
