// AsciiFilter.h : Include file for standard system include files,
// or project specific include files.

#pragma once
#define NOMINMAX
#include <algorithm>
#include <iostream>
#include <windows.h>
#include <windowsx.h>  // for GET_X_LPARAM, GET_Y_LPARAM macros
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <dxgi1_2.h>
#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

LRESULT CALLBACK WndProcMain(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcInput(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK WndProcOutput(HWND, UINT, WPARAM, LPARAM);

struct AppGlobals
{
    HINSTANCE           hInst = nullptr;
    HWND                hwndInput = nullptr;  // The "capture" border window
    HWND                hwndOutput = nullptr;  // The ASCII output window
    IDXGIOutputDuplication* pDuplication = nullptr;  // Desktop Duplication interface
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    // For dragging/resizing the border
    bool  dragging = false;
    bool  resizing = false;
    POINT dragStartPt = { 0, 0 };
    RECT  dragStartRect = { 0, 0, 0, 0 };
    // Keep track which edge/corner is grabbed
    enum class HitZone { None, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
    HitZone hitZone = HitZone::None;
};

extern AppGlobals g_App;

// A helper function to register window classes
bool RegisterWindowClasses(HINSTANCE hInst);

// Utility for creating child windows
HWND CreateChildWindow(HINSTANCE hInst = nullptr,
    HWND hWndParent = nullptr,
    LPCTSTR className = nullptr,
    int x = 0, int y = 0, int width = 100, int height = 100,
    WNDPROC wndProc = nullptr,
    LPCTSTR wndName = nullptr);

// A function simulating capture of an image (from camera or file)
// For demonstration, we create a checkerboard or load a test .bmp
bool GetTestImageData(std::vector<COLORREF>& pixels, int width, int height);

// ASCII-art conversion
void ConvertToASCII(const std::vector<COLORREF>& pixels, int width, int height,
    std::vector<wchar_t>& output, bool useColor);

// TODO: Reference additional headers your program requires here. //
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int);

void PresentBuffer(HWND hWnd);
void CleanupTripleBuffers();