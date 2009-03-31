/*
 * Analog Clock Test for GDI Operation
 *
 * Copyright 2009 Hilary Cheng
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

#define WINE_TRACE dprintf

static const char appname[] = "ACLOCK";

void dprintf( const char *format, ... )
{
	char str[0x100];
	va_list va;
	int len;

	va_start( va, format );
	vsprintf( str, format, va );
	va_end( va );
	len = strlen(str);
	if (len && str[len - 1] == '\n')
		str[len-1] = 0;
	OutputDebugString( str );
}

#define PI 3.14159265

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT pPaint;
    HDC         hDC;
    HPEN        hPen, hOldPen;
    HBRUSH      hBrush;
    float       rad, x0, y0, x1, y1;

    switch (uMsg) {
    case WM_CREATE:
      SetTimer(hWnd, 1000, 1000, NULL);
      return 0;

    case WM_PAINT:
      hDC = BeginPaint(hWnd, &pPaint);

      hPen    = CreatePen(PS_DOT, 2, RGB(0, 255, 0));
      hOldPen = SelectObject(hDC, hPen);
      for (rad = 0; rad < 2 * PI; rad += (2 * PI) / 60) {
	x0 = sin(rad) * 100 + 200;
	y0 = cos(rad) * 100 + 200;

	x1 = sin(rad) * 90 + 200;
	y1 = cos(rad) * 90 + 200;

	MoveToEx(hDC, x0, y0, NULL);
	LineTo(hDC, x1, y1);
      }
      SelectObject(hDC, hOldPen);
      DeleteObject(hPen);

      hPen    = CreatePen(PS_DOT, 2, RGB(255, 0, 0));
      hOldPen = SelectObject(hDC, hPen);
      for (rad = 0; rad < 2 * PI; rad += (2 * PI) / 12) {
	x0 = sin(rad) * 100 + 200;
	y0 = cos(rad) * 100 + 200;

	x1 = sin(rad) * 80 + 200;
	y1 = cos(rad) * 80 + 200;

	MoveToEx(hDC, x0, y0, NULL);
	LineTo(hDC, x1, y1);
      }
      SelectObject(hDC, hOldPen);
      DeleteObject(hPen);

      hPen    = CreatePen(PS_DOT, 5, RGB(0, 0, 0));
      hOldPen = SelectObject(hDC, hPen);

      x1 = sin(((2.0 * PI) / 12.0) * 3.0) * 70 + 200;
      y1 = cos(((2.0 * PI) / 12.0) * 3.0) * 70 + 200;
      MoveToEx( hDC, 200, 200, NULL );
      LineTo( hDC, x1, y1 );

      x1 = sin(((2.0 * PI) / 12.0) * 5.0) * 50 + 200;
      y1 = cos(((2.0 * PI) / 12.0) * 5.0) * 50 + 200;
      MoveToEx( hDC, 200, 200, NULL );
      LineTo( hDC, x1, y1 );

      SelectObject(hDC, hOldPen);
      DeleteObject(hPen);

      EndPaint( hWnd, &pPaint );

      break;

    case WM_TIMER:
      {
	// dprintf(" WM Timer \n");
	SYSTEMTIME local;
	GetLocalTime( &local );
      }
      return 0;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// this is required when we're replacing winlogon
void init_window_station( void )
{
	SECURITY_ATTRIBUTES sa;
	HANDLE hwsta, hdesk;

	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = 0;
	sa.bInheritHandle = TRUE;

	hwsta = CreateWindowStationW( L"winsta0", 0, MAXIMUM_ALLOWED, &sa );
	SetProcessWindowStation( hwsta );
	hdesk = CreateDesktopW( L"Winlogon", 0, 0, 0, MAXIMUM_ALLOWED, &sa );
	SetThreadDesktop( hdesk );
}

int APIENTRY WinMain( HINSTANCE Instance, HINSTANCE Prev, LPSTR CmdLine, int Show )
{
	// running as winlogon.exe?
	HWINSTA hwsta = GetProcessWindowStation();
	if (!hwsta)
		init_window_station();

	WNDCLASS wc;
	MSG msg;
	HWND hWnd;

    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = Instance;
    wc.hIcon = NULL;
    wc.hCursor = LoadCursor( 0, IDI_APPLICATION );
    wc.hbrBackground = (HBRUSH) GetStockObject( BLACK_BRUSH );
    wc.lpszMenuName = NULL;
    wc.lpszClassName = appname;
    if (!RegisterClass(&wc)) ExitProcess(1);

    hWnd = CreateWindow(appname, appname, WS_VISIBLE | WS_POPUP | WS_DLGFRAME,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                    0, 0, Instance, NULL);
    if (!hWnd) ExitProcess(1);

    ShowWindow( hWnd, Show );
    UpdateWindow( hWnd );

	while (GetMessage(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return msg.wParam;
}

