#include "AsciiFilter.h>"

//----------------------------------------------------------------
// Global definitions/variables
//----------------------------------------------------------------

// For simplicity, define a single global instance to store settings
struct AppState
{
    HWND   hWndMain = nullptr;  // Main window
    HWND   hWndInput = nullptr;  // Child: input rectangle
    HWND   hWndOutput = nullptr;  // Child: output rectangle
    bool   useColor = false;    // Toggle: color or grayscale for ASCII
    SIZE   inputSize = { 400, 300 };
    SIZE   outputSize = { 400, 300 };
    POINT  inputPos = { 50, 50 };
    POINT  outputPos = { 500, 50 };
} g_App;

// ASCII shades (from dark to light). Adjust or extend as desired.
static const char* ASCII_GRAYSCALE = " .:-=+*#%@";
static const char* ASCII_COLORS[] = {
    // This is a very simplified color lookup. 
    // In a real scenario, you might map each pixel to 
    // a best-fit ASCII+color combination.
    " .:-=+*#%@",
    " .:-=+*#%@",
    " .:-=+*#%@",
};

//----------------------------------------------------------------
// Forward declarations
//----------------------------------------------------------------
LRESULT CALLBACK WndProcMain(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcInput(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcOutput(HWND, UINT, WPARAM, LPARAM);

// A helper function to register window classes
bool RegisterWindowClasses(HINSTANCE hInst);

// Utility for creating child windows 
HWND CreateChildWindow(HINSTANCE hInst, HWND hWndParent, LPCTSTR className,
    int x, int y, int width, int height,
    WNDPROC wndProc, LPCTSTR wndName = nullptr);

// A function simulating capture of an image (from camera or file)
// For demonstration, we create a checkerboard or load a test .bmp
bool GetTestImageData(std::vector<COLORREF>& pixels, int width, int height);

// ASCII-art conversion
void ConvertToASCII(const std::vector<COLORREF>& pixels, int width, int height,
    std::vector<wchar_t>& output, bool useColor);

//----------------------------------------------------------------
// WinMain
//----------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    // 1) Register window classes
    if (!RegisterWindowClasses(hInst))
    {
        MessageBox(nullptr, L"Failed to register window classes!", L"Error", MB_ICONERROR);
        return -1;
    }

    // 2) Create the main window
    g_App.hWndMain = CreateWindowEx(
        0,
        L"MainWindowClass",
        L"ASCII Art Example",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1200, 800,
        nullptr,
        nullptr,
        hInst,
        nullptr
    );
    if (!g_App.hWndMain)
    {
        MessageBox(nullptr, L"Failed to create main window!", L"Error", MB_ICONERROR);
        return -1;
    }

    // 3) Create child windows: Input & Output
    g_App.hWndInput = CreateChildWindow(hInst, g_App.hWndMain, L"InputWindowClass",
        g_App.inputPos.x, g_App.inputPos.y, g_App.inputSize.cx, g_App.inputSize.cy,
        WndProcInput, L"InputArea");
    g_App.hWndOutput = CreateChildWindow(hInst, g_App.hWndMain, L"OutputWindowClass",
        g_App.outputPos.x, g_App.outputPos.y, g_App.outputSize.cx, g_App.outputSize.cy,
        WndProcOutput, L"OutputArea");

    // Create the Timer for 60 FPS on the output window
    // 1000 / 60 = ~16 ms interval
    SetTimer(g_App.hWndOutput, 1, 1000 / 60, nullptr);

    // 4) Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

//----------------------------------------------------------------
// Register Window Classes
//----------------------------------------------------------------
bool RegisterWindowClasses(HINSTANCE hInst)
{
    // Main Window Class
    {
        WNDCLASS wc = {};
        wc.lpfnWndProc = WndProcMain;
        wc.hInstance = hInst;
        wc.lpszClassName = L"MainWindowClass";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClass(&wc))
            return false;
    }

    // Child: Input Window Class
    {
        WNDCLASS wc = {};
        wc.lpfnWndProc = WndProcInput;
        wc.hInstance = hInst;
        wc.lpszClassName = L"InputWindowClass";
        // We use a NULL brush here and custom paint with transparency
        wc.hbrBackground = nullptr;
        if (!RegisterClass(&wc))
            return false;
    }

    // Child: Output Window Class
    {
        WNDCLASS wc = {};
        wc.lpfnWndProc = WndProcOutput;
        wc.hInstance = hInst;
        wc.lpszClassName = L"OutputWindowClass";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClass(&wc))
            return false;
    }

    return true;
}

//----------------------------------------------------------------
// CreateChildWindow
//----------------------------------------------------------------
HWND CreateChildWindow(HINSTANCE hInst, HWND hWndParent, LPCTSTR className,
    int x, int y, int width, int height,
    WNDPROC wndProc, LPCTSTR wndName)
{
    // For child window creation, typically we use WS_CHILD. 
    // But for easy transparency, we can use WS_POPUP + WS_EX_LAYERED.
    // However, to remain "on top" inside the main window, we handle movement ourselves.

    HWND hWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,  // extended style for layered window
        className,
        wndName ? wndName : L"",
        WS_POPUP | WS_VISIBLE,
        x, y, width, height,
        hWndParent,
        nullptr,
        hInst,
        nullptr
    );

    // Set up 90% translucency for the input window
    if (className == std::wstring(L"InputWindowClass"))
    {
        // Range 0 (fully transparent) to 255 (fully opaque). We want 90% => ~230
        SetLayeredWindowAttributes(hWnd, 0, (BYTE)(0.9f * 255), LWA_ALPHA);
    }
    else
    {
        // Output window is fully opaque
        SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
    }

    return hWnd;
}

//----------------------------------------------------------------
// Main Window Procedure
//----------------------------------------------------------------
LRESULT CALLBACK WndProcMain(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

//----------------------------------------------------------------
// Helper for handling dragging/resizing a "popup" child
//----------------------------------------------------------------
void HandleMouseDragResize(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static bool  s_dragging = false;
    static bool  s_resizing = false;
    static POINT s_startPt = {};
    static RECT  s_startRect = {};

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        // Decide if we are near the border => resizing
        // or inside => moving.
        // For simplicity, treat corners/border as resizing.
        RECT rc;
        GetWindowRect(hWnd, &rc);
        int borderSize = 10;  // detect 10px border region
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        if (mx < borderSize || my < borderSize ||
            mx >(rc.right - rc.left - borderSize) ||
            my >(rc.bottom - rc.top - borderSize))
        {
            s_resizing = true;
        }
        else
        {
            s_dragging = true;
        }

        POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &ptScreen);
        s_startPt = ptScreen;
        s_startRect = rc;
        SetCapture(hWnd);
    }
    break;

    case WM_MOUSEMOVE:
    {
        if (!s_dragging && !s_resizing)
            break;

        POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &ptScreen);
        int dx = ptScreen.x - s_startPt.x;
        int dy = ptScreen.y - s_startPt.y;

        RECT newRect = s_startRect;
        if (s_dragging)
        {
            OffsetRect(&newRect, dx, dy);
        }
        else if (s_resizing)
        {
            newRect.right += dx;
            newRect.bottom += dy;
        }

        // Minimum size
        if ((newRect.right - newRect.left) < 50)
            newRect.right = newRect.left + 50;
        if ((newRect.bottom - newRect.top) < 50)
            newRect.bottom = newRect.top + 50;

        SetWindowPos(hWnd, nullptr, newRect.left, newRect.top,
            newRect.right - newRect.left, newRect.bottom - newRect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;

    case WM_LBUTTONUP:
    {
        s_dragging = false;
        s_resizing = false;
        ReleaseCapture();
    }
    break;
    }
}

//----------------------------------------------------------------
// Input Window Procedure
//----------------------------------------------------------------
LRESULT CALLBACK WndProcInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        // We could do custom drawing here if needed.
        // The input rectangle is 90% translucent, so we usually
        // see what's behind it (the main window background).
        EndPaint(hWnd, &ps);
    }
    break;

    // Let user drag/resize
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
        HandleMouseDragResize(hWnd, msg, wParam, lParam);
        break;

    default:
        break;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

//----------------------------------------------------------------
// Output Window Procedure
//----------------------------------------------------------------
LRESULT CALLBACK WndProcOutput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
    {
        if (wParam == 1)
        {
            // Timer 1: Prompt a repaint at ~60 FPS
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }
    break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        // 1) Grab "input" data (for demonstration, we generate it or load once).
        //    In a real scenario, you'd capture a frame from camera or read an image.
        std::vector<COLORREF> pixels(width * height);
        if (!GetTestImageData(pixels, width, height))
        {
            // fallback: fill black
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            EndPaint(hWnd, &ps);
            return 0;
        }

        // 2) Convert to ASCII
        std::vector<wchar_t> asciiOut(width * height);
        ConvertToASCII(pixels, width, height, asciiOut, g_App.useColor);

        // 3) Draw ASCII text
        //    We'll simply output line by line. This is naive, but demonstrates the concept.
        SetBkMode(hdc, TRANSPARENT);
        HFONT hFont = (HFONT)GetStockObject(OEM_FIXED_FONT);
        HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

        int yOffset = 0;
        int xCharsPerLine = width;  // 1 char per pixel horizontally
        for (int y = 0; y < height; ++y)
        {
            // Construct the line from y*width to (y+1)*width
            std::wstring line(asciiOut.begin() + y * width, asciiOut.begin() + (y + 1) * width);

            TextOut(hdc, 0, yOffset, line.c_str(), (int)line.size());
            yOffset += 14; // approximate line height; 
            // if you want perfect spacing, measure the font size
        }

        SelectObject(hdc, oldFont);

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
        HandleMouseDragResize(hWnd, msg, wParam, lParam);
        break;

    default:
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

//----------------------------------------------------------------
// Simulate capturing an image
//----------------------------------------------------------------
bool GetTestImageData(std::vector<COLORREF>& pixels, int width, int height)
{
    if (pixels.size() < (size_t)(width * height))
        return false;

    // Example: draw a simple checkerboard
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            bool blackWhite = ((x / 20) % 2) ^ ((y / 20) % 2); // small squares
            // Make some gradient color
            BYTE r = blackWhite ? 200 : 50;
            BYTE g = (BYTE)(x % 255);
            BYTE b = (BYTE)(y % 255);
            pixels[y * width + x] = RGB(r, g, b);
        }
    }
    return true;
}

//----------------------------------------------------------------
// Convert to ASCII
//----------------------------------------------------------------
void ConvertToASCII(const std::vector<COLORREF>& pixels, int width, int height,
    std::vector<wchar_t>& output, bool useColor)
{
    // Basic approach: for each pixel, map intensity to an ASCII character.
    // For color mode, you might map to a color-coded ASCII in a real console
    // or use GDI with colored text. Here, we just pick the same ASCII set
    // but could store a separate color attribute if you wish.

    if (output.size() < (size_t)(width * height))
        return;

    int numLevels = (int)strlen(ASCII_GRAYSCALE) - 1;
    for (int i = 0; i < width * height; ++i)
    {
        COLORREF c = pixels[i];
        BYTE r = GetRValue(c);
        BYTE g = GetGValue(c);
        BYTE b = GetBValue(c);

        // Get intensity
        float intensity = 0.299f * r + 0.587f * g + 0.114f * b;
        int idx = (int)(intensity * numLevels / 255.0f);
        idx = max(0, min(numLevels, idx));

        // For color, you might do a best color match. 
        // For demonstration, just pick a "colored" ASCII if useColor is true
        wchar_t ch = ASCII_GRAYSCALE[idx];
        output[i] = ch;
    }
}

