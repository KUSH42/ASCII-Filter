﻿#include "AsciiFilter.h"

// ------------------------------------------------------------
// GLOBALS / STRUCTS
// ------------------------------------------------------------
AppGlobals g_App = {
   nullptr,
   nullptr,
   nullptr,
   nullptr,
   nullptr,
   nullptr,
   false,
   false,
   { 0, 0 },{ 0, 0, 0, 0 },
   AppGlobals::HitZone::None,
   std::chrono::high_resolution_clock::now(),
   0,
   0.0
};

//Constants
static const char* ASCII_GRAYSCALE = " !\"#$ % &\\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"; // ASCII palette
const int borderThickness = 4; // Adjust this to match the actual border thickness
const wchar_t* ASCII_FONT = L"Consolas";
//Constants for Aspect Ratio
const int ASCII_BLOCK_SIZE = 8; // Block size for sampling in  pixels; width of a character block
const int blockWidth = ASCII_BLOCK_SIZE;
float ASCII_CHAR_ASPECT_RATIO = 2.0f; // Example: character width-to-height ratio
const int blockHeight = static_cast<int>(ASCII_BLOCK_SIZE * ASCII_CHAR_ASPECT_RATIO); // Height adjusted for character aspect ratio

//for triple buffer
HBITMAP g_buffers[3] = { nullptr, nullptr, nullptr }; // Triple buffers
HBITMAP g_currentBuffer = nullptr;                    // Current buffer to render to
int g_bufferIndex = 0;                                // Current buffer index
HDC g_memoryDC = nullptr;                             // Memory DC for rendering

// A small struct to hold block-based ASCII info
struct AsciiCell
{
	wchar_t ch;
	COLORREF textColor;
	COLORREF bgColor;   // Background color
};

// For precomputing the ASCII-grayscale palette
static wchar_t intensityToAscii[256];
static bool initialized = false;

// Global variable to store the high-resolution timer frequency
static LARGE_INTEGER g_PerfFrequency = { 0 };

// Forward declarations
LRESULT CALLBACK WndProcOutputFrame(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcInputFrame(HWND, UINT, WPARAM, LPARAM);

void InitializeAsciiGrayscalePalette();
void InitializeHighResolutionTimer();
void RunMessageLoop();
void UpdateWindowTitleWithFPS(HWND hwnd, double fps);
bool InitDesktopDuplication();
double GetElapsedTime(LARGE_INTEGER start, LARGE_INTEGER end);
void ReleaseDesktopDuplication();
void CaptureFrame(std::vector<BYTE>& frameData, int& fullWidth, int& fullHeight);
void DrawBorderWithUpdateLayered(HWND hWnd);
void HandleMouseDown(HWND hWnd, LPARAM lParam);
void HandleMouseMove(HWND hWnd, LPARAM lParam);
void HandleMouseUp(HWND hWnd);
RECT GetBorderWindowRect();
void DrawAsciiOutput(HWND hWnd);
void ConvertRegionToAscii(const std::vector<BYTE>& frameData,
	int desktopWidth, int desktopHeight,
	const RECT& region, int blockSize,
	std::vector<AsciiCell>& asciiOut,
	int& outCols, int& outRows);

// Utility: returns which "zone" the mouse is in, for resizing
AppGlobals::HitZone DetectHitZone(RECT rc, POINT pt);

//
// Entry point
//
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	g_App.hInst = hInstance;

	// 1) Register window classes
	WNDCLASSEX wcInput = { sizeof(WNDCLASSEX) };
	wcInput.style = CS_HREDRAW | CS_VREDRAW;
	wcInput.hInstance = hInstance;
	wcInput.lpfnWndProc = WndProcInputFrame; // Input callback for green border
	wcInput.hbrBackground = nullptr; // We'll do layered painting
	wcInput.lpszClassName = L"InputWindowClass";
	if (!RegisterClassEx(&wcInput)) {
		MessageBox(nullptr, L"Failed to register input class", L"Error", MB_ICONERROR);
		return 0;
	}

	WNDCLASSEX wcOutput = { sizeof(WNDCLASSEX) };
	wcOutput.style = CS_HREDRAW | CS_VREDRAW;
	wcOutput.hInstance = hInstance;
	wcOutput.lpfnWndProc = WndProcOutputFrame; // Output callback for ASCII rendering
	wcOutput.hbrBackground = CreateSolidBrush(RGB(0, 0, 0)); // Dark background
	wcOutput.lpszClassName = L"OutputWindowClass";
	if (!RegisterClassEx(&wcOutput)) {
		MessageBox(nullptr, L"Failed to register output class", L"Error", MB_ICONERROR);
		return 0;
	}

	// 2) Create transparent input border window
	g_App.hwndInput = CreateWindowEx(
		WS_EX_TOOLWINDOW | WS_EX_LAYERED,  // Toolwindow + layered
		L"InputWindowClass",               // Class name
		L"DesktopCaptureBorder",           // Window title (not shown)
		WS_POPUP,                          // No frame
		100, 100, 400, 300,                // Initial position and size
		nullptr, nullptr, hInstance, nullptr
	);
	if (!g_App.hwndInput) {
		MessageBox(nullptr, L"Failed to create input window", L"Error", MB_ICONERROR);
		return 0;
	}

	// Adjust extended style
	LONG exStyle = GetWindowLong(g_App.hwndInput, GWL_EXSTYLE);
	exStyle &= ~WS_EX_TRANSPARENT;
	SetWindowLong(g_App.hwndInput, GWL_EXSTYLE, exStyle);
	SetWindowPos(g_App.hwndInput, HWND_TOPMOST, 100, 200, 300, 400, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	ShowWindow(g_App.hwndInput, SW_SHOW);
	UpdateWindow(g_App.hwndInput);

	// 3) Create ASCII window
	// Get inputRect of hwndInput, so that we can size hwndOutput accordingly        
	RECT inputRect;
	GetWindowRect(g_App.hwndInput, &inputRect);
	int inputWidth = inputRect.right - inputRect.left;
	int inputHeight = inputRect.bottom - inputRect.top;

	// Account for border
	int extendedWidth = inputWidth + (2 * borderThickness);
	int extendedHeight = inputHeight + (2 * borderThickness);

	// Calculate the dimensions for the output window's client area
	int outCols = (extendedWidth + blockWidth - 1) / blockWidth;
	int outRows = (extendedHeight + blockHeight - 1) / blockHeight;
	int clientWidth = outCols * blockWidth;
	int clientHeight = outRows * blockHeight;

	// Adjust the total window size to match the desired client area
	RECT desiredClientRect = { 0, 0, clientWidth, clientHeight };
	AdjustWindowRectEx(&desiredClientRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
	int totalWidth = desiredClientRect.right - desiredClientRect.left;
	int totalHeight = desiredClientRect.bottom - desiredClientRect.top;

	// Create the ASCII output window with the adjusted size
	g_App.hwndOutput = CreateWindowEx(
		0,                          // Extended window style
		L"OutputWindowClass",
		L"ASCII Output",
		WS_OVERLAPPEDWINDOW,
		inputRect.right + 10,       // Place next to the input window
		inputRect.top,              // Align top with input window
		totalWidth,
		totalHeight,
		nullptr, nullptr, hInstance, nullptr
	);
	if (!g_App.hwndOutput) {
		MessageBox(nullptr, L"Failed to create output window", L"Error", MB_ICONERROR);
		return 0;
	}

	ShowWindow(g_App.hwndOutput, SW_SHOW);
	UpdateWindow(g_App.hwndOutput);

	// 4) Init Desktop Duplication
	if (!InitDesktopDuplication()) {
		MessageBox(nullptr, L"Failed to init Desktop Duplication. Windows 8+ required, or driver support issue.",
			L"Error", MB_ICONERROR);
		return 0;
	}

	// 5) Initialize high-performance timer
	InitializeHighResolutionTimer();

	// 6) Precompute ASCII-grayscale pallette
	InitializeAsciiGrayscalePalette();

	// 7) Run message loop
	RunMessageLoop();

	// Cleanup
	ReleaseDesktopDuplication();
	
	return 0;
}

void InitializeAsciiGrayscalePalette() {
	for (int i = 0; i < 256; ++i) {
		long asciiIndex = static_cast<long>(i * (strlen(ASCII_GRAYSCALE) - 1) / 255.0f);
		intensityToAscii[i] = static_cast<wchar_t>(ASCII_GRAYSCALE[asciiIndex]);
	}
	initialized = true;
	OutputDebugString(L"ASCII-grayscale Palette has been initialized\n");
}

void InitializeHighResolutionTimer()
{
    if (!QueryPerformanceFrequency(&g_PerfFrequency)) {
        MessageBox(nullptr, L"High-resolution timer not supported.", L"Error", MB_ICONERROR);
        exit(EXIT_FAILURE);
    }
    OutputDebugString(L"High-resolution timer initialized.\n");
}

void InitializeTripleBuffers(HWND hWnd)
{
	// Get the client dimensions of the output window
	RECT clientRect;
	GetClientRect(hWnd, &clientRect);
	int width = clientRect.right - clientRect.left;
	int height = clientRect.bottom - clientRect.top;

	// Validate dimensions
	if (width <= 0 || height <= 0) {
		OutputDebugString(L"InitializeTripleBuffers: Invalid dimensions\n");
		return;
	}

	// Get the screen DC
	HDC screenDC = GetDC(hWnd);

	// Create memory DC
	if (!g_memoryDC)
		g_memoryDC = CreateCompatibleDC(screenDC);

	// Allocate triple buffers
	for (int i = 0; i < 3; i++)
	{
		if (g_buffers[i])
			DeleteObject(g_buffers[i]);

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;       // Must be > 0
		bmi.bmiHeader.biHeight = -height;    // Negative for top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;       // 32-bit color
		bmi.bmiHeader.biCompression = BI_RGB;

		if (bmi.bmiHeader.biWidth < 0) {
			wchar_t debugOutput[256]; // Ensure enough space for the debug string
			swprintf_s(debugOutput, _countof(debugOutput), L"CreateDIBSection: Width=%d, Height=%d\n", bmi.bmiHeader.biWidth, bmi.bmiHeader.biHeight);
			OutputDebugString(debugOutput);
		}

		void* bits = nullptr;
		g_buffers[i] = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

		if (!g_buffers[i] && g_buffers[i] != nullptr) {
			OutputDebugString(L"InitializeTripleBuffers: Failed to create DIB buffer\n");
		}
	}

	// Release the screen DC
	ReleaseDC(hWnd, screenDC);
}

void UpdateWindowTitleWithFPS(HWND hwnd, double fps)
{
	wchar_t title[256];
	swprintf_s(title, _countof(title), L"ASCII Output - FPS: %.2f", fps);
	SetWindowText(hwnd, title);
}

// Calculate elapsed time in seconds
double GetElapsedTime(LARGE_INTEGER start, LARGE_INTEGER end)
{
	return static_cast<double>(end.QuadPart - start.QuadPart) / g_PerfFrequency.QuadPart;
}

// Example usage of the timer for frame timing
void RunMessageLoop()
{
	MSG msg;
	LARGE_INTEGER timerStart, timerEnd, fpsStart;
	int frameCount = 0;

	QueryPerformanceCounter(&timerStart);
	QueryPerformanceCounter(&fpsStart);

	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);

		// Calculate elapsed time
		QueryPerformanceCounter(&timerEnd);
		double elapsed = GetElapsedTime(timerStart, timerEnd);

		if (elapsed >= 1.0 / 60.0) // Target 60 FPS
		{
			// Update and redraw logic
			InvalidateRect(g_App.hwndOutput, nullptr, FALSE);
			frameCount++;
			timerStart = timerEnd; // Reset start time
		}

		// Calculate FPS over 1 second intervals
		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);
		double fpsElapsed = GetElapsedTime(fpsStart, currentTime);

		if (fpsElapsed >= 1.0)
		{
			double fps = frameCount / fpsElapsed;
			UpdateWindowTitleWithFPS(g_App.hwndOutput, fps);

			// Reset for the next interval
			frameCount = 0;
			fpsStart = currentTime;
		}
	}
}

//
// WndProc for the output window
//
LRESULT CALLBACK WndProcOutputFrame(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
		InitializeTripleBuffers(hWnd);
		return 0;

	case WM_TIMER:
		if (wParam == 1) {
			{
				// Scope to avoid issues with auto declarations
				g_App.frameCounter++;

				// Calculate time elapsed
				auto currentTime = std::chrono::high_resolution_clock::now();
				auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(currentTime - g_App.lastTime);

				if (elapsed.count() >= 1.0) {
					// Update FPS
					g_App.fps.store(static_cast<double>(g_App.frameCounter) / elapsed.count());
					g_App.frameCounter = 0;
					g_App.lastTime = currentTime;

					// Update the window title
					wchar_t title[256];
					double fpsValue = g_App.fps.load();
					swprintf_s(title, sizeof(title) / sizeof(title[0]), L"ASCII Output - FPS: %.2f", fpsValue);
					SetWindowText(g_App.hwndOutput, title);
				}

				// Trigger a redraw
				InvalidateRect(hWnd, nullptr, FALSE);
			}
		}
		return 0;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		BeginPaint(hWnd, &ps);

		// Render to the back buffer and present it
		DrawAsciiOutput(hWnd);
		PresentBuffer(hWnd);

		EndPaint(hWnd, &ps);
		return 0;
	}

	case WM_DESTROY:
		CleanupTripleBuffers();
		PostQuitMessage(0);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	default:
		break;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

//
// WndProc for the input window
//
LRESULT CALLBACK WndProcInputFrame(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{

	wchar_t debugMsg[256];
	swprintf_s(debugMsg, L"WndProcInputFrame: Message=%u\n", msg);
	OutputDebugString(debugMsg);

	switch (msg)
	{
	case WM_CREATE:
		DrawBorderWithUpdateLayered(hWnd);
		return 0;

	case WM_PAINT:
		DrawBorderWithUpdateLayered(hWnd);
		return 0;

	case WM_LBUTTONDOWN:
		OutputDebugString(L"WndProcInputFrame: WM_LBUTTONDOWN triggered\n");
		HandleMouseDown(hWnd, lParam);
		return 0;

	case WM_MOUSEMOVE:
		OutputDebugString(L"WndProcInputFrame: WM_MOUSEMOVE triggered\n");
		HandleMouseMove(hWnd, lParam);
		return 0;

	case WM_LBUTTONUP:
		OutputDebugString(L"WndProcInputFrame: WM_LBUTTONUP triggered\n");
		HandleMouseUp(hWnd);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_NCHITTEST:
	{
		OutputDebugString(L"WM_NCHITTEST received\n");
		return HTCLIENT; // Inside the client area
	}

	default:
		break;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

//------------------------------------------------------------
// Desktop Duplication: init
//------------------------------------------------------------
bool InitDesktopDuplication()
{
	HRESULT hr;

	// 1) Create D3D11 device
	UINT createFlags = 0;
#ifdef _DEBUG
	// createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL outLevel;

	hr = D3D11CreateDevice(
		nullptr,            // default adapter
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		createFlags,
		featureLevels,
		_countof(featureLevels),
		D3D11_SDK_VERSION,
		&g_App.pDevice,
		&outLevel,
		&g_App.pContext
	);
	if (FAILED(hr)) {
		return false;
	}

	// 2) Get DXGI device
	IDXGIDevice* dxgiDevice = nullptr;
	hr = g_App.pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
	if (FAILED(hr)) {
		return false;
	}

	// 3) Get adapter
	IDXGIAdapter* adapter = nullptr;
	hr = dxgiDevice->GetAdapter(&adapter);
	dxgiDevice->Release();
	if (FAILED(hr)) {
		return false;
	}

	// 4) Get output (monitor). We pick the first output for simplicity
	IDXGIOutput* output = nullptr;
	hr = adapter->EnumOutputs(0, &output);
	adapter->Release();
	if (FAILED(hr) || !output) {
		return false;
	}

	// 5) Get output2
	IDXGIOutput1* output1 = nullptr;
	hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
	output->Release();
	if (FAILED(hr) || !output1) {
		return false;
	}

	// 6) Create duplication
	hr = output1->DuplicateOutput(g_App.pDevice, &g_App.pDuplication);
	output1->Release();
	if (FAILED(hr)) {
		return false;
	}

	return true;
}

//------------------------------------------------------------
// Release Desktop Duplication
//------------------------------------------------------------
void ReleaseDesktopDuplication()
{
	if (g_App.pDuplication) {
		g_App.pDuplication->Release();
		g_App.pDuplication = nullptr;
	}
	if (g_App.pContext) {
		g_App.pContext->Release();
		g_App.pContext = nullptr;
	}
	if (g_App.pDevice) {
		g_App.pDevice->Release();
		g_App.pDevice = nullptr;
	}
}

//------------------------------------------------------------
// Capture one frame via Desktop Duplication
// Returns raw BGRA (or RGBA) in frameData
//------------------------------------------------------------
void CaptureFrame(std::vector<BYTE>& frameData, int& fullWidth, int& fullHeight)
{
	if (!g_App.pDuplication)
		return;

	// Acquire frame
	IDXGIResource* desktopResource = nullptr;
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
	HRESULT hr = g_App.pDuplication->AcquireNextFrame(0, &frameInfo, &desktopResource);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		// no new frame => just skip
		return;
	}
	if (FAILED(hr)) {
		// Attempt to release, re-init?
		g_App.pDuplication->ReleaseFrame();
		return;
	}

	// Query for ID3D11Texture2D
	ID3D11Texture2D* tex = nullptr;
	if (desktopResource) {
		desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
		desktopResource->Release();
	}

	if (!tex) {
		g_App.pDuplication->ReleaseFrame();
		return;
	}

	// Get desc
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	fullWidth = desc.Width;
	fullHeight = desc.Height;

	// Create a staging texture to copy into CPU-accessible memory
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;

	ID3D11Texture2D* stagingTex = nullptr;
	hr = g_App.pDevice->CreateTexture2D(&desc, nullptr, &stagingTex);
	if (SUCCEEDED(hr) && stagingTex) {
		// Copy
		g_App.pContext->CopyResource(stagingTex, tex);

		// Map
		D3D11_MAPPED_SUBRESOURCE map;
		hr = g_App.pContext->Map(stagingTex, 0, D3D11_MAP_READ, 0, &map);
		if (SUCCEEDED(hr)) {
			// copy out
			int rowPitch = map.RowPitch;
			frameData.resize(desc.Height * rowPitch);
			memcpy(frameData.data(), map.pData, desc.Height * rowPitch);
			g_App.pContext->Unmap(stagingTex, 0);
		}
		stagingTex->Release();
	}

	tex->Release();
	g_App.pDuplication->ReleaseFrame();
}

//------------------------------------------------------------
// Draw the border in an offscreen DIB, call UpdateLayeredWindow
// to make a truly see-through center with a 2px green line
//------------------------------------------------------------
void DrawBorderWithUpdateLayered(HWND hWnd) {
	RECT rect;
	GetWindowRect(hWnd, &rect);
	int width = rect.right - rect.left;
	int height = rect.bottom - rect.top;

	if (width <= 0 || height <= 0) {
		OutputDebugString(L"Invalid dimensions in DrawBorderWithUpdateLayered\n");
		return;
	}

	HDC screenDC = GetDC(nullptr);
	HDC memDC = CreateCompatibleDC(screenDC);

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height; // Top-down DIB
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* pBits = nullptr;
	HBITMAP hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
	if (!hBitmap || !pBits) {
		OutputDebugString(L"Failed to create DIB section\n");
		DeleteDC(memDC);
		ReleaseDC(nullptr, screenDC);
		return;
	}

	HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);

	// Initialize the entire bitmap as transparent
	memset(pBits, 0, width * height * 4); // BGRA, all zero is fully transparent

	// Create a green border
	BYTE* pixels = (BYTE*)pBits;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int idx = (y * width + x) * 4;
			if (x < 4 || y < 4 || x >= width - 4 || y >= height - 4) {
				// Border: fully opaque green
				pixels[idx + 0] = 0;   // Blue
				pixels[idx + 1] = 180; // Green
				pixels[idx + 2] = 0;   // Red
				pixels[idx + 3] = 1;   // Alpha (1 == nearly transparent, 255 == opaque)
				continue;
			}
			// Extend hit-test area to double the thickness but keep visual thickness unchanged
			if (x < 8 || y < 8 || x >= width - 8 || y >= height - 8) {
				// Invisible draggable area
				pixels[idx + 0] = 0;   // Blue
				pixels[idx + 1] = 0;   // Green
				pixels[idx + 2] = 0;   // Red
				pixels[idx + 3] = 1;   // Alpha (nearly transparent)
				continue;
			}
			// Center: fully transparent, mouse input will not register
			pixels[idx + 0] = 0;   // Blue
			pixels[idx + 1] = 0;   // Green
			pixels[idx + 2] = 0;   // Red
			pixels[idx + 3] = 0;   // Alpha (0 == fully transparent, 255 == opaque)
		}
	}

	// Use UpdateLayeredWindow to render the layered window
	POINT ptSrc = { 0, 0 };
	POINT ptDest = { rect.left, rect.top };
	SIZE size = { width, height };
	BLENDFUNCTION blend = {};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = 255; // Fully opaque
	blend.AlphaFormat = AC_SRC_ALPHA; // Use per-pixel alpha

	if (!UpdateLayeredWindow(hWnd, screenDC, &ptDest, &size, memDC, &ptSrc, 0, &blend, ULW_ALPHA)) {
		DWORD error = GetLastError();
		wchar_t debugOutput[256];
		swprintf_s(debugOutput, _countof(debugOutput), L"UpdateLayeredWindow failed: %lu\n", error);
		OutputDebugString(debugOutput);
	}

	SelectObject(memDC, oldBitmap);
	DeleteObject(hBitmap);
	DeleteDC(memDC);
	ReleaseDC(nullptr, screenDC);
}

//------------------------------------------------------------
// On left button down in the border, decide if we're dragging or resizing
//------------------------------------------------------------
void HandleMouseDown(HWND hWnd, LPARAM lParam)
{
	OutputDebugString(L"HandleMouseDown: Called\n");

	SetCapture(hWnd);

	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	ClientToScreen(hWnd, &pt);

	RECT rc;
	GetWindowRect(hWnd, &rc);

	OutputDebugString(L"HandleMouseDown: Detecting hit zone\n");
	g_App.hitZone = DetectHitZone(rc, pt);

	// Top or Left border triggers dragging (move)
	if (g_App.hitZone == AppGlobals::HitZone::Top ||
		g_App.hitZone == AppGlobals::HitZone::Left) {
		g_App.dragging = true;
		g_App.resizing = false;
		OutputDebugString(L"HandleMouseDown: Moving initiated\n");
	}
	else {
		g_App.dragging = false;
		g_App.resizing = true;
		OutputDebugString(L"HandleMouseDown: Resizing initiated\n");
	}

	g_App.dragStartPt = pt;
	g_App.dragStartRect = rc;
}

//------------------------------------------------------------
void HandleMouseMove(HWND hWnd, LPARAM lParam)
{
	if (!g_App.dragging && !g_App.resizing)
		return;

	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	ClientToScreen(hWnd, &pt);

	int dx = pt.x - g_App.dragStartPt.x;
	int dy = pt.y - g_App.dragStartPt.y;

	RECT rc = g_App.dragStartRect;

	if (g_App.dragging) {
		// Move the entire rectangle
		OffsetRect(&rc, dx, dy);

		// Update the input window position
		SetWindowPos(hWnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);

		// Debugging logs for verification
		wchar_t debugMsg[256];
		swprintf_s(debugMsg, L"Dragging: Input Rect: [%d, %d, %d, %d]\n", rc.left, rc.top, rc.right, rc.bottom);
		OutputDebugString(debugMsg);

		RECT inputRect;
		GetWindowRect(g_App.hwndInput, &inputRect);
		swprintf_s(debugMsg, L"Dragging: Updated Input Rect: [%d, %d, %d, %d]\n", inputRect.left, inputRect.top, inputRect.right, inputRect.bottom);
		OutputDebugString(debugMsg);

		RECT outputRect;
		GetWindowRect(g_App.hwndOutput, &outputRect);
		swprintf_s(debugMsg, L"Dragging: Output Rect Unchanged: [%d, %d, %d, %d]\n", outputRect.left, outputRect.top, outputRect.right, outputRect.bottom);
		OutputDebugString(debugMsg);
	}
	else if (g_App.resizing) {
		// Resize the green border based on the hit zone
		switch (g_App.hitZone) {
		case AppGlobals::HitZone::Top:         rc.top += dy; break;
		case AppGlobals::HitZone::Left:        rc.left += dx; break;
		case AppGlobals::HitZone::Right:       rc.right += dx; break;
		case AppGlobals::HitZone::Bottom:      rc.bottom += dy; break;
		case AppGlobals::HitZone::TopLeft:	   rc.left += dx; rc.top += dy; break;
		case AppGlobals::HitZone::TopRight:    rc.right += dx; rc.top += dy; break;
		case AppGlobals::HitZone::BottomLeft:  rc.left += dx; rc.bottom += dy; break;
		case AppGlobals::HitZone::BottomRight: rc.right += dx; rc.bottom += dy; break;
		default:
			break;
		}

		// Enforce minimum size
		const int minWidth = 60;
		const int minHeight = 60;
		if ((rc.right - rc.left) < minWidth) rc.right = rc.left + minWidth;
		if ((rc.bottom - rc.top) < minHeight) rc.bottom = rc.top + minHeight;

		// Apply the new size to the green border
		SetWindowPos(hWnd, HWND_TOPMOST, rc.left, rc.top,
			rc.right - rc.left, rc.bottom - rc.top,
			SWP_NOACTIVATE);

		// Redraw the green border
		DrawBorderWithUpdateLayered(hWnd);

		// Recalculate the size for the output window
		RECT inputRect;
		GetWindowRect(g_App.hwndInput, &inputRect);
		int inputWidth = inputRect.right - inputRect.left;
		int inputHeight = inputRect.bottom - inputRect.top;

		// Account for border
		int extendedWidth = inputWidth + (2 * borderThickness);
		int extendedHeight = inputHeight + (2 * borderThickness);

		// Calculate new dimensions for the output window's client area
		int outCols = (extendedWidth + blockWidth - 1) / blockWidth;
		int outRows = (extendedHeight + blockHeight - 1) / blockHeight;
		int clientWidth = outCols * blockWidth;
		int clientHeight = outRows * blockHeight;

		// Adjust total window size to ensure client area matches desired dimensions
		RECT desiredClientRect = { 0, 0, clientWidth, clientHeight };
		AdjustWindowRectEx(&desiredClientRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
		int totalWidth = desiredClientRect.right - desiredClientRect.left;
		int totalHeight = desiredClientRect.bottom - desiredClientRect.top;

		// Keep the output window at its current position but adjust its size
		RECT outputRect;
		GetWindowRect(g_App.hwndOutput, &outputRect);
		SetWindowPos(g_App.hwndOutput, nullptr,
			outputRect.left,                     // Keep current position
			outputRect.top,                      // Keep current position
			totalWidth,                          // Adjusted width
			totalHeight,                         // Adjusted height
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

		// Reinitialize the buffer for the output frame to match the new size
		InitializeTripleBuffers(g_App.hwndOutput);

		// Debugging logs
		wchar_t debugMsg[256];
		swprintf_s(debugMsg, L"Resizing: Input Rect: [%d, %d, %d, %d]\n", rc.left, rc.top, rc.right, rc.bottom);
		OutputDebugString(debugMsg);
		swprintf_s(debugMsg, L"Resizing: Output Rect: [%d, %d, %d, %d]\n", outputRect.left, outputRect.top, outputRect.top, outputRect.bottom);
		OutputDebugString(debugMsg);
	}
}

//------------------------------------------------------------
void HandleMouseUp(HWND hWnd)
{
	g_App.dragging = false;
	g_App.resizing = false;
	g_App.hitZone = AppGlobals::HitZone::None;
	ReleaseCapture();
}

//------------------------------------------------------------
// Determine which edge/corner the point is in
//------------------------------------------------------------
AppGlobals::HitZone DetectHitZone(RECT rc, POINT pt)
{
	if (!PtInRect(&rc, pt)) {
		OutputDebugString(L"Hit zone: None\n");
		return AppGlobals::HitZone::None;
	}

	int x = pt.x - rc.left;
	int y = pt.y - rc.top;
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;

	bool left = (x < borderThickness);
	bool right = (x > (w - borderThickness));
	bool top = (y < borderThickness);
	bool bottom = (y > (h - borderThickness));

	if (top && left) {
		OutputDebugString(L"Hit zone: TopLeft\n");
		return AppGlobals::HitZone::TopLeft;
	}
	if (top && right) {
		OutputDebugString(L"Hit zone: TopRight\n");
		return AppGlobals::HitZone::TopRight;
	}
	if (bottom && left) {
		OutputDebugString(L"Hit zone: BottomLeft\n");
		return AppGlobals::HitZone::BottomLeft;
	}
	if (bottom && right) {
		OutputDebugString(L"Hit zone: BottomRight\n");
		return AppGlobals::HitZone::BottomRight;
	}
	if (left) {
		OutputDebugString(L"Hit zone: Left\n");
		return AppGlobals::HitZone::Left;
	}
	if (right) {
		OutputDebugString(L"Hit zone: Right\n");
		return AppGlobals::HitZone::Right;
	}
	if (top) {
		OutputDebugString(L"Hit zone: Top\n");
		return AppGlobals::HitZone::Top;
	}
	if (bottom) {
		OutputDebugString(L"Hit zone: Bottom\n");
		return AppGlobals::HitZone::Bottom;
	}

	OutputDebugString(L"Hit zone: None\n");
	return AppGlobals::HitZone::None;
}

//------------------------------------------------------------
RECT GetBorderWindowRect()
{
	RECT rc;
	GetWindowRect(g_App.hwndInput, &rc);
	return rc;
}

//------------------------------------------------------------
// Paint the ASCII output
// 1) Capture a frame (Desktop Dup).
// 2) Extract region under border window.
// 3) Convert to ASCII with block sampling (e.g. 8x8).
// 4) Draw each character with SetTextColor.
//------------------------------------------------------------
void DrawAsciiOutput(HWND hWnd)
{
	// Capture frame data
	std::vector<BYTE> frameData;
	int desktopWidth = 0, desktopHeight = 0;
	CaptureFrame(frameData, desktopWidth, desktopHeight);

	// If no frame data, skip
	if (frameData.empty() || desktopWidth == 0 || desktopHeight == 0) {
		OutputDebugString(L"DrawAsciiOutput: No frame data captured\n");
		return;
	}

	// Get the input region, including borders
	RECT capRect = GetBorderWindowRect();
	// Clamp the region to desktop size
	capRect.left = std::max(0L, capRect.left);
	capRect.top = std::max(0L, capRect.top);
	capRect.right = std::min((LONG)desktopWidth, capRect.right);
	capRect.bottom = std::min((LONG)desktopHeight, capRect.bottom);

	// Select the current buffer into the memory DC
	HBITMAP oldBitmap = (HBITMAP)SelectObject(g_memoryDC, g_buffers[g_bufferIndex]);

	// Clear only the parts of the buffer that require it
	RECT clientRect;
	GetClientRect(hWnd, &clientRect);
	static RECT previousRect = { 0, 0, 0, 0 };

	if (!EqualRect(&clientRect, &previousRect)) {
		// Only clear the areas that changed since the last frame
		HRGN diffRegion = CreateRectRgnIndirect(&clientRect);
		CombineRgn(diffRegion, diffRegion, CreateRectRgnIndirect(&previousRect), RGN_DIFF);

		HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
		FillRgn(g_memoryDC, diffRegion, bgBrush);
		DeleteObject(bgBrush);
		DeleteObject(diffRegion);

		previousRect = clientRect; // Update the previous rect
	}

	SetBkMode(g_memoryDC, OPAQUE); // Allow background color rendering

	// Convert the captured region to ASCII
	std::vector<AsciiCell> asciiOut;
	int outCols = 0, outRows = 0;
	ConvertRegionToAscii(frameData, desktopWidth, desktopHeight, capRect, ASCII_BLOCK_SIZE, asciiOut, outCols, outRows);

	// Prepare font
	HFONT hFont = CreateFont(
		16, 8, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, OEM_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		FIXED_PITCH | FF_MODERN, ASCII_FONT
	);
	HFONT oldFont = (HFONT)SelectObject(g_memoryDC, hFont);
	
	TEXTMETRIC tm;
	HDC hdc = GetDC(nullptr); // Or your window's DC
	GetTextMetrics(hdc, &tm);
	ASCII_CHAR_ASPECT_RATIO = (float)tm.tmHeight / (float)tm.tmAveCharWidth;

	// Draw each row
	for (int row = 0; row < outRows; ++row) {
		std::wstring rowBuffer;
		COLORREF lastTextColor = RGB(255, 255, 255);
		COLORREF lastBgColor = RGB(0, 0, 0);

		for (int col = 0; col < outCols; ++col) {
			const AsciiCell& cell = asciiOut[row * outCols + col];

			if (rowBuffer.empty()) {
				// Start the buffer with the initial colors
				lastTextColor = cell.textColor;
				lastBgColor = cell.bgColor;
			}
			else if (cell.textColor != lastTextColor || cell.bgColor != lastBgColor) {
				// Flush the current buffer when colors change
				SetTextColor(g_memoryDC, lastTextColor);
				SetBkColor(g_memoryDC, lastBgColor);
				TextOut(g_memoryDC, col * blockWidth - static_cast<int>(rowBuffer.length()) * blockWidth, row * blockHeight, rowBuffer.c_str(), rowBuffer.length());
				rowBuffer.clear();
				lastTextColor = cell.textColor;
				lastBgColor = cell.bgColor;
			}
			// Add character to buffer
			rowBuffer += cell.ch;
		}

		// Flush the remaining characters in the buffer
		if (!rowBuffer.empty()) {
			SetTextColor(g_memoryDC, lastTextColor);
			SetBkColor(g_memoryDC, lastBgColor);
			TextOut(g_memoryDC, 0, row * blockHeight, rowBuffer.c_str(), rowBuffer.length());
		}
	}

	// Restore and bitmap
	SelectObject(g_memoryDC, oldFont);
	DeleteObject(hFont); 
	SelectObject(g_memoryDC, oldBitmap);
}

void PresentBuffer(HWND hWnd)
{
	HDC screenDC = GetDC(hWnd);
	RECT clientRect;
	GetClientRect(hWnd, &clientRect);
	int width = clientRect.right - clientRect.left;
	int height = clientRect.bottom - clientRect.top;

	if (g_memoryDC && g_buffers[g_bufferIndex]) {
		// Copy the current buffer to the screen
		HBITMAP oldBitmap = (HBITMAP)SelectObject(g_memoryDC, g_buffers[g_bufferIndex]);
		BitBlt(screenDC, 0, 0, width, height, g_memoryDC, 0, 0, SRCCOPY);
		SelectObject(g_memoryDC, oldBitmap);
	}
	else {
		OutputDebugString(L"PresentBuffer: Invalid memory DC or buffers\n");
	}

	// Cycle to the next buffer
	g_bufferIndex = (g_bufferIndex + 1) % 3;

	// Release screen DC
	ReleaseDC(hWnd, screenDC);
}

void CleanupTripleBuffers()
{
	for (int i = 0; i < 3; i++)
	{
		if (g_buffers[i]) {
			DeleteObject(g_buffers[i]);
			g_buffers[i] = nullptr;
		}
	}

	if (g_memoryDC) {
		DeleteDC(g_memoryDC);
		g_memoryDC = nullptr;
	}
}

//------------------------------------------------------------
// Convert the (capRect) portion of the frameData to ASCII
// with block sampling of size blockSize x blockSize
//------------------------------------------------------------
void ConvertRegionToAscii(const std::vector<BYTE>& frameData,
	int desktopWidth, int desktopHeight,
	const RECT& region, int blockSize,
	std::vector<AsciiCell>& asciiOut,
	int& outCols, int& outRows)
{
	// Precompute ASCII mapping
	if (!initialized) {
		
	}

	int rowPitch = desktopWidth * 4;

	// Compute region size
	int regionW = region.right - region.left;
	int regionH = region.bottom - region.top;

	// Calculate number of blocks
	outCols = (regionW + blockSize - 1) / blockSize;
	outRows = (regionH + blockHeight - 1) / blockHeight;

	asciiOut.resize(outCols * outRows);

	// Loop through each block
	for (int row = 0; row < outRows; ++row) {
		for (int col = 0; col < outCols; ++col) {
			// Pixel block region
			int startX = region.left + col * blockWidth;
			int startY = region.top + row * blockHeight;
			int endX = std::min((long) startX + blockWidth, region.right);
			int endY = std::min((long) startY + blockHeight, region.bottom);

			// Accumulate color
			unsigned int sumR = 0, sumG = 0, sumB = 0, count = 0;
			for (int y = startY; y < endY; ++y) {
				const BYTE* pixel = frameData.data() + y * rowPitch + startX * 4;
				for (int x = startX; x < endX; ++x) {
					sumB += pixel[0];
					sumG += pixel[1];
					sumR += pixel[2];
					pixel += 4;
					count++;
				}
			}

			// Calculate average color
			if (count > 0) {
				BYTE avgR = sumR / count;
				BYTE avgG = sumG / count;
				BYTE avgB = sumB / count;

				// Calculate intensity and luminance
				BYTE intensity = static_cast<BYTE>(0.299f * avgR + 0.587f * avgG + 0.114f * avgB);
				BYTE luminance = static_cast<BYTE>((0.299 * avgR + 0.587 * avgG + 0.114 * avgB));

				// Text color as a dynamic grayscale based on luminance
				BYTE textGray = (luminance > 128) ? luminance - 80 : luminance + 80; // Ensure contrast
				COLORREF textColor = RGB(textGray, textGray, textGray);

				// Populate AsciiCell with text and background colors
				asciiOut[row * outCols + col] = {
					intensityToAscii[intensity], // Character
					textColor,                   // Dynamic grayscale text color
					RGB(avgR, avgG, avgB)        // Background color
				};
			}
		}
	}
}