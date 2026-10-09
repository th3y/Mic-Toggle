#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <mmsystem.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

constexpr wchar_t kAppName[] = L"MicToggle";
constexpr wchar_t kDialogClass[] = L"MicToggleHotkeyDialog";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kSettingsKey[] = L"Software\\MicToggle";
constexpr wchar_t kSoundOn[] = L"C:\\Windows\\Media\\Speech On.wav";
constexpr wchar_t kSoundOff[] = L"C:\\Windows\\Media\\Speech Off.wav";
constexpr WORD kDefaultHotkey = MAKEWORD('M', HOTKEYF_CONTROL | HOTKEYF_ALT);

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_MUTE_CHANGED = WM_APP + 2;
constexpr UINT kTrayId = 1;
constexpr int kHotkeyId = 1;

enum MenuCommand { CmdToggle = 100, CmdChangeHotkey, CmdNotifications, CmdSound, CmdAutostart, CmdExit };
enum DialogControl { IdHotkeyInput = 200 };

struct Icons {
	HICON smallOn, smallOff, largeOn, largeOff;
};

HINSTANCE gInstance;
HWND gWindow;
HWND gHotkeyDialog;
HFONT gFont;
UINT gTaskbarCreatedMsg;
Icons gIcons;
ComPtr<IMMDeviceEnumerator> gEnumerator;
ComPtr<IAudioEndpointVolume> gWatchedVolume;
bool gMuted;
bool gNotificationsEnabled;
bool gSoundEnabled;
WORD gHotkey;
wchar_t gHotkeyLabel[64];

class MuteWatcher : public IAudioEndpointVolumeCallback {
public:
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }

	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&refs_);
		if (count == 0) delete this;
		return count;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
			*out = static_cast<IAudioEndpointVolumeCallback*>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}

	HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
		PostMessageW(gWindow, WM_MUTE_CHANGED, data->bMuted, 0);
		return S_OK;
	}

private:
	LONG refs_ = 1;
};

MuteWatcher* gWatcher;

DWORD PremultipliedPixel(COLORREF color, double coverage) {
	BYTE alpha = static_cast<BYTE>(coverage * 255);
	DWORD red = GetRValue(color) * alpha / 255;
	DWORD green = GetGValue(color) * alpha / 255;
	DWORD blue = GetBValue(color) * alpha / 255;
	return (static_cast<DWORD>(alpha) << 24) | (red << 16) | (green << 8) | blue;
}

HICON CreateDotIcon(int size, COLORREF color) {
	BITMAPINFO bitmapInfo{};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = size;
	bitmapInfo.bmiHeader.biHeight = -size;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	HBITMAP colorBitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
	HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);
	auto pixels = static_cast<DWORD*>(bits);

	double center = size / 2.0;
	double radius = size * 0.375;
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			double distance = std::hypot(x + 0.5 - center, y + 0.5 - center);
			double coverage = std::clamp(radius + 0.5 - distance, 0.0, 1.0);
			pixels[y * size + x] = PremultipliedPixel(color, coverage);
		}
	}

	ICONINFO iconInfo{TRUE, 0, 0, maskBitmap, colorBitmap};
	HICON icon = CreateIconIndirect(&iconInfo);

	DeleteObject(colorBitmap);
	DeleteObject(maskBitmap);
	return icon;
}

Icons CreateIcons() {
	int smallSize = GetSystemMetrics(SM_CXSMICON);
	int largeSize = GetSystemMetrics(SM_CXICON);
	COLORREF green = RGB(34, 197, 94);
	COLORREF gray = RGB(128, 128, 128);
	return {
		CreateDotIcon(smallSize, green),
		CreateDotIcon(smallSize, gray),
		CreateDotIcon(largeSize, green),
		CreateDotIcon(largeSize, gray),
	};
}

void DestroyIcons(const Icons& icons) {
	DestroyIcon(icons.smallOn);
	DestroyIcon(icons.smallOff);
	DestroyIcon(icons.largeOn);
	DestroyIcon(icons.largeOff);
}

HFONT CreateMessageFont() {
	NONCLIENTMETRICSW metrics{sizeof(metrics)};
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
	return CreateFontIndirectW(&metrics.lfMessageFont);
}

ComPtr<IAudioEndpointVolume> GetVolume(IMMDevice* device) {
	ComPtr<IAudioEndpointVolume> volume;
	device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
	return volume;
}

ComPtr<IAudioEndpointVolume> GetDefaultMicVolume() {
	ComPtr<IMMDevice> device;
	if (FAILED(gEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) return nullptr;
	return GetVolume(device.Get());
}

bool IsDefaultMicMuted() {
	auto volume = GetDefaultMicVolume();
	BOOL muted = FALSE;
	if (volume) volume->GetMute(&muted);
	return muted != FALSE;
}

std::vector<ComPtr<IMMDevice>> GetActiveMics() {
	std::vector<ComPtr<IMMDevice>> mics;
	ComPtr<IMMDeviceCollection> devices;
	if (FAILED(gEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return mics;

	UINT count = 0;
	devices->GetCount(&count);
	for (UINT i = 0; i < count; ++i) {
		ComPtr<IMMDevice> device;
		if (SUCCEEDED(devices->Item(i, &device))) mics.push_back(device);
	}
	return mics;
}

void SetAllMicsMuted(bool muted) {
	for (auto& mic : GetActiveMics()) {
		if (auto volume = GetVolume(mic.Get())) volume->SetMute(muted, nullptr);
	}
}

std::wstring GetProcessName(DWORD processId) {
	std::wstring fallback = L"Unknown app (PID " + std::to_wstring(processId) + L")";

	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (!process) return fallback;

	wchar_t path[MAX_PATH];
	DWORD size = MAX_PATH;
	bool found = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
	CloseHandle(process);
	if (!found) return fallback;

	std::wstring name = path;
	name = name.substr(name.find_last_of(L'\\') + 1);
	if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
	return name;
}

void AddAppsRecordingFrom(IMMDevice* mic, std::vector<std::wstring>& apps) {
	ComPtr<IAudioSessionManager2> manager;
	if (FAILED(mic->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager))) return;

	ComPtr<IAudioSessionEnumerator> sessions;
	if (FAILED(manager->GetSessionEnumerator(&sessions))) return;

	int count = 0;
	sessions->GetCount(&count);
	for (int i = 0; i < count; ++i) {
		ComPtr<IAudioSessionControl> session;
		if (FAILED(sessions->GetSession(i, &session))) continue;

		AudioSessionState state;
		if (FAILED(session->GetState(&state)) || state != AudioSessionStateActive) continue;

		ComPtr<IAudioSessionControl2> details;
		if (FAILED(session.As(&details))) continue;

		DWORD processId = 0;
		details->GetProcessId(&processId);
		std::wstring name = GetProcessName(processId);
		if (std::find(apps.begin(), apps.end(), name) == apps.end()) apps.push_back(name);
	}
}

std::vector<std::wstring> GetAppsUsingMic() {
	std::vector<std::wstring> apps;
	for (auto& mic : GetActiveMics()) AddAppsRecordingFrom(mic.Get(), apps);
	return apps;
}

void StartWatchingDefaultMic() {
	gWatchedVolume = GetDefaultMicVolume();
	if (!gWatchedVolume) return;
	gWatcher = new MuteWatcher();
	gWatchedVolume->RegisterControlChangeNotify(gWatcher);
}

void StopWatchingDefaultMic() {
	if (gWatchedVolume && gWatcher) gWatchedVolume->UnregisterControlChangeNotify(gWatcher);
	if (gWatcher) gWatcher->Release();
	gWatcher = nullptr;
	gWatchedVolume.Reset();
}

NOTIFYICONDATAW BaseTrayData() {
	NOTIFYICONDATAW data{sizeof(data)};
	data.hWnd = gWindow;
	data.uID = kTrayId;
	return data;
}

void UpdateTrayIcon(DWORD action) {
	NOTIFYICONDATAW data = BaseTrayData();
	data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	data.uCallbackMessage = WM_TRAY;
	data.hIcon = gMuted ? gIcons.smallOff : gIcons.smallOn;
	wsprintfW(data.szTip, L"Mic %s (%s)", gMuted ? L"Muted" : L"Enabled", gHotkeyLabel);
	Shell_NotifyIconW(action, &data);
}

void ShowStateNotification() {
	NOTIFYICONDATAW data = BaseTrayData();
	data.uFlags = NIF_INFO;
	data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_NOSOUND;
	data.hBalloonIcon = gMuted ? gIcons.largeOff : gIcons.largeOn;
	lstrcpyW(data.szInfoTitle, L"Mic");
	lstrcpyW(data.szInfo, gMuted ? L"Muted" : L"Enabled");
	Shell_NotifyIconW(NIM_MODIFY, &data);
}

void PlayStateSound() {
	PlaySoundW(gMuted ? kSoundOff : kSoundOn, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

DWORD LoadSetting(const wchar_t* name, DWORD defaultValue) {
	DWORD value = 0;
	DWORD size = sizeof(value);
	LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
	return status == ERROR_SUCCESS ? value : defaultValue;
}

void SaveSetting(const wchar_t* name, DWORD value) {
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
	RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
	RegCloseKey(key);
}

void ToggleMic() {
	gMuted = !IsDefaultMicMuted();
	SetAllMicsMuted(gMuted);
	SaveSetting(L"Muted", gMuted);
	UpdateTrayIcon(NIM_MODIFY);
	if (gSoundEnabled) PlayStateSound();
	if (gNotificationsEnabled) ShowStateNotification();
}

UINT ToModifiers(BYTE flags) {
	UINT modifiers = MOD_NOREPEAT;
	if (flags & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
	if (flags & HOTKEYF_ALT) modifiers |= MOD_ALT;
	if (flags & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
	return modifiers;
}

void FormatHotkey(WORD hotkey, wchar_t* label) {
	BYTE key = LOBYTE(hotkey);
	BYTE flags = HIBYTE(hotkey);

	label[0] = L'\0';
	if (flags & HOTKEYF_CONTROL) lstrcatW(label, L"Ctrl+");
	if (flags & HOTKEYF_ALT) lstrcatW(label, L"Alt+");
	if (flags & HOTKEYF_SHIFT) lstrcatW(label, L"Shift+");

	LONG keyInfo = MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16;
	if (flags & HOTKEYF_EXT) keyInfo |= 1 << 24;

	wchar_t keyName[32];
	if (GetKeyNameTextW(keyInfo, keyName, 32) == 0) wsprintfW(keyName, L"0x%02X", key);
	lstrcatW(label, keyName);
}

WORD LoadHotkey() {
	DWORD value = LoadSetting(L"Hotkey", kDefaultHotkey);
	return LOBYTE(value) != 0 ? static_cast<WORD>(value) : kDefaultHotkey;
}

void SaveHotkey(WORD hotkey) {
	SaveSetting(L"Hotkey", hotkey);
}

void SetNotificationsEnabled(bool enabled) {
	gNotificationsEnabled = enabled;
	SaveSetting(L"Notifications", enabled);
}

void SetSoundEnabled(bool enabled) {
	gSoundEnabled = enabled;
	SaveSetting(L"Sound", enabled);
}

bool RegisterToggleHotkey(WORD hotkey) {
	return RegisterHotKey(gWindow, kHotkeyId, ToModifiers(HIBYTE(hotkey)), LOBYTE(hotkey)) != FALSE;
}

void UseHotkey(WORD hotkey) {
	gHotkey = hotkey;
	FormatHotkey(hotkey, gHotkeyLabel);
}

bool IsAutostartEnabled() {
	return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool enabled) {
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;

	if (enabled) {
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		wchar_t command[MAX_PATH + 2];
		wsprintfW(command, L"\"%s\"", path);
		RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command),
					   (lstrlenW(command) + 1) * sizeof(wchar_t));
	} else {
		RegDeleteValueW(key, kAppName);
	}
	RegCloseKey(key);
}

HWND AddControl(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
	HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
								   parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInstance, nullptr);
	SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
	return control;
}

void SaveHotkeyFromDialog(HWND dialog) {
	WORD hotkey = static_cast<WORD>(SendDlgItemMessageW(dialog, IdHotkeyInput, HKM_GETHOTKEY, 0, 0));

	if (LOBYTE(hotkey) == 0) {
		MessageBoxW(dialog, L"Press a shortcut.", kAppName, MB_ICONINFORMATION);
		return;
	}

	if (!RegisterToggleHotkey(hotkey)) {
		MessageBoxW(dialog, L"This shortcut is already taken.", kAppName, MB_ICONWARNING);
		return;
	}

	UnregisterHotKey(gWindow, kHotkeyId);
	UseHotkey(hotkey);
	SaveHotkey(hotkey);
	UpdateTrayIcon(NIM_MODIFY);
	DestroyWindow(dialog);
}

LRESULT CALLBACK HotkeyDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK) SaveHotkeyFromDialog(dialog);
		else if (LOWORD(wParam) == IDCANCEL) DestroyWindow(dialog);
		return 0;

	case WM_CLOSE:
		DestroyWindow(dialog);
		return 0;

	case WM_DESTROY:
		gHotkeyDialog = nullptr;
		RegisterToggleHotkey(gHotkey);
		return 0;
	}
	return DefWindowProcW(dialog, message, wParam, lParam);
}

void ShowHotkeyDialog() {
	if (gHotkeyDialog) {
		SetForegroundWindow(gHotkeyDialog);
		return;
	}

	UnregisterHotKey(gWindow, kHotkeyId);

	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	RECT frame{0, 0, 300, 112};
	AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);
	int width = frame.right - frame.left;
	int height = frame.bottom - frame.top;
	int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
	int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

	gHotkeyDialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kDialogClass, L"Cambiar atajo",
									style, x, y, width, height, nullptr, nullptr, gInstance, nullptr);

	AddControl(gHotkeyDialog, L"STATIC", L"Set a new shortcut:", 0, 16, 14, 268, 20, 0);
	HWND input = AddControl(gHotkeyDialog, HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, 16, 38, 268, 24, IdHotkeyInput);
	AddControl(gHotkeyDialog, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 112, 76, 82, 26, IDOK);
	AddControl(gHotkeyDialog, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 202, 76, 82, 26, IDCANCEL);

	SendMessageW(input, HKM_SETHOTKEY, gHotkey, 0);

	ShowWindow(gHotkeyDialog, SW_SHOW);
	SetForegroundWindow(gHotkeyDialog);
	SetFocus(input);
}

void ShowTrayMenu() {
	wchar_t toggleLabel[96];
	wsprintfW(toggleLabel, L"%s\t%s", gMuted ? L"Enable Mic" : L"Mute Mic", gHotkeyLabel);

	HMENU menu = CreatePopupMenu();

	auto apps = GetAppsUsingMic();
	if (!apps.empty()) {
		AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Apps using the mic:");
		for (auto& app : apps) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"	" + app).c_str());
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	}

	AppendMenuW(menu, MF_STRING, CmdToggle, toggleLabel);
	AppendMenuW(menu, MF_STRING, CmdChangeHotkey, L"Change Shortcut...");
	AppendMenuW(menu, MF_STRING | (gNotificationsEnabled ? MF_CHECKED : 0), CmdNotifications, L"Show Notifications");
	AppendMenuW(menu, MF_STRING | (gSoundEnabled ? MF_CHECKED : 0), CmdSound, L"Play Sound");
	AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), CmdAutostart, L"Start with Windows");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, CmdExit, L"Exit");

	POINT cursor;
	GetCursorPos(&cursor);
	SetForegroundWindow(gWindow);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, gWindow, nullptr);
	PostMessageW(gWindow, WM_NULL, 0, 0);
	DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == gTaskbarCreatedMsg) {
		UpdateTrayIcon(NIM_ADD);
		return 0;
	}

	switch (message) {
	case WM_HOTKEY:
		if (wParam == kHotkeyId) ToggleMic();
		return 0;

	case WM_TRAY:
		if (lParam == WM_LBUTTONUP) ToggleMic();
		else if (lParam == WM_RBUTTONUP) ShowTrayMenu();
		return 0;

	case WM_MUTE_CHANGED:
		if (gMuted != (wParam != 0)) {
			gMuted = wParam != 0;
			SaveSetting(L"Muted", gMuted);
			UpdateTrayIcon(NIM_MODIFY);
		}
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case CmdToggle: ToggleMic(); break;
		case CmdChangeHotkey: ShowHotkeyDialog(); break;
		case CmdNotifications: SetNotificationsEnabled(!gNotificationsEnabled); break;
		case CmdSound: SetSoundEnabled(!gSoundEnabled); break;
		case CmdAutostart: SetAutostart(!IsAutostartEnabled()); break;
		case CmdExit: DestroyWindow(window); break;
		}
		return 0;

	case WM_DESTROY:
		if (gHotkeyDialog) DestroyWindow(gHotkeyDialog);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

void RegisterWindowClasses() {
	WNDCLASSW mainClass{};
	mainClass.lpfnWndProc = WindowProc;
	mainClass.hInstance = gInstance;
	mainClass.lpszClassName = kAppName;
	RegisterClassW(&mainClass);

	WNDCLASSW dialogClass{};
	dialogClass.lpfnWndProc = HotkeyDialogProc;
	dialogClass.hInstance = gInstance;
	dialogClass.hIcon = gIcons.largeOn;
	dialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	dialogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	dialogClass.lpszClassName = kDialogClass;
	RegisterClassW(&dialogClass);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
	HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\MicToggleSingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

	gInstance = instance;

	INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_HOTKEY_CLASS};
	InitCommonControlsEx(&controls);

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&gEnumerator));

	gIcons = CreateIcons();
	gFont = CreateMessageFont();
	RegisterWindowClasses();

	gWindow = CreateWindowW(kAppName, kAppName, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
	gTaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
	gMuted = LoadSetting(L"Muted", IsDefaultMicMuted()) != 0;
	SetAllMicsMuted(gMuted);
	UseHotkey(LoadHotkey());
	gNotificationsEnabled = LoadSetting(L"Notifications", TRUE) != 0;
	gSoundEnabled = LoadSetting(L"Sound", TRUE) != 0;

	UpdateTrayIcon(NIM_ADD);
	StartWatchingDefaultMic();

	if (!RegisterToggleHotkey(gHotkey)) {
		wchar_t text[160];
		wsprintfW(text, L"Cant register shortcut %s. Already in use.", gHotkeyLabel);
		MessageBoxW(nullptr, text, kAppName, MB_ICONWARNING);
	}

	SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		if (gHotkeyDialog && IsDialogMessageW(gHotkeyDialog, &message)) continue;
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	UnregisterHotKey(gWindow, kHotkeyId);
	StopWatchingDefaultMic();
	UpdateTrayIcon(NIM_DELETE);
	DestroyIcons(gIcons);
	DeleteObject(gFont);
	gEnumerator.Reset();
	CoUninitialize();
	ReleaseMutex(singleInstance);
	CloseHandle(singleInstance);
	return 0;
}
