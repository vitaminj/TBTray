// dllmain.cpp : Defines the entry point for the DLL application.
#include "stdafx.h"
#include <stdio.h>
#include <vector>

const int WM_TUMALWAS = 2;

char *GetErrorMessage()
{
	static char msgbuf[1024];
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msgbuf, sizeof(msgbuf), NULL);
	return msgbuf;
}

std::vector<HHOOK> gMouseHooks;

bool isTouchMouseEvent()
{
	const LONG_PTR cSignatureMask = 0xFFFFFF00;
	const LONG_PTR cFromTouch = 0xFF515700;
	return (GetMessageExtraInfo() & cSignatureMask) == cFromTouch;
}

POINT touchOrigin, touchCorrection;
bool touching = false;

typedef BOOL(WINAPI *SetCursorPos_t)(_In_ int X, _In_ int Y);
SetCursorPos_t OrigSetCursorPos = (SetCursorPos_t)GetProcAddress(GetModuleHandle(L"user32"), "SetCursorPos");
BOOL WINAPI MySetCursorPos(_In_ int x, _In_ int y)
{
	printf("scp %i %i\n", x, y);
	if (touching) {
		POINT p;
		GetCursorPos(&p);
		touchCorrection.x = p.x - x;
		touchCorrection.y = p.y - y;
		return true;
	}
	else {
		return OrigSetCursorPos(x, y);
	}
}

LRESULT CALLBACK HookedWindowProc(
	HWND hWnd,
	UINT uMsg,
	WPARAM wParam,
	LPARAM lParam,
	UINT_PTR uIdSubclass,
	DWORD_PTR dwRefData
)
{
	if (uMsg == WM_APP && lParam == 0xDEADBEEF && wParam == 0xCAFE) {
		printf("self-remove\n");
		RemoveWindowSubclass(hWnd, HookedWindowProc, 1);
		Mhook_Unhook((PVOID *)&OrigSetCursorPos);
		return 0xBABE;
	}

	switch (uMsg) {
	case WM_TABLET_QUERYSYSTEMGESTURESTATUS:
		printf("question\n");
		return TABLET_DISABLE_PRESSANDHOLD;
	case WM_NCDESTROY:
		RemoveWindowSubclass(hWnd, HookedWindowProc, uIdSubclass);
		break;
	}
	return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

bool firstTime = true;
HHOOK hHook;

HWND hTraktorWindow;
LRESULT CALLBACK MessageHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	MSG &msg = *(MSG *)lParam;

	if (firstTime) {
		printf("First time\n");
		firstTime = false;
		if (!RegisterTouchWindow(hTraktorWindow, 0))
			printf("Error registering touch: %s\n", GetErrorMessage());

		BOOL disable = false;
		SetWindowFeedbackSetting(hTraktorWindow, FEEDBACK_TOUCH_PRESSANDHOLD, 0, sizeof(BOOL), &disable);
		SetWindowFeedbackSetting(hTraktorWindow, FEEDBACK_TOUCH_RIGHTTAP, 0, sizeof(BOOL), &disable);
	}

	if (msg.message == WM_TUMALWAS && msg.wParam == 1) {
		SetWindowSubclass(hTraktorWindow, HookedWindowProc, 1, 0);
		printf("ok ok %i %i\n", msg.wParam, msg.lParam);
		return 1;
	}

	if (msg.message == WM_TABLET_QUERYSYSTEMGESTURESTATUS) {
		printf("query\n");
		return TABLET_DISABLE_PRESSANDHOLD;
	}
	
	if (msg.message == WM_TOUCH) {
		printf("tatsch\n");
	}
	if (msg.message == WM_GESTURE) {
		printf("gesture\n");
	}

	if (msg.message == WM_LBUTTONDOWN && isTouchMouseEvent()) {
		touching = true;
		GetCursorPos(&touchOrigin);
		touchCorrection.x = touchCorrection.y = 0;
		printf("touch down %i %i\n", touchOrigin.x, touchOrigin.y);
	}
	if (msg.message == WM_LBUTTONUP && isTouchMouseEvent()) {
		touching = false;
		printf("touch up\n");
	}
	if (touching && (msg.message >= WM_MOUSEFIRST) && (msg.message <= WM_MOUSELAST) && isTouchMouseEvent()) {
		POINTS &lPoint = MAKEPOINTS(msg.lParam);
		printf("touchy mouse %i %i -> %i %i\n", lPoint.x, lPoint.y, lPoint.x - touchCorrection.x, lPoint.y - touchCorrection.y);
		lPoint.x -= touchCorrection.x;
		lPoint.y -= touchCorrection.y;
	}

	return CallNextHookEx(hHook, nCode, wParam, lParam);
}

void hookAllThreads(int idHook, HOOKPROC lpfn, HINSTANCE hmod)
{
	const HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnap == INVALID_HANDLE_VALUE)
		printf("Failed to create snapshot! %i\n", GetLastError());

	const DWORD pid = GetCurrentProcessId();
	THREADENTRY32 te;
	te.dwSize = sizeof(te);
	BOOL more = Thread32First(hSnap, &te);
	while (more) {
		if (te.th32OwnerProcessID == pid) {
			HHOOK hHook = SetWindowsHookEx(idHook, lpfn, hmod, te.th32ThreadID);
			if (hHook) // We can't hook non-GUI threads, that's okay.
				gMouseHooks.push_back(hHook);
		}

		more = Thread32Next(hSnap, &te);
	}

	CloseHandle(hSnap);
}

HANDLE hHookThread;
DWORD idHookThread;
void HookThread(HMODULE hModule)
{
	printf("Hook hook %x\n", GetCurrentThreadId());
	printf("waiting for window\n");
	while (!(hTraktorWindow = FindWindow(NULL, L"Traktor")))
		Sleep(100);

	printf("got it! %p hooking!\n", hTraktorWindow);

	DWORD idTraktorUIThread = GetWindowThreadProcessId(hTraktorWindow, NULL);
	hHook = SetWindowsHookEx(WH_GETMESSAGE, MessageHook, hModule, idTraktorUIThread);
	if (!hHook)
		printf("Failed to hook Traktor UI thread %x\n", idTraktorUIThread);
	else
		printf("Hooked Traktor UI thread %x\n", idTraktorUIThread);

	//hookAllThreads(WH_GETMESSAGE, MessageHook, hModule);
	Mhook_SetHook((PVOID *)&OrigSetCursorPos, MySetCursorPos);

	printf("%d %s\n", PostMessage(hTraktorWindow, WM_TUMALWAS, 1, 0), GetErrorMessage());

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	Mhook_Unhook((PVOID *)&OrigSetCursorPos);
	//for (HHOOK hook : gMouseHooks)
		if (!UnhookWindowsHookEx(hHook))
			printf("Failed to unhook %p: %s\n", hHook, GetErrorMessage());
}

void selfUnload(HMODULE hModule)
{
	printf("My handle: %p\n", hModule);
	if (IDYES == MessageBoxA(0, "Click Yes to unload DLL again", "Traktor Touch DLL", MB_ICONQUESTION | MB_YESNO)) {
		SendMessage(hTraktorWindow, WM_TUMALWAS, 2, 0);
		PostThreadMessage(idHookThread, WM_QUIT, 0, 0);
		WaitForSingleObject(hHookThread, INFINITE);
		FreeLibraryAndExitThread(hModule, 0);
	}
}

HMODULE myModule;

extern "C" __declspec(dllexport) LRESULT CALLBACK EntryHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	MSG &msg = *(MSG *)lParam;
	if (firstTime) {
		printf("First time\n");
		firstTime = false;
/*		if (!RegisterTouchWindow(hTraktorWindow, 0))
			printf("Error registering touch: %s\n", GetErrorMessage());

		BOOL disable = false;
		SetWindowFeedbackSetting(hTraktorWindow, FEEDBACK_TOUCH_PRESSANDHOLD, 0, sizeof(BOOL), &disable);
		SetWindowFeedbackSetting(hTraktorWindow, FEEDBACK_TOUCH_RIGHTTAP, 0, sizeof(BOOL), &disable); */
		SetWindowSubclass(msg.hwnd, HookedWindowProc, 1, 0);
		Mhook_SetHook((PVOID *)&OrigSetCursorPos, MySetCursorPos);
		printf("hook done\n");
	}

	if (msg.message == WM_LBUTTONDOWN && isTouchMouseEvent()) {
		touching = true;
		GetCursorPos(&touchOrigin);
		touchCorrection.x = touchCorrection.y = 0;
		printf("touch down %i %i\n", touchOrigin.x, touchOrigin.y);
	}
	if (msg.message == WM_LBUTTONUP && isTouchMouseEvent()) {
		touching = false;
		printf("touch up\n");
	}
	if (touching && (msg.message >= WM_MOUSEFIRST) && (msg.message <= WM_MOUSELAST) && isTouchMouseEvent()) {
		POINTS &lPoint = MAKEPOINTS(msg.lParam);
		printf("touchy mouse %i %i -> %i %i\n", lPoint.x, lPoint.y, lPoint.x - touchCorrection.x, lPoint.y - touchCorrection.y);
		lPoint.x -= touchCorrection.x;
		lPoint.y -= touchCorrection.y;
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
		myModule = hModule;
#ifdef _DEBUG
		RedirectIOToConsole();
#endif
/*		hHookThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)HookThread, hModule, 0, &idHookThread);
		if (!hHookThread)
			printf("Can't create hook thread: %s", GetErrorMessage());
			
#ifdef _DEBUG
		if (!CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)selfUnload, hModule, 0, NULL))
			printf("Can't create unload thread: %s", GetErrorMessage());
#endif*/
		break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
		break;
    case DLL_PROCESS_DETACH:
#ifdef _DEBUG
		CloseConsole();
#endif
        break;
    }
    return TRUE;
}

