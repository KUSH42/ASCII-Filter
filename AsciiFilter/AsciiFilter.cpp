#include "AsciiFilter.h"

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
   AppGlobals::HitZone::None
};


//Constants
static const char* ASCII_GRAYSCALE = " !\"#$ % &\\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"; // ASCII palette
const int borderThickness = 4; // Adjust this to match the actual border thickness
const wchar_t* ASCII_FONT = L"Consolas";
//Constants for Aspect Ratio
const int ASCII_BLOCK_SIZE = 8; // Block size for sampling in pixels; width of a character block
const int blockWidth = ASCII_BLOCK_SIZE;
float ASCII_CHAR_ASPECT_RATIO = 2.0f; // Example: character width-to-height ratio
const int blockHeight = static_cast<int>(ASCII_BLOCK_SIZE * ASCII_CHAR_ASPECT_RATIO); // Height adjusted for character aspect ratio

//for triple buffer
HBITMAP g_buffers[3] = { nullptr, nullptr, nullptr }; // Triple buffers
HBITMAP g_currentBuffer = nullptr;                   // Current buffer to render to
int g_bufferIndex = 0;                               // Current buffer index
HDC g_memoryDC = nullptr;                            // Memory DC for rendering

// A small struct to hold block-based ASCII info
struct AsciiCell
{
	wchar_t ch;
	COLORREF color;
};


// Forward declarations
LRESULT CALLBACK WndProcOutputFrame(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcInputFrame(HWND, UINT, WPARAM, LPARAM);

bool InitDesktopDuplication();
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

	// Preserve the aspect ratio for the output window, accounting for its frame
	RECT outputClientRect = { 0, 0, extendedWidth, extendedHeight };
	AdjustWindowRectEx(&outputClientRect, WS_OVERLAPPEDWINDOW, FALSE, 0); // Use WS_OVERLAPPEDWINDOW styles

	// Calculate number of blocks (ceil division to ensure coverage)
	int outCols = (extendedWidth + blockWidth - 1) / blockWidth;
	int outRows = (extendedHeight + blockHeight - 1) / blockHeight;

	// Calculate the size of the output window based on ASCII grid
	int outputWidth = outCols * blockWidth;
	int outputHeight = outRows * blockHeight;

	// Adjust for the frame of the output window
	RECT outputRect = { 0, 0, outputWidth, outputHeight };
	AdjustWindowRectEx(&outputRect, WS_OVERLAPPEDWINDOW, FALSE, 0);

	// Create the ASCII output window
	g_App.hwndOutput = CreateWindowEx(
		0, // No extended window styles
		L"OutputWindowClass", // Class name for output window
		L"ASCII Output",
		WS_OVERLAPPEDWINDOW, // Window style
		inputRect.right + 10,
		inputRect.top, // Position the output window to the right of the input window
		outputRect.right - outputRect.left,  // Adjusted width
		outputRect.bottom - outputRect.top,  // Adjusted height		
		nullptr, // Parent window
		nullptr, // Menu handle
		hInstance, // Instance handle
		nullptr // Additional application data
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

	// 5) Timer ~30 FPS
	SetTimer(g_App.hwndOutput, 1, 33, nullptr);

	// 6) Message loop
	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// Cleanup
	ReleaseDesktopDuplication();
	return (int)msg.wParam;
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
			// Redraw the frame periodically
			InvalidateRect(hWnd, nullptr, FALSE);
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
		// Resize based on the hit zone
		switch (g_App.hitZone) {
		case AppGlobals::HitZone::Left:       rc.left += dx; break;
		case AppGlobals::HitZone::Right:      rc.right += dx; break;
		case AppGlobals::HitZone::Top:        rc.top += dy; break;
		case AppGlobals::HitZone::Bottom:     rc.bottom += dy; break;
		case AppGlobals::HitZone::TopLeft:    rc.left += dx; rc.top += dy; break;
		case AppGlobals::HitZone::TopRight:   rc.right += dx; rc.top += dy; break;
		case AppGlobals::HitZone::BottomLeft: rc.left += dx; rc.bottom += dy; break;
		case AppGlobals::HitZone::BottomRight:rc.right += dx; rc.bottom += dy; break;
		default: break;
		}

		// Ensure minimum size
		const int minWidth = 60;
		const int minHeight = 60;
		if ((rc.right - rc.left) < minWidth) rc.right = rc.left + minWidth;
		if ((rc.bottom - rc.top) < minHeight) rc.bottom = rc.top + minHeight;

		// Update the input window size
		SetWindowPos(hWnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);

		// Re-draw the layered border
		DrawBorderWithUpdateLayered(hWnd);

		RECT inputRect;
		GetWindowRect(g_App.hwndInput, &inputRect);
		int inputWidth = inputRect.right - inputRect.left;
		int inputHeight = inputRect.bottom - inputRect.top;

		// Account for border
		int extendedWidth = inputWidth + (2 * borderThickness);
		int extendedHeight = inputHeight + (2 * borderThickness);

		// Get Rectangle we want to resize to preserve coordinates
		RECT oldRect;
		GetWindowRect(g_App.hwndOutput, &oldRect);

		// Preserve the aspect ratio for the output window, accounting for its frame
		RECT outputClientRect = { 0, 0, extendedWidth, extendedHeight };
		AdjustWindowRectEx(&outputClientRect, WS_OVERLAPPEDWINDOW, FALSE, 0); // Use WS_OVERLAPPEDWINDOW styles

		// Calculate number of blocks (ceil division to ensure coverage)
		int outCols = (extendedWidth + 2 * blockWidth - 1) / blockWidth;
		int outRows = (extendedHeight + 2 * blockHeight - 1) / blockHeight + 1;

		// Calculate the size of the output window based on ASCII grid
		int outputWidth = outCols * blockWidth;
		int outputHeight = outRows * blockHeight;

		// Adjust for the frame of the output window
		RECT outputRect = { 0, 0, outputWidth, outputHeight };
		AdjustWindowRectEx(&outputRect, WS_OVERLAPPEDWINDOW, FALSE, 0);

		SetWindowPos(g_App.hwndOutput, nullptr, oldRect.left, oldRect.top,
			outputWidth, outputHeight,
			SWP_NOZORDER | SWP_NOACTIVATE);

		// Reinitialize the buffer for the output frame to match the new size
		InitializeTripleBuffers(g_App.hwndOutput);

		// Debugging logs
		wchar_t debugMsg[256];
		swprintf_s(debugMsg, L"Resizing: Input Rect: [%d, %d, %d, %d]\n", rc.left, rc.top, rc.right, rc.bottom);
		OutputDebugString(debugMsg);
		swprintf_s(debugMsg, L"Resizing: Output Rect: [%d, %d, %d, %d]\n", outputRect.right - outputRect.left, outputRect.bottom - outputRect.top);
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

	// Clear the buffer with black
	RECT clientRect;
	GetClientRect(hWnd, &clientRect);
	HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
	FillRect(g_memoryDC, &clientRect, bgBrush);
	DeleteObject(bgBrush);

	// Set text background mode to transparent
	SetBkMode(g_memoryDC, TRANSPARENT);

	// Convert the captured region to ASCII
	std::vector<AsciiCell> asciiOut;
	int outCols = 0, outRows = 0;
	ConvertRegionToAscii(frameData, desktopWidth, desktopHeight, capRect, ASCII_BLOCK_SIZE, asciiOut, outCols, outRows);

	// Draw ASCII output
	HFONT hFont = CreateFont(
		16, 8, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, OEM_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		FIXED_PITCH | FF_MODERN, ASCII_FONT
	);
	HDC hdc = GetDC(nullptr); // Or your window's DC
	HFONT oldFont = (HFONT)SelectObject(g_memoryDC, hFont);
	TEXTMETRIC tm;
	GetTextMetrics(hdc, &tm);
	ASCII_CHAR_ASPECT_RATIO = (float)tm.tmHeight / (float)tm.tmAveCharWidth;

	for (int row = 0; row < outRows; ++row) {
		for (int col = 0; col < outCols; ++col) {
			const AsciiCell& cell = asciiOut[row * outCols + col];
			SetTextColor(g_memoryDC, cell.color);

			// Create a null-terminated string with the character
			wchar_t text[2] = { cell.ch, L'\0' };
			// Pass the string and length
			TextOut(g_memoryDC, col * blockWidth, row * blockHeight, text, 1);
		}
	}

	// Restore old font
	SelectObject(g_memoryDC, oldFont);
	DeleteObject(hFont);

	// Restore old bitmap
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
	// frameData is typically in BGRA or RGBA layout depending on driver,
	// but usually it's BGRA with rowPitch >= desktopWidth * 4.

	// We'll guess rowPitch == desktopWidth * 4. 
	// (In a real app, store rowPitch from the map.)
	int rowPitch = desktopWidth * 4;
		
	// Compute the region width and height
	int regionW = region.right - region.left;
	int regionH = region.bottom - region.top;

	// Calculate the number of blocks (columns and rows)
	outCols = (regionW + blockSize - 1) / blockSize; // Ceiling division
	// TODO: Remove 4x OutputDebugString
	if (((regionW + blockSize - 1) / blockSize) % blockSize != 0) {
		++outCols;
		wchar_t debugOutput[256]; // Ensure enough space for the debug string
		swprintf_s(debugOutput, _countof(debugOutput), L"(regionW + blockSize - 1) / blockSize) %% blockSize = %d\noutCols has been increased\n", ((regionW + blockSize - 1) / blockSize) % blockSize);
		OutputDebugString(debugOutput);
	}
	else {
		wchar_t debugOutput[256]; // Ensure enough space for the debug string
		swprintf_s(debugOutput, _countof(debugOutput), L"(regionW + blockSize - 1) / blockSize) %% blockSize = %d\noutCols has NOT been increased\n", ((regionW + blockSize - 1) / blockSize) % blockSize);
		OutputDebugString(debugOutput);
	}
	outRows = (regionH + blockHeight - 1) / blockHeight;
	if (((regionH + blockHeight - 1) / blockHeight) % blockHeight != 0) {
		++outRows;
		wchar_t debugOutput[256]; // Ensure enough space for the debug string
		swprintf_s(debugOutput, _countof(debugOutput), L"regionH + blockHeight - 1) / blockHeight) %% blockHeight = %d\noutRows has been increased\n", ((regionH + blockHeight - 1) / blockHeight) % blockHeight);
		OutputDebugString(debugOutput);
	}
	else {
		wchar_t debugOutput[256]; // Ensure enough space for the debug string
		swprintf_s(debugOutput, _countof(debugOutput), L"regionH + blockHeight - 1) / blockHeight) %% blockHeight = %d\noutRows has NOT been increased\n", ((regionH + blockHeight - 1) / blockHeight) % blockHeight);
		OutputDebugString(debugOutput);
	}

	asciiOut.resize(outCols * outRows);

	// Loop through each block
	for (int row = 0; row < outRows; ++row) {
		for (int col = 0; col < outCols; ++col) {
			// Calculate pixel block region
			int startX = region.left + col * blockWidth;
			int startY = region.top + row * blockHeight;
			int endX = std::min((LONG)startX + blockWidth, region.right);
			int endY = std::min((LONG)startY + blockHeight, region.bottom);

			// Compute average color within the block
			unsigned int sumR = 0, sumG = 0, sumB = 0, count = 0;
			for (int y = startY; y < endY; ++y) {
				const BYTE* pixel = frameData.data() + y * rowPitch + startX * 4;
				for (int x = startX; x < endX; ++x) {
					sumB += pixel[0];
					sumG += pixel[1];
					sumR += pixel[2];
					pixel += 4;
					++count;
				}
			}
			if (count > 0) {
				BYTE avgR = sumR / count;
				BYTE avgG = sumG / count;
				BYTE avgB = sumB / count;
				float intensity = 0.299f * avgR + 0.587f * avgG + 0.114f * avgB;

				// Calculate the index in the ASCII grayscale palette
				long asciiIndex = static_cast<long>(intensity * (strlen(ASCII_GRAYSCALE) - 1) / 255.f);

				// Fix: Convert the char to wchar_t
				wchar_t asciiChar = static_cast<wchar_t>(ASCII_GRAYSCALE[asciiIndex]);

				// Store the cell
				asciiOut[row * outCols + col] = { asciiChar, RGB(avgR, avgG, avgB) };
			}
		}
	}
}