/*
 * Traktouch companion DLL
 *
 * This DLL implements the actual business end of Traktor Touch:
 *   - Turn off tap-and-hold right-click emulation for the Traktor window
 *   - Hook SetCursorPos to intercept Traktor's attempts to move the mouse cursor back
 *   - Hook GetMessage to intercept mouse messages generated from touch events and apply
 *     an offset based on previous SetCursorPos calls
 *
 * Copyright (c) 2019 by Joachim Fenkes <github@dojoe.net>
 */

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files:

#include <windows.h>
#include <tpcshrd.h>
#include <CommCtrl.h>

#include "mhook-lib/mhook.h"
#include "guicon.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifdef _DEBUG
char *GetErrorMessage()
{
	static char msgbuf[1024];
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msgbuf, sizeof(msgbuf), NULL);
	return msgbuf;
}

#define dprintf printf
#else
#define dprintf(...)
#endif

struct {
	bool scrollTrackListOnly;
	int scrollTrackListOffsetX;
	int scrollTrackListOffsetY;
	int scrollScale;
	int scrollAccelDeadZone;
	float scrollAccelExponent;
} config;

static void readConfig(HMODULE hModule)
{
	TCHAR iniFileName[1024];
	int nchars = GetModuleFileName(hModule, iniFileName, _countof(iniFileName));
	lstrcpy(iniFileName + nchars - 4, L".ini");

	config.scrollTrackListOnly =       GetPrivateProfileInt(L"Scroll", L"TrackListOnly",    1,   iniFileName);
	config.scrollTrackListOffsetX =    GetPrivateProfileInt(L"Scroll", L"TrackListOffsetX", 20,  iniFileName);
	config.scrollTrackListOffsetY =    GetPrivateProfileInt(L"Scroll", L"TrackListOffsetY", 80,  iniFileName);
	config.scrollScale =               GetPrivateProfileInt(L"Scroll", L"Scale",            10,  iniFileName);
	config.scrollAccelDeadZone =       GetPrivateProfileInt(L"Scroll", L"AccelDeadZone",    3,   iniFileName);
	config.scrollAccelExponent = float(GetPrivateProfileInt(L"Scroll", L"AccelExponent",    200, iniFileName)) * 0.01f;
}

/* Our GetMessage hook */
HHOOK hMessageHook;

/* Is the user currently touching the screen, and the accumulated correction offset for mouse events */
bool touching = false;
POINTS touchCorrection;

/*
 * Replacement for SetCursorPos.
 *
 * If the user is not touching the screen, defer to the original SetCursorPos.
 *
 * If the user _is_ touching the screen, don't move the cursor but instead store the offset
 * that will need to be applied to the touch position to make it _appear_ to Traktor as if
 * it had moved the cursor.
 */
typedef BOOL(WINAPI *SetCursorPos_t)(_In_ int X, _In_ int Y);
SetCursorPos_t OrigSetCursorPos = (SetCursorPos_t)GetProcAddress(GetModuleHandle(L"user32"), "SetCursorPos");
BOOL WINAPI MySetCursorPos(_In_ int x, _In_ int y)
{
	if (touching) {
		POINT p;
		GetCursorPos(&p);
		touchCorrection.x = (SHORT)(p.x - x);
		touchCorrection.y = (SHORT)(p.y - y);
		return true;
	}
	else {
		return OrigSetCursorPos(x, y);
	}
}

static inline int sign(int x) {
	return (x > 0) - (x < 0);
}

static LRESULT HandlePanGesture(const GESTUREINFO &gi, HWND hWnd)
{
	static POINTS panOrigin = { 0, 0 };
	static int panPrevY = 0;
	static bool ignorePanEvent = false;

	/*
	* Windows' gesture matcher has a "dead zone" where no pan gesture is detected yet.
	* Once you overcome that dead zone, the first pan event will have a very large distance, causing
	* the scrolling to jump. While this is desirable on touch interfaces where the panned canvas
	* is supposed to follow the finger, it is confusing when scrolling through tracks in Traktor,
	* especially with the acceleration we're performing below :)
	* So we're going to ignore that first pan event.
	*/
	if (ignorePanEvent) {
		panPrevY = gi.ptsLocation.y;
		ignorePanEvent = false;
	}

	/*
	* When a pan gesture starts, store the touch location -
	* it becomes the location of the emulated mouse wheel events
	*/
	if (gi.dwFlags & GF_BEGIN) {
		if (config.scrollTrackListOnly) {
			RECT wr;
			GetWindowRect(hWnd, &wr);
			panOrigin.x = short(wr.right - config.scrollTrackListOffsetX);
			panOrigin.y = short(wr.bottom - config.scrollTrackListOffsetY);
		}
		else {
			panOrigin = gi.ptsLocation;
		}
		panPrevY = gi.ptsLocation.y;
		ignorePanEvent = true;
	}

	int delta = gi.ptsLocation.y - panPrevY;
	panPrevY = gi.ptsLocation.y;

	/*
	* The mouse wheel distance is calculated from the relative pan movement times a scaling factor,
	* and accelerated slightly so the user can scroll really fast if they want.
	*/
	int scroll;
	if (abs(delta) < config.scrollAccelDeadZone)
		scroll = delta * config.scrollScale;
	else
		scroll = sign(delta) * (
					int(powf(float(abs(delta) - config.scrollAccelDeadZone), config.scrollAccelExponent) * float(config.scrollScale))
					+ config.scrollAccelDeadZone * config.scrollScale
				);

	/*
	* Make sure we don't accidentally wrap if the acceleration grows too large.
	*/
	if (scroll > MAXSHORT)
		scroll = MAXSHORT;
	else if (scroll < short(MINSHORT))
		scroll = short(MINSHORT);

	/* Hand a fake WM_MOUSEWHEEL to the window, discard the original message. */
	return DefSubclassProc(hWnd, WM_MOUSEWHEEL, scroll << 16, *(DWORD *)&panOrigin);
}

/*
 * We subclass the Traktor main window mainly for two purposes:
 *
 * One, to be able to tell Windows that we don't want it to interpret the touch-and-hold gesture
 * to emulate right clicks for the Traktor windows. For details, see
 * https://blogs.msdn.microsoft.com/oldnewthing/20170227-00/?p=95585
 *
 * Two, to turn pan gestures into mouse wheel events.
 *
 * We also undo the subclassing if the window is about to close, and in debug mode
 * we allow the loader to ask the DLL to remove itself from Traktor.
 */
LRESULT CALLBACK HookedWindowProc(
	HWND hWnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam,
	UINT_PTR uIdSubclass,
	DWORD_PTR dwRefData
)
{
#ifdef _DEBUG
	/* We're uncleanly hijacking a message that may be in use -- debug mode only ;) */
	if (uMsg == WM_APP && lParam == 0xDEADBEEF && wParam == 0xCAFE) {
		dprintf("self-remove\n");
		RemoveWindowSubclass(hWnd, HookedWindowProc, 1);
		Mhook_Unhook((PVOID *)&OrigSetCursorPos);
		UnhookWindowsHookEx(hMessageHook);
		CloseConsole();
		return 0xBABE;
	}
#endif

	switch (uMsg) {
	/* This is where we tell Windows that we don't want right-click emulation */
	case WM_TABLET_QUERYSYSTEMGESTURESTATUS:
		return TABLET_DISABLE_PRESSANDHOLD;

	/* Turn pan gestures into mouse wheel events */
	case WM_GESTURE:
	{
		HGESTUREINFO hgi = (HGESTUREINFO)lParam;
		GESTUREINFO gi;

		gi.cbSize = sizeof(gi);
		GetGestureInfo(hgi, &gi);
		if (gi.dwID == GID_PAN) {
			CloseGestureInfoHandle(hgi);
			return HandlePanGesture(gi, hWnd);
		}

		break;
	}
	case WM_NCDESTROY:
		RemoveWindowSubclass(hWnd, HookedWindowProc, uIdSubclass);
		break;
	}
	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

/*
* Return true if the current mouse event is derived from touch, according to
* https://docs.microsoft.com/en-us/windows/desktop/wintouch/troubleshooting-applications
*/
bool isTouchMouseEvent()
{
	const LONG_PTR cSignatureMask = 0xFFFFFF00;
	const LONG_PTR cFromTouch = 0xFF515700;
	return (GetMessageExtraInfo() & cSignatureMask) == cFromTouch;
}

/*
 * The GetMessage hook intercepts all touch-generated mouse messages,
 * manages entering and exiting touch mode, and applies the offset correction.
 *
 * It also takes care of subclassing the Traktor window and configuring it for pan gestures.
 */
LRESULT CALLBACK MessageHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	static bool firstTime = true;
	MSG &msg = *(MSG *)lParam;

	if (firstTime) {
		/*
		 * The main UI thread handles several windows and we need to subclass the right one,
		 * so check the window title until we see the proper Traktor window pass by, then
		 * subclass that one.
		 */
		static wchar_t winTitle[20];
		GetWindowText(msg.hwnd, winTitle, _countof(winTitle));
		if (!lstrcmp(winTitle, L"Traktor")) {
			static GESTURECONFIG gc[] = {
				{ 0, 0, GC_ALLGESTURES }, /* Turn off all gestures */
				{ GID_PAN, GC_PAN, 0, }   /* Turn on just the pan gesture */
			};

			firstTime = false;
			SetWindowSubclass(msg.hwnd, HookedWindowProc, 1, 0);
			SetGestureConfig(msg.hwnd, 0, _countof(gc), gc, sizeof(GESTURECONFIG));
		}
	}

	if (msg.message == WM_LBUTTONDOWN && isTouchMouseEvent()) {
		touching = true;
		touchCorrection.x = touchCorrection.y = 0;
	}
	if (msg.message == WM_LBUTTONUP && isTouchMouseEvent()) {
		touching = false;
	}
	if (touching && (msg.message >= WM_MOUSEFIRST) && (msg.message <= WM_MOUSELAST) && isTouchMouseEvent()) {
		POINTS &lPoint = MAKEPOINTS(msg.lParam);
		lPoint.x -= touchCorrection.x;
		lPoint.y -= touchCorrection.y;
	}

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

HMODULE hDLL;

/*
 * This is a temporary WindowProc hook that serves as the initial entry point to the DLL.
 * The loader installs this hook into the Traktor main UI thread, then sends a message to
 * make sure this function runs.
 *
 * This installs the actual hook from the context of the running Traktor, and also pins the
 * DLL inside Traktor to make sure it's not being unloaded. It runs inside the main UI thread
 * so we're safe against concurrency issues at this point.
 */
extern "C" __declspec(dllexport) LRESULT CALLBACK EntryHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	static bool firstTime = true;
	static TCHAR dllName[2048];
	if (firstTime) {
		#ifdef _DEBUG
			RedirectIOToConsole();
		#else
		    /* Prevent the DLL from unloading by incrementing its refcount.
			 * In debug mode the loader's hook will keep the DLL loaded and we want
			 * to be able to unload, so we don't do this in debug mode. */
			GetModuleFileName(hDLL, dllName, _countof(dllName));
			LoadLibrary(dllName);
		#endif

		/* Install the main GetMessage hook */
		hMessageHook = SetWindowsHookEx(WH_GETMESSAGE, MessageHook, NULL, GetCurrentThreadId());
		if (!hMessageHook)
			dprintf("Failed to install hook: %s\n", GetErrorMessage());

		/* Install the SetCursorPos hook */
		Mhook_SetHook((PVOID *)&OrigSetCursorPos, MySetCursorPos);
		firstTime = false;
	}

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
		hDLL = hModule;
		readConfig(hModule);
		break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
		break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
