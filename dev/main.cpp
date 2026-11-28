// 本软件不提供，不分发任何游戏本体，仅提供网络下载逻辑，真要起诉的话投诉BMCAPI
// 编译：g++ main.cpp -o UMCL.exe -mwindows -municode -std=c++17 -O2
//        -lcomctl32 -lgdiplus -lurlmon -lshell32 -lcomdlg32
//        -lwinhttp -lole32 -loleaut32 -luuid -lm -lpthread -lbcrypt
// 代码开始 1.0-alpha (绝对禁用 -D + 假死续命机制)

#define WIN32_LEAN_AND_MEAN

#include <objidl.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <filesystem>
#include <sstream>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <set>
#include <algorithm>
#include <regex>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;
using namespace Gdiplus;

#define ID_TAB_MAIN        1000
#define ID_BTN_DOWN        1001
#define ID_COMBO_VER       1002
#define ID_COMBO_LOADER    1003
#define ID_EDIT_LOG        1004
#define ID_COMBO_JAVA      1005
#define ID_BTN_LAUNCH      1006
#define ID_BTN_CHECK       1007

#define IDD_LOCAL_DLG      2000
#define IDC_LOCAL_LIST     2001
#define IDC_LOCAL_OK       2002
#define IDC_LOCAL_CANCEL   2003

static HWND g_hMainWnd   = nullptr;
static HINSTANCE g_hInst = nullptr;
static HWND g_hTab       = nullptr;
static HWND g_hLog       = nullptr;
static HWND g_hComboVer  = nullptr;
static ULONG_PTR g_gdiplusToken = 0;

static std::atomic<bool> g_bIsExitRequested{ false };
static std::atomic<DWORD> g_dwExtractPid{ 0 };

#define WM_APP_SWITCH_HOME (WM_APP + 1)
#define WM_APP_LOG         (WM_APP + 2)
#define WM_APP_FILL_VERS   (WM_APP + 3)

static bool HttpDownloadToFile(const std::wstring& url, const std::wstring& localPath);
static bool SmartDownloadMCFile(const std::wstring& url, const std::wstring& localPath);
static bool ExtractWithPowerShell(const std::wstring& archive, const std::wstring& destDir);
static bool Is32BitSystem();
static bool DownloadJavaForHost(const std::wstring& jdkVer);
static bool LaunchMinecraft(const std::wstring& gameDir, const std::wstring& mcVer, const std::wstring& jdkVer);
static std::wstring FindJavaExeRecursive(const std::wstring& dir);
static bool TryDownloadWithProxyFallback(const std::wstring& url, const std::wstring& localPath);
static std::wstring ResolveJdkVersion(const std::wstring& raw);
static bool DownloadVanillaJsonViaCpp(const std::wstring& exeDir, const std::wstring& mcVer);
static bool RunNodeScript(const std::wstring& exeDir, const std::wstring& nodeRelPath,
						  const std::vector<std::wstring>& args, const std::wstring& workDir);
static bool DownloadFromTxtList(const std::wstring& exeDir, const std::wstring& txtPath);
static bool ParseVanillaWithNode(const std::wstring& exeDir, const std::wstring& mcVer);
static bool ParseFabricWithNode(const std::wstring& exeDir, const std::wstring& mcVer);
static bool ParseAndInstallForgeWithNode(const std::wstring& exeDir, const std::wstring& mcVer, const std::wstring& jdkVer);
static std::wstring BuildClasspath(const std::wstring& gameDir, const std::wstring& actualVer, bool strictForge);
static bool ShowLocalVersionDialog(HWND hParent, const std::vector<std::wstring>& versions, std::wstring& outVer);
static std::wstring ExtractJsonStringValue(const std::string& content, const std::string& key);

static void AppendLog(const std::wstring& line)
{
	if (!g_hMainWnd) return;
	std::wstring* p = new std::wstring(line);
	PostMessageW(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)p);
}

static void AppendLogDirect(const std::wstring& line)
{
	if (!g_hLog) return;
	
	int lineCount = (int)SendMessageW(g_hLog, EM_GETLINECOUNT, 0, 0);
	
	if (lineCount >= 200)
	{
		std::wstring text = line + L"\r\n";
		SetWindowTextW(g_hLog, text.c_str()); 
		int len = GetWindowTextLengthW(g_hLog);
		SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
		SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
		return;
	}
	
	std::wstring text = line + L"\r\n";
	int len = GetWindowTextLengthW(g_hLog);
	SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
	SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
	SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty()) return L"";
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
	return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty()) return "";
	int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
	return s;
}

static std::wstring NormalizeWinPath(std::wstring s)
{
	for (auto& c : s) if (c == L'/') c = L'\\';
	return s;
}
static std::wstring CleanRelPath(std::wstring s)
{
	s = NormalizeWinPath(s);
	while (s.size() >= 2 && s[0] == L'.' && s[1] == L'\\') s = s.substr(2);
	while (s.size() >= 2 && s[0] == L'\\' && s[1] == L'.') s = s.substr(2);
	while (!s.empty() && s[0] == L'\\' && !(s.size() >= 2 && s[1] == L'\\')) s = s.substr(1);
	std::wstring out; out.reserve(s.size());
	wchar_t prev = 0;
	for (wchar_t c : s) { if (c == L'\\' && prev == L'\\') continue; out.push_back(c); prev = c; }
	while (!out.empty() && out.back() == L'\\') out.pop_back();
	return out;
}
static std::wstring ParentDir(const std::wstring& p)
{
	size_t pos = p.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return L".";
	return p.substr(0, pos);
}
static bool FileExistsW(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExistsW(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool EnsureDirectoryRecursiveW(const std::wstring& path)
{
	if (path.empty()) return false;
	if (DirExistsW(path)) return true;
	std::wstring cur;
	cur.reserve(path.size());
	for (size_t i = 0; i < path.size(); ++i)
	{
		wchar_t c = path[i];
		cur.push_back(c);
		bool isSep = (c == L'\\' || c == L'/');
		bool isEnd = (i + 1 == path.size());
		if (!isSep && !isEnd) continue;
		if (cur.size() == 2 && cur[1] == L':') continue;
		if (cur == L"\\\\?\\") continue;
		if (cur.size() == 3 && cur[1] == L':' && cur[2] == L'\\') continue;
		DWORD a = GetFileAttributesW(cur.c_str());
		if (a == INVALID_FILE_ATTRIBUTES)
		{
			if (!CreateDirectoryW(cur.c_str(), nullptr))
				if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
		}
				else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
	}
	return true;
}
static std::wstring GetExeDir()
{
	wchar_t buf[MAX_PATH] = {};
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return L".";
	std::wstring full(buf, n);
	size_t pos = full.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return L".";
	return full.substr(0, pos);
}
static std::wstring GetGameDir()
{
	std::wstring dir = GetExeDir() + L"\\.minecraft";
	if (!DirExistsW(dir)) EnsureDirectoryRecursiveW(dir);
	return dir;
}
static bool Is32BitSystem()
{
	BOOL isWow64 = FALSE;
	if (IsWow64Process(GetCurrentProcess(), &isWow64))
		if (isWow64) return false;
	SYSTEM_INFO si{};
	GetNativeSystemInfo(&si);
	return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL;
}
static std::wstring FindJavaExeRecursive(const std::wstring& dir)
{
	if (dir.empty() || !DirExistsW(dir)) return L"";
	std::wstring directBin = dir + L"\\bin";
	if (DirExistsW(directBin))
	{
		std::wstring exe = directBin + L"\\java.exe";
		if (FileExistsW(exe)) return exe;
	}
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return L"";
	std::wstring found;
	do
	{
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
			found = FindJavaExeRecursive(dir + L"\\" + fd.cFileName);
			if (!found.empty()) break;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return found;
}

static std::string ReadWholeFileA(const std::wstring& path)
{
	std::ifstream ifs(fs::path(path), std::ios::binary);
	if (!ifs) return "";
	std::stringstream ss; ss << ifs.rdbuf();
	return ss.str();
}

static std::wstring CalculateSHA1(const std::wstring& filePath)
{
	std::ifstream file(fs::path(filePath), std::ios::binary);
	if (!file) return L"";
	
	BCRYPT_ALG_HANDLE hAlg = nullptr;
	BCRYPT_HASH_HANDLE hHash = nullptr;
	DWORD cbHashObject = 0, cbData = 0, cbHash = 0, cbResult = 0;
	PBYTE pbHashObject = nullptr, pbHash = nullptr;
	std::vector<BYTE> buffer(65536);
	std::wstring result;
	
	if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0)
	{
		BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbResult, 0);
		BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbResult, 0);
		
		pbHashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHashObject);
		pbHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHash);
		
		if (pbHashObject && pbHash)
		{
			if (BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, nullptr, 0, 0) == 0)
			{
				while (file.read((char*)buffer.data(), buffer.size()) || file.gcount() > 0)
				{
					BCryptHashData(hHash, buffer.data(), (ULONG)file.gcount(), 0);
				}
				BCryptFinishHash(hHash, pbHash, cbHash, 0);
				
				wchar_t hex[3];
				for (DWORD i = 0; i < cbHash; ++i)
				{
					swprintf_s(hex, L"%02x", pbHash[i]);
					result += hex;
				}
				BCryptDestroyHash(hHash);
			}
		}
		if (pbHashObject) HeapFree(GetProcessHeap(), 0, pbHashObject);
		if (pbHash) HeapFree(GetProcessHeap(), 0, pbHash);
		BCryptCloseAlgorithmProvider(hAlg, 0);
	}
	return result;
}

struct CheckTask {
	std::wstring url;
	std::wstring absPath;
	std::string expectSha1;
};

static std::mutex g_taskMutex;
static std::vector<CheckTask> g_mismatchFiles;

static void ProcessCheckFile(const std::wstring& gameDir, const std::wstring& filePath, const std::string& expectSha1, const std::wstring& url)
{
	if (!FileExistsW(filePath))
	{
		std::lock_guard<std::mutex> lock(g_taskMutex);
		g_mismatchFiles.push_back({ url, filePath, expectSha1 });
		return;
	}
	
	std::wstring sha1 = CalculateSHA1(filePath);
	std::string sha1Str = WideToUtf8(sha1);
	
	if (_stricmp(sha1Str.c_str(), expectSha1.c_str()) != 0)
	{
		AppendLog(L"[CHECK] 校验失败: " + filePath);
		std::lock_guard<std::mutex> lock(g_taskMutex);
		g_mismatchFiles.push_back({ url, filePath, expectSha1 });
	}
}

static void DoFixDownload()
{
	if (g_mismatchFiles.empty()) return;
	
	std::atomic<int> okCount(0), failCount(0);
	
	auto worker = [&]() {
		while (true) {
			CheckTask task;
			{
				std::lock_guard<std::mutex> lock(g_taskMutex);
				if (g_mismatchFiles.empty()) return;
				task = g_mismatchFiles.back();
				g_mismatchFiles.pop_back();
			}
			
			std::wstring fixUrl = task.url;
			if (fixUrl.find(L"resources.download.minecraft.net") != std::wstring::npos) {
				size_t pos = fixUrl.find(L"resources.download.minecraft.net");
				fixUrl.replace(pos, 32, L"bmclapi2.bangbang93.com/assets");
			} else if (fixUrl.find(L"libraries.minecraft.net") != std::wstring::npos) {
				size_t pos = fixUrl.find(L"libraries.minecraft.net");
				fixUrl.replace(pos, 23, L"bmclapi2.bangbang93.com/maven");
			}
			
			AppendLog(L"[FIX] 重新下载: " + task.absPath);
			bool ok = false;
			if (SmartDownloadMCFile(task.url, task.absPath)) ok = true;
			else if (SmartDownloadMCFile(fixUrl, task.absPath)) ok = true;
			
			if (ok) okCount++;
			else failCount++;
		}
	};
	
	std::vector<std::thread> threads;
	for (int i = 0; i < 8; ++i) threads.emplace_back(worker);
	for (auto& t : threads) t.join();
	
	AppendLog(L"[FIX] 修复完成！成功: " + std::to_wstring(okCount) + L"，失败: " + std::to_wstring(failCount));
}

static void RunFullCheck(const std::wstring& gameDir, const std::wstring& mcVer)
{
	AppendLog(L"[CHECK] 开始进行全量资源文件校验 (8线程)");
	
	std::wstring versionJson = gameDir + L"\\versions\\" + mcVer + L"\\" + mcVer + L".json";
	if (!FileExistsW(versionJson))
	{
		AppendLog(L"[ERROR] 找不到版本 JSON: " + versionJson);
		return;
	}
	
	std::string json = ReadWholeFileA(versionJson);
	
	std::regex libRegex(R"re("path"\s*:\s*"([^"]+)"\s*,\s*"sha1"\s*:\s*"([^"]+)"\s*,\s*"size"\s*:\s*\d+\s*,\s*"url"\s*:\s*"([^"]+)")re");
	auto libBegin = std::sregex_iterator(json.begin(), json.end(), libRegex);
	auto libEnd = std::sregex_iterator();
	for (auto it = libBegin; it != libEnd; ++it)
	{
		std::wstring path = Utf8ToWide((*it)[1].str());
		std::string sha1 = (*it)[2].str();
		std::wstring url = Utf8ToWide((*it)[3].str());
		
		if (path.find(L"-natives-macos") != std::wstring::npos ||
			path.find(L"-natives-linux") != std::wstring::npos ||
			path.find(L"-natives-osx") != std::wstring::npos ||
			path.find(L"java-objc-bridge") != std::wstring::npos ||
			path.find(L"netty-transport-native-epoll") != std::wstring::npos)
		{
			continue;
		}
		
		std::wstring absPath = NormalizeWinPath(gameDir + L"\\libraries\\" + path);
		ProcessCheckFile(gameDir, absPath, sha1, url);
	}
	
	std::regex clientRegex(R"re("client"\s*:\s*\{[^}]*"sha1"\s*:\s*"([^"]+)"[^}]*"url"\s*:\s*"([^"]+)")re");
	std::smatch match;
	if (std::regex_search(json, match, clientRegex))
	{
		std::string sha1 = match[1].str();
		std::wstring url = Utf8ToWide(match[2].str());
		std::wstring absPath = NormalizeWinPath(gameDir + L"\\versions\\" + mcVer + L"\\" + mcVer + L".jar");
		ProcessCheckFile(gameDir, absPath, sha1, url);
	}
	
	std::wstring assetIndexId = ExtractJsonStringValue(json, "assetIndex");
	if (!assetIndexId.empty())
	{
		std::wstring indexJsonPath = NormalizeWinPath(gameDir + L"\\assets\\indexes\\" + assetIndexId + L".json");
		if (FileExistsW(indexJsonPath))
		{
			std::string assetJson = ReadWholeFileA(indexJsonPath);
			std::regex assetRegex(R"re("hash"\s*:\s*"([^"]+)"[^}]*"path"\s*:\s*"([^"]+)")re");
			auto assetBegin = std::sregex_iterator(assetJson.begin(), assetJson.end(), assetRegex);
			auto assetEnd = std::sregex_iterator();
			for (auto it = assetBegin; it != assetEnd; ++it)
			{
				std::string hash = (*it)[1].str();
				std::wstring path = Utf8ToWide((*it)[2].str());
				std::wstring absPath = NormalizeWinPath(gameDir + L"\\assets\\objects\\" + path);
				std::wstring url = L"https://resources.download.minecraft.net/" + path.substr(0, 2) + L"/" + path.substr(2);
				ProcessCheckFile(gameDir, absPath, hash, url);
			}
		}
	}
	
	AppendLog(L"[CHECK] 校验完成，发现 " + std::to_wstring(g_mismatchFiles.size()) + L" 个文件存在差异或缺失。");
	DoFixDownload();
}

static std::wstring GetXmxArg()
{
	MEMORYSTATUSEX memInfo;
	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	if (GlobalMemoryStatusEx(&memInfo))
	{
		DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
		DWORDLONG physMemMB = totalPhysMem / (1024 * 1024);
		
		if (physMemMB <= 3096) {
			AppendLog(L"[JAVA] 检测到低内存配置 (" + std::to_wstring(physMemMB) + L"MB)，采用 512M + G1GC 优化");
			return L"-Xmx512M -Xms256M -XX:+UseG1GC -XX:MaxMetaspaceSize=128M -XX:+UseStringDeduplication";
		}
		else if (physMemMB <= 8192) {
			AppendLog(L"[JAVA] 检测到中等配置 (" + std::to_wstring(physMemMB) + L"MB)，采用 2G 内存");
			return L"-Xmx2G -Xms1G";
		}
		else {
			AppendLog(L"[JAVA] 检测到高配置 (" + std::to_wstring(physMemMB) + L"MB)，采用 3G 内存");
			return L"-Xmx3G -Xms1G";
		}
	}
	AppendLog(L"[WRN] 无法获取物理内存，默认采用 2G 内存");
	return L"-Xmx2G -Xms1G";
}

static std::wstring ToAbs_GameDir(const std::wstring& exeDir, const std::wstring& rel)
{
	if (rel.size() >= 2 && rel[1] == L':') return NormalizeWinPath(rel);
	if (rel.size() >= 2 && rel[0] == L'\\' && rel[1] == L'\\') return NormalizeWinPath(rel);
	std::wstring cleaned = CleanRelPath(rel);
	if (cleaned.empty()) return NormalizeWinPath(exeDir + L"\\.minecraft");
	if (_wcsnicmp(cleaned.c_str(), L".minecraft", 11) == 0 && (cleaned.size() == 11 || cleaned[11] == L'\\'))
		return NormalizeWinPath(exeDir + L"\\" + cleaned);
	auto startsWith = [&](const wchar_t* p) -> bool
	{
		size_t n = wcslen(p);
		return _wcsnicmp(cleaned.c_str(), p, n) == 0 && (cleaned.size() == n || cleaned[n] == L'\\');
	};
	if (startsWith(L"libraries") || startsWith(L"versions") || startsWith(L"assets") ||
		startsWith(L"temp") || startsWith(L"OpenJDK-JRE") || startsWith(L"logs") || startsWith(L"saves"))
		return NormalizeWinPath(exeDir + L"\\.minecraft\\" + cleaned);
	bool looksLikeVersion = !cleaned.empty() && (iswdigit(cleaned[0]) != 0);
	if (!looksLikeVersion) looksLikeVersion = (cleaned.find(L'.') != std::wstring::npos && cleaned.find(L'\\') != std::wstring::npos);
	if (looksLikeVersion) return NormalizeWinPath(exeDir + L"\\.minecraft\\versions\\" + cleaned);
	return NormalizeWinPath(exeDir + L"\\.minecraft\\" + cleaned);
}
static std::wstring ToAbs_ExeDir(const std::wstring& exeDir, const std::wstring& rel)
{
	if (rel.size() >= 2 && rel[1] == L':') return NormalizeWinPath(rel);
	if (rel.size() >= 2 && rel[0] == L'\\' && rel[1] == L'\\') return NormalizeWinPath(rel);
	std::wstring cleaned = CleanRelPath(rel);
	if (cleaned.empty()) return NormalizeWinPath(exeDir);
	return NormalizeWinPath(exeDir + L"\\" + cleaned);
}

static void KillChildProcesses()
{
	system("taskkill /F /IM node.exe >nul 2>&1");
	system("taskkill /F /IM powershell.exe >nul 2>&1");
}

static void CleanupTempFiles()
{
	std::wstring gameDir = GetGameDir();
	std::vector<std::wstring> filesToDelete = {
		gameDir + L"\\args.txt",
		gameDir + L"\\temp\\cp.txt",
		gameDir + L"\\temp\\launch_main.txt"
	};
	for (const auto& f : filesToDelete) {
		if (FileExistsW(f)) {
			DeleteFileW(f.c_str());
		}
	}
}

static std::wstring PsEscapeSingleQuote(const std::wstring& s)
{
	std::wstring out; out.reserve(s.size() + 8);
	for (wchar_t c : s) { if (c == L'\'') out += L"''"; else out.push_back(c); }
	return out;
}

// ============ 下载函数（加入 5 秒假死检测 + 15 分钟无增长才杀） ============
static bool HttpDownloadToFile(const std::wstring& url, const std::wstring& localPath)
{
	if (g_bIsExitRequested) return false;
	if (!EnsureDirectoryRecursiveW(ParentDir(localPath))) return false;
	DeleteFileW(localPath.c_str());
	std::wstring urlEsc = PsEscapeSingleQuote(url);
	std::wstring pathEsc = PsEscapeSingleQuote(localPath);
	
	std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command \""
	L"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; "
	L"$wc = New-Object System.Net.WebClient; "
	L"$wc.Headers.Add('User-Agent', 'Mozilla/5.0'); "
	L"$wc.DownloadFile('" + urlEsc + L"', '" + pathEsc + L"'); "
	L"if ($wc) { $wc.Dispose() }\"";
	
	STARTUPINFOW si{}; si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	std::wstring mutableCmd = cmd;
	BOOL ok = CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
							 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	if (!ok) return false;
	
	DWORD startTime = GetTickCount();
	DWORD lastChangeTime = startTime;
	ULARGE_INTEGER lastSize = { 0 };
	bool finished = false;
	bool killed = false;
	
	for (;;)
	{
		if (g_bIsExitRequested)
		{
			TerminateProcess(pi.hProcess, 1);
			killed = true;
			break;
		}
		
		DWORD wr = WaitForSingleObject(pi.hProcess, 200);
		if (wr == WAIT_OBJECT_0) { finished = true; break; }
		if (wr == WAIT_FAILED) break;
		
		DWORD now = GetTickCount();
		
		
		// 连续 15 分钟无增长才杀
		if (now - lastChangeTime >= 900000)
		{
			AppendLog(L"[DL] 连续 15 分钟无进度，判定假死，强制终止");
			TerminateProcess(pi.hProcess, 1);
			killed = true;
			break;
		}
	}
	
	DWORD code = 1;
	if (finished) GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	if (killed || g_bIsExitRequested) { DeleteFileW(localPath.c_str()); return false; }
	if (!finished || code != 0) { DeleteFileW(localPath.c_str()); return false; }
	return FileExistsW(localPath);
}

static bool TryDownloadWithProxyFallback(const std::wstring& url, const std::wstring& localPath)
{
	if (g_bIsExitRequested) return false;
	if (url.find(L"api.adoptium.net") != std::wstring::npos) return false;
	if (url.find(L"https://v6.gh-proxy.org/") == 0) return SmartDownloadMCFile(url, localPath);
	if (SmartDownloadMCFile(url, localPath)) return true;
	if (g_bIsExitRequested) return false;
	if (url.find(L"https://") == 0)
	{
		std::wstring proxied = L"https://v6.gh-proxy.org/" + url;
		return SmartDownloadMCFile(proxied, localPath);
	}
	return false;
}
static bool SmartDownloadMCFile(const std::wstring& url, const std::wstring& localPath)
{
	if (g_bIsExitRequested) return false;
	if (!EnsureDirectoryRecursiveW(ParentDir(localPath))) return false;
	return HttpDownloadToFile(url, localPath);
}

static bool DownloadVanillaJsonViaCpp(const std::wstring& exeDir, const std::wstring& mcVer)
{
	if (g_bIsExitRequested) return false;
	std::wstring relDir  = CleanRelPath(L".minecraft\\versions\\" + mcVer);
	std::wstring relJson = CleanRelPath(relDir + L"\\" + mcVer + L".json");
	std::wstring absDir  = exeDir + L"\\" + relDir;
	std::wstring absJson = exeDir + L"\\" + relJson;
	if (FileExistsW(absJson)) return true;
	if (!EnsureDirectoryRecursiveW(absDir)) return false;
	return SmartDownloadMCFile(L"https://bmclapi2.bangbang93.com/version/" + mcVer + L"/json", absJson);
}

static bool ExtractWithPowerShell(const std::wstring& archive, const std::wstring& destDir)
{
	if (g_bIsExitRequested) return false;
	std::wstring archiveEsc = PsEscapeSingleQuote(archive);
	std::wstring destEsc    = PsEscapeSingleQuote(destDir);
	std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '"
	+ archiveEsc + L"' -DestinationPath '" + destEsc + L"' -Force\"";
	STARTUPINFOW si{}; si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	std::wstring cmdLine = cmd;
	if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
	g_dwExtractPid.store(pi.dwProcessId);
	bool bFinished = false, bKilled = false;
	for (;;) {
		if (g_bIsExitRequested) { TerminateProcess(pi.hProcess, 1); bKilled = true; break; }
		DWORD wr = WaitForSingleObject(pi.hProcess, 200);
		if (wr == WAIT_OBJECT_0) { bFinished = true; break; }
		if (wr == WAIT_FAILED) break;
	}
	DWORD code = 0;
	if (bFinished) GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	g_dwExtractPid.store(0);
	if (bKilled || g_bIsExitRequested) return false;
	return bFinished && code == 0;
}
static void RemoveDirectoryRecursiveW(const std::wstring& dir)
{
	if (dir.empty() || !DirExistsW(dir)) return;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
			std::wstring sub = dir + L"\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryRecursiveW(sub);
			else DeleteFileW(sub.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}
static bool CopyDirContentsTo(const std::wstring& src, const std::wstring& dst, bool bMove)
{
	if (!DirExistsW(src)) return false;
	if (!EnsureDirectoryRecursiveW(dst)) return false;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return true;
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
		std::wstring s = src + L"\\" + fd.cFileName;
		std::wstring d = dst + L"\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			CopyDirContentsTo(s, d, bMove);
			if (bMove) RemoveDirectoryW(s.c_str());
		} else {
			if (bMove) MoveFileExW(s.c_str(), d.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
			else CopyFileW(s.c_str(), d.c_str(), FALSE);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return true;
}
static std::wstring FindDirContainingBin(const std::wstring& root)
{
	if (!DirExistsW(root)) return L"";
	if (DirExistsW(root + L"\\bin")) return root;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return L"";
	std::wstring result;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
		std::wstring sub = root + L"\\" + fd.cFileName;
		if (DirExistsW(sub + L"\\bin")) { result = sub; break; }
		WIN32_FIND_DATAW fd2{};
		HANDLE h2 = FindFirstFileW((sub + L"\\*").c_str(), &fd2);
		if (h2 != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
				if (wcscmp(fd2.cFileName, L".") == 0 || wcscmp(fd2.cFileName, L"..") == 0) continue;
				std::wstring sub2 = sub + L"\\" + fd2.cFileName;
				if (DirExistsW(sub2 + L"\\bin")) { result = sub2; break; }
			} while (FindNextFileW(h2, &fd2));
			FindClose(h2);
		}
		if (!result.empty()) break;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return result;
}

static std::wstring ResolveJdkVersion(const std::wstring& raw)
{
	if (raw.empty()) return L"17";
	if (_wcsicmp(raw.c_str(), L"auto") == 0) return L"17";
	if (_wcsicmp(raw.c_str(), L"自动") == 0) return L"17";
	if (_wcsicmp(raw.c_str(), L"8") == 0)  return L"8";
	if (_wcsicmp(raw.c_str(), L"11") == 0) return L"11";
	if (_wcsicmp(raw.c_str(), L"17") == 0) return L"17";
	if (_wcsicmp(raw.c_str(), L"21") == 0) return L"21";
	if (_wcsicmp(raw.c_str(), L"25") == 0) return L"25";
	return L"17";
}
static std::wstring GetJavaRootDir() { return GetGameDir() + L"\\OpenJDK-JRE"; }
static std::wstring GetJavaVersionDir(const std::wstring& ver) { return GetJavaRootDir() + L"\\JRE" + ver; }
static std::wstring Get64BitHardcodedUrl(const std::wstring& ver)
{
	if (ver == L"8") return L"https://download.bell-sw.com/java/8u514+5/bellsoft-jre8u514+5-windows-amd64.zip";
	if (ver == L"11") return L"https://download.bell-sw.com/java/11.0.33.1+5/bellsoft-jre11.0.33.1+5-windows-amd64.zip";
	if (ver == L"17") return L"https://download.bell-sw.com/java/17.0.21.1+5/bellsoft-jre17.0.21.1+5-windows-amd64.zip";
	if (ver == L"21") return L"https://download.bell-sw.com/java/21.0.13.1+5/bellsoft-jre21.0.13.1+5-windows-amd64.zip";
	if (ver == L"25") return L"https://download.bell-sw.com/java/25.0.5.1+5/bellsoft-jre25.0.5.1+5-windows-amd64.zip";
	return L"";
}
static std::wstring Get32BitHardcodedUrl(const std::wstring& ver)
{
	if (ver == L"8") return L"https://download.bell-sw.com/java/8u514+5/bellsoft-jre8u514+5-windows-i586.zip";
	if (ver == L"11") return L"https://download.bell-sw.com/java/11.0.33.1+5/bellsoft-jre11.0.33.1+5-windows-i586.zip";
	if (ver == L"17") return L"https://download.bell-sw.com/java/17.0.21.1+5/bellsoft-jre17.0.21.1+5-windows-i586.zip";
	if (ver == L"21") return L"https://download.bell-sw.com/java/21.0.13.1+5/bellsoft-jre21.0.13.1+5-windows-i586.zip";
	return L"";
}
static bool DownloadJavaForHost(const std::wstring& jdkVer)
{
	if (g_bIsExitRequested) return false;
	std::wstring exeDir = GetExeDir();
	bool isAuto = jdkVer.empty() || _wcsicmp(jdkVer.c_str(), L"auto") == 0 || _wcsicmp(jdkVer.c_str(), L"自动") == 0;
	std::wstring resolved = ResolveJdkVersion(jdkVer);
	bool is32 = Is32BitSystem();
	if (is32 && (resolved == L"21" || resolved == L"25")) { AppendLog(L"[JAVA] 回退 17"); resolved = L"17"; isAuto = false; }
	std::wstring javaDir = GetJavaVersionDir(resolved);
	std::wstring tempDir = javaDir + L"_temp";
	if (DirExistsW(javaDir)) {
		if (!FindJavaExeRecursive(javaDir).empty()) { AppendLog(L"[JAVA] 已有 Java " + resolved + L"，跳过"); return true; }
		RemoveDirectoryRecursiveW(javaDir);
	}
	std::wstring url;
	bool useApi = false;
	if (is32) url = Get32BitHardcodedUrl(resolved);
	else {
		if (isAuto) { url = L"https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jre/hotspot/normal/eclipse"; useApi = true; }
		else url = Get64BitHardcodedUrl(resolved);
	}
	std::wstring zipPath = exeDir + L"\\java" + resolved + L".zip";
	bool ok = false;
	if (useApi) {
		if (SmartDownloadMCFile(url, zipPath)) ok = true;
		else if (!g_bIsExitRequested) ok = TryDownloadWithProxyFallback(Get64BitHardcodedUrl(L"17"), zipPath);
	} else ok = TryDownloadWithProxyFallback(url, zipPath);
	if (g_bIsExitRequested) return false;
	if (!ok) { AppendLog(L"[ERROR] [JAVA] 下载失败"); return false; }
	RemoveDirectoryRecursiveW(tempDir);
	if (!EnsureDirectoryRecursiveW(tempDir)) return false;
	if (!ExtractWithPowerShell(zipPath, tempDir)) { RemoveDirectoryRecursiveW(tempDir); AppendLog(L"[ERROR] [JAVA] 解压失败"); return false; }
	std::wstring realRoot = FindDirContainingBin(tempDir);
	if (realRoot.empty()) { RemoveDirectoryRecursiveW(tempDir); DeleteFileW(zipPath.c_str()); AppendLog(L"[ERROR] [JAVA] 解压后找不到bin"); return false; }
	if (!EnsureDirectoryRecursiveW(javaDir)) return false;
	CopyDirContentsTo(realRoot, javaDir, true);
	RemoveDirectoryRecursiveW(tempDir);
	DeleteFileW(zipPath.c_str());
	if (!FileExistsW(javaDir + L"\\bin\\java.exe") && FindJavaExeRecursive(javaDir).empty()) { AppendLog(L"[ERROR] [JAVA] 安装后无java.exe"); return false; }
	AppendLog(L"[JAVA] Java " + resolved + L" 安装完成");
	return true;
}

struct VersionFetchResult { std::vector<std::wstring> versions; std::wstring error; };
static void FetchMinecraftVersionsThread(HWND hComboVer, std::wstring manifestPath)
{
	VersionFetchResult* result = new VersionFetchResult();
	if (g_bIsExitRequested) { result->error = L"exit"; PostMessageW(g_hMainWnd, WM_APP_FILL_VERS, (WPARAM)hComboVer, (LPARAM)result); return; }
	
	struct MSource { const wchar_t* name; std::wstring url; };
	std::vector<MSource> mirrors;
	mirrors.push_back({ L"BMCLAPI", L"https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json" });
	mirrors.push_back({ L"光速",    L"https://bsmirror.hkmc.online/mc/game/version_manifest_v2.json" });
	mirrors.push_back({ L"官方",    L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json" });
	
	bool got = false;
	for (auto& s : mirrors) {
		if (g_bIsExitRequested) { result->error = L"exit"; PostMessageW(g_hMainWnd, WM_APP_FILL_VERS, (WPARAM)hComboVer, (LPARAM)result); return; }
		AppendLog(L"[VER] 尝试源 " + std::wstring(s.name) + L" 下载版本清单");
		if (HttpDownloadToFile(s.url, manifestPath)) {
			std::ifstream chk(fs::path(manifestPath), std::ios::binary | std::ios::ate);
			if (chk && chk.tellg() > 0) { got = true; AppendLog(L"[VER] 版本清单成功（" + std::wstring(s.name) + L"）"); break; }
			DeleteFileW(manifestPath.c_str());
		}
		if (g_bIsExitRequested) { result->error = L"exit"; PostMessageW(g_hMainWnd, WM_APP_FILL_VERS, (WPARAM)hComboVer, (LPARAM)result); return; }
		DeleteFileW(manifestPath.c_str());
		AppendLog(L"[VER] 源 " + std::wstring(s.name) + L" 失败");
	}
	if (!got) { result->error = L"下载版本清单失败"; PostMessageW(g_hMainWnd, WM_APP_FILL_VERS, (WPARAM)hComboVer, (LPARAM)result); return; }
	
	try {
		std::ifstream ifs(fs::path(manifestPath), std::ios::binary);
		if (!ifs) { result->error = L"无法打开清单"; PostMessageW(g_hMainWnd, WM_APP_FILL_VERS, (WPARAM)hComboVer, (LPARAM)result); return; }
		std::stringstream ss; ss << ifs.rdbuf();
		std::string content = ss.str();
		size_t pos = 0;
		while ((pos = content.find("\"id\"", pos)) != std::string::npos) {
			size_t colon = content.find(':', pos + 4);
			if (colon == std::string::npos) break;
			size_t q1 = content.find('"', colon + 1);
			if (q1 == std::string::npos) break;
			size_t q2 = content.find('"', q1 + 1);
			if (q2 == std::string::npos) break;
			std::string id = content.substr(q1 + 1, q2 - q1 - 1);
			size_t next = content.find("\"type\"", q2);
			std::string type;
			if (next != std::string::npos) {
				size_t c2 = content.find(':', next + 6);
				if (c2 != std::string::npos) {
					size_t t1 = content.find('"', c2 + 1);
					size_t t2 = (t1 != std::string::npos) ? content.find('"', t1 + 1) : std::string::npos;
					if (t1 != std::string::npos && t2 != std::string::npos) type = content.substr(t1 + 1, t2 - t1 - 1);
				}
			}
			if (type == "release" && !id.empty()) result->versions.push_back(Utf8ToWide(id));
			pos = q2;
		}
		if (result->versions.empty()) result->error = L"未解析到版本";
	} catch (...) { result->error = L"解析异常"; }
	PostMessageW(g_hMainWnd, WM_APP_FILL_VERS, (WPARAM)hComboVer, (LPARAM)result);
}
static void OnFillVersions(HWND hComboVer, VersionFetchResult* result)
{
	if (!result) return;
	auto fallback = [&]() {
		SendMessageW(hComboVer, CB_RESETCONTENT, 0, 0);
		SendMessageW(hComboVer, CB_ADDSTRING, 0, (LPARAM)L"1.20.1");
		SendMessageW(hComboVer, CB_SETCURSEL, 0, 0);
	};
	if (!result->error.empty()) { AppendLog(L"[VER] " + result->error + L"，回退 1.20.1"); fallback(); delete result; return; }
	if (result->versions.empty()) { AppendLog(L"[VER] 版本为空，回退 1.20.1"); fallback(); delete result; return; }
	SendMessageW(hComboVer, CB_RESETCONTENT, 0, 0);
	for (auto& v : result->versions) SendMessageW(hComboVer, CB_ADDSTRING, 0, (LPARAM)v.c_str());
	SendMessageW(hComboVer, CB_SETCURSEL, 0, 0);
	AppendLog(L"[VER] 已加载 " + std::to_wstring(result->versions.size()) + L" 个正式版");
	delete result;
}

static bool RunNodeScript(const std::wstring& exeDir, const std::wstring& nodeRelPath,
						  const std::vector<std::wstring>& args, const std::wstring& workDir)
{
	if (g_bIsExitRequested) return false;
	std::wstring nodeExe = exeDir + L"\\" + CleanRelPath(nodeRelPath);
	if (!FileExistsW(nodeExe)) { AppendLog(L"[ERROR] 找不到 node.exe: " + nodeExe); return false; }
	std::wstring cmdLine = L"\"" + nodeExe + L"\"";
	for (const auto& a : args) { cmdLine += L" \""; cmdLine += a; cmdLine += L"\""; }
	std::wstring curDir = workDir.empty() ? exeDir : workDir;
	STARTUPINFOW si{}; si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	std::wstring mutableCmd = cmdLine;
	BOOL ok = CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
							 nullptr, curDir.c_str(), &si, &pi);
	if (!ok) { AppendLog(L"[ERROR] CreateProcessW 失败: " + std::to_wstring(GetLastError())); return false; }
	bool finished = false, killed = false;
	for (;;) {
		if (g_bIsExitRequested) { TerminateProcess(pi.hProcess, 1); killed = true; break; }
		DWORD wr = WaitForSingleObject(pi.hProcess, 200);
		if (wr == WAIT_OBJECT_0) { finished = true; break; }
		if (wr == WAIT_FAILED) break;
	}
	DWORD code = 0;
	if (finished) GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	if (killed || g_bIsExitRequested) return false;
	if (!finished) { AppendLog(L"[ERROR] Node等待失败"); return false; }
	if (code != 0) { AppendLog(L"[ERROR] Node.js 退出码为: " + std::to_wstring(code)); return false; }
	return true;
}

struct DownloadTask {
	std::wstring url;
	std::wstring absPath;
};

static bool DownloadTaskList(const std::vector<DownloadTask>& initialTasks, bool isRetry)
{
	if (initialTasks.empty()) return true;
	
	std::mutex mtx;
	std::queue<DownloadTask> q;
	for (auto& t : initialTasks) q.push(t);
	
	std::vector<DownloadTask> failedTasks;
	std::mutex failedMtx;
	
	std::atomic<int> okCount{ 0 };
	const int THREAD_COUNT = 8;
	std::vector<std::thread> threads;
	threads.reserve(THREAD_COUNT);
	
	for (int i = 0; i < THREAD_COUNT; ++i)
	{
		threads.emplace_back([&]()
							 {
			for (;;)
			{
				if (g_bIsExitRequested) return;
				DownloadTask t;
				{
					std::lock_guard<std::mutex> lk(mtx);
					if (q.empty()) return;
					t = q.front();
					q.pop();
				}
				if (FileExistsW(t.absPath)) { okCount.fetch_add(1); continue; }
				
				if (isRetry && t.url.find(L"resources.download.minecraft.net") != std::wstring::npos) {
					size_t pos = t.url.find(L"resources.download.minecraft.net");
					t.url.replace(pos, 32, L"bmclapi2.bangbang93.com/assets");
				}
				
				if (!isRetry) {
					size_t slash = t.absPath.find_last_of(L"\\/");
					std::wstring fileName = (slash != std::wstring::npos) ? t.absPath.substr(slash + 1) : t.absPath;
					AppendLog(L"[TXT] 下载: " + fileName);
				}
				
				if (SmartDownloadMCFile(t.url, t.absPath)) okCount.fetch_add(1);
				else {
					std::lock_guard<std::mutex> lk(failedMtx);
					failedTasks.push_back(t);
				}
			}
		});
	}
	for (auto& th : threads) if (th.joinable()) th.join();
	
	if (!failedTasks.empty())
	{
		if (isRetry) AppendLog(L"[WRN] 第 2 轮重试仍有 " + std::to_wstring(failedTasks.size()) + L" 个文件失败，尝试第 3 轮...");
		else AppendLog(L"[WRN] 发现 " + std::to_wstring(failedTasks.size()) + L" 个文件下载失败，开始第二轮 BMCLAPI 强制重试...");
		return DownloadTaskList(failedTasks, true);
	}
	return true;
}

static bool DownloadFromTxtList(const std::wstring& exeDir, const std::wstring& txtPath)
{
	if (g_bIsExitRequested) return false;
	if (!FileExistsW(txtPath)) { AppendLog(L"[ERROR] 下载清单不存在: " + txtPath); return false; }
	std::ifstream ifs(fs::path(txtPath), std::ios::binary);
	if (!ifs) return false;
	
	std::vector<DownloadTask> tasks;
	std::string line;
	while (std::getline(ifs, line)) {
		if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line = line.substr(3);
		while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) line.erase(line.begin());
		while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
		if (line.empty()) continue;
		size_t sep = line.find('|');
		if (sep == std::string::npos) continue;
		std::string urlPart = line.substr(0, sep);
		std::string pathPart = line.substr(sep + 1);
		while (!urlPart.empty() && (urlPart.front() == ' ' || urlPart.front() == '\t')) urlPart.erase(urlPart.begin());
		while (!urlPart.empty() && (urlPart.back() == ' ' || urlPart.back() == '\t')) urlPart.pop_back();
		while (!pathPart.empty() && (pathPart.front() == ' ' || pathPart.front() == '\t')) pathPart.erase(pathPart.begin());
		while (!pathPart.empty() && (pathPart.back() == ' ' || pathPart.back() == '\t')) pathPart.pop_back();
		if (urlPart.empty() || pathPart.empty()) continue;
		
		std::wstring wUrl = Utf8ToWide(urlPart);
		std::wstring wPath = Utf8ToWide(pathPart);
		std::wstring absPath = ToAbs_GameDir(exeDir, wPath);
		if (FileExistsW(absPath)) continue;
		tasks.push_back({ wUrl, absPath });
	}
	
	int total = (int)tasks.size();
	if (total == 0) { AppendLog(L"[TXT] 所有文件已存在，跳过"); return true; }
	AppendLog(L"[TXT] 待下载: " + std::to_wstring(total) + L" 个文件，启用 8 线程");
	
	bool ok = DownloadTaskList(tasks, false);
	if (!ok) {
		AppendLog(L"[ERROR] 仍有文件彻底失败，请重新点击下载");
		return false;
	}
	AppendLog(L"[TXT] 全部文件下载成功！");
	return true;
}

static std::wstring FindListFile(const std::wstring& exeDir, const std::wstring& mcVer, const std::wstring& fileName)
{
	std::vector<std::wstring> candidates;
	if (!mcVer.empty()) candidates.push_back(ToAbs_ExeDir(exeDir, L".minecraft\\versions\\" + mcVer + L"\\" + fileName));
	candidates.push_back(ToAbs_ExeDir(exeDir, L".minecraft\\" + fileName));
	candidates.push_back(ToAbs_ExeDir(exeDir, L".minecraft\\temp\\" + fileName));
	for (auto& c : candidates) if (FileExistsW(c)) { AppendLog(L"[TXT] 找到清单: " + c); return c; }
	return L"";
}

static bool ParseVanillaWithNode(const std::wstring& exeDir, const std::wstring& mcVer)
{
	if (g_bIsExitRequested) return false;
	std::wstring verDir = exeDir + L"\\.minecraft\\versions\\" + mcVer;
	if (!EnsureDirectoryRecursiveW(verDir)) {
		AppendLog(L"[ERROR] 无法创建版本目录: " + verDir);
		return false;
	}
	
	AppendLog(L"[VANILLA] 步骤 1/2：准备原版 JSON...");
	if (!DownloadVanillaJsonViaCpp(exeDir, mcVer)) {
		AppendLog(L"[ERROR] 前置下载原版 JSON 失败");
		return false;
	}
	
	std::wstring jsAbs   = ToAbs_ExeDir(exeDir, L"Node(YouMustNeed)v13\\parse_vanilla.js");
	std::wstring jsonAbs = ToAbs_ExeDir(exeDir, L".minecraft\\versions\\" + mcVer + L"\\" + mcVer + L".json");
	
	AppendLog(L"[VANILLA] 步骤 2/2：调用 Node 解析并下载原版全量（核心/库/素材）...");
	std::vector<std::wstring> args; 
	args.push_back(jsAbs); 
	args.push_back(jsonAbs);
	
	if (!RunNodeScript(exeDir, L"Node(YouMustNeed)v13\\node.exe", args, exeDir)) {
		AppendLog(L"[ERROR] 原版 Node 解析失败");
		return false;
	}
	
	std::wstring listPath = FindListFile(exeDir, mcVer, L"download_list.txt");
	if (listPath.empty()) {
		AppendLog(L"[ERROR] 找不到 download_list.txt 清单文件");
		return false;
	}
	
	if (!DownloadFromTxtList(exeDir, listPath)) {
		AppendLog(L"[ERROR] 下载原版依赖失败");
		return false;
	}
	
	std::wstring coreJarPath = verDir + L"\\" + mcVer + L".jar";
	if (!FileExistsW(coreJarPath)) {
		AppendLog(L"[VANILLA] 正在下载原版核心 jar (BMCLAPI)...");
		std::wstring coreUrl = L"https://bmclapi2.bangbang93.com/version/" + mcVer + L"/client";
		if (!SmartDownloadMCFile(coreUrl, coreJarPath)) {
			AppendLog(L"[ERROR] 原版核心 jar 下载失败！");
			return false;
		}
	}
	
	AppendLog(L"[VANILLA] 原版流水线完成，已确保核心、assets、依赖全部就绪");
	return true;
}

static bool ParseFabricWithNode(const std::wstring& exeDir, const std::wstring& mcVer)
{
	if (g_bIsExitRequested) return false;
	std::wstring vanillaJar = exeDir + L"\\.minecraft\\versions\\" + mcVer + L"\\" + mcVer + L".jar";
	if (!FileExistsW(vanillaJar)) {
		AppendLog(L"[ERROR] [FABRIC] 流水线中断：前置原版核心缺失 " + vanillaJar);
		return false;
	}
	
	std::wstring fabricVer = mcVer + L"-fabric";
	std::wstring verDir = exeDir + L"\\.minecraft\\versions\\" + fabricVer;
	EnsureDirectoryRecursiveW(verDir);
	
	std::wstring jsAbs       = ToAbs_ExeDir(exeDir, L"Node(YouMustNeed)v13\\parse_fabric.js");
	std::wstring versionsAbs = ToAbs_ExeDir(exeDir, L".minecraft\\versions");
	
	AppendLog(L"[FABRIC] 调用 Node 生成 fabric_classpath 和 launch_config...");
	std::vector<std::wstring> args; args.push_back(jsAbs); args.push_back(mcVer); args.push_back(versionsAbs);
	
	if (!RunNodeScript(exeDir, L"Node(YouMustNeed)v13\\node.exe", args, exeDir)) {
		AppendLog(L"[ERROR] [FABRIC] Node 执行失败");
		return false;
	}
	
	std::wstring listPath = verDir + L"\\fabric_download_list.txt";
	AppendLog(L"[FABRIC] 尝试读取专属库清单: " + listPath);
	if (FileExistsW(listPath)) {
		DownloadFromTxtList(exeDir, listPath);
	} else {
		AppendLog(L"[WRN] [FABRIC] 未发现 fabric_download_list.txt，跳过额外下载");
	}
	
	return true;
}

static bool ParseAndInstallForgeWithNode(const std::wstring& exeDir, const std::wstring& mcVer, const std::wstring& jdkVer)
{
	if (g_bIsExitRequested) return false;
	
	std::wstring vanillaJar = exeDir + L"\\.minecraft\\versions\\" + mcVer + L"\\" + mcVer + L".jar";
	if (!FileExistsW(vanillaJar)) {
		AppendLog(L"[ERROR] [FORGE] 流水线中断：前置原版核心缺失 " + vanillaJar);
		return false;
	}
	
	std::wstring jsFetchAbs = ToAbs_ExeDir(exeDir, L"Node(YouMustNeed)v13\\parse_forge.js");
	std::wstring tempAbs    = ToAbs_ExeDir(exeDir, L".minecraft\\temp");
	std::vector<std::wstring> argsFetch; argsFetch.push_back(jsFetchAbs); argsFetch.push_back(mcVer); argsFetch.push_back(tempAbs);
	
	AppendLog(L"[FORGE] 调用 Node 生成安装器下载清单...");
	if (!RunNodeScript(exeDir, L"Node(YouMustNeed)v13\\node.exe", argsFetch, exeDir)) {
		AppendLog(L"[ERROR] [FORGE] Node 获取清单失败");
		return false;
	}
	
	std::wstring tempRoot = exeDir + L"\\.minecraft\\temp";
	std::wstring listPath;
	
	AppendLog(L"[FORGE] 正在 " + tempRoot + L" 下寻找以 " + mcVer + L" 开头的子目录...");
	if (DirExistsW(tempRoot)) {
		std::error_code ec;
		for (auto& p : fs::directory_iterator(fs::path(tempRoot), ec)) {
			if (ec) break;
			if (!p.is_directory()) continue;
			std::wstring folderName = p.path().filename().wstring();
			if (folderName.find(mcVer) == 0) {
				std::wstring potentialList = p.path().wstring() + L"\\forge_download_list.txt";
				if (FileExistsW(potentialList)) {
					listPath = potentialList;
					AppendLog(L"[FORGE] 命中专属子目录清单: " + listPath);
					break;
				}
			}
		}
	}
	
	if (listPath.empty()) {
		listPath = FindListFile(exeDir, mcVer, L"download_list_forge.txt");
		if (listPath.empty()) {
			AppendLog(L"[ERROR] [FORGE] 找不到 forge_download_list.txt");
			return false;
		}
	}
	
	AppendLog(L"[FORGE] 开始下载安装器及前置...");
	if (!DownloadFromTxtList(exeDir, listPath)) { AppendLog(L"[ERROR] [FORGE] 下载依赖失败"); return false; }
	
	std::wstring resolvedJdk = ResolveJdkVersion(jdkVer);
	std::wstring javaAbs = ToAbs_ExeDir(exeDir, L".minecraft\\OpenJDK-JRE\\JRE" + resolvedJdk + L"\\bin\\java.exe");
	if (!FileExistsW(javaAbs)) javaAbs = ToAbs_ExeDir(exeDir, L".minecraft\\OpenJDK-JRE\\JRE17\\bin\\java.exe");
	if (!FileExistsW(javaAbs)) { AppendLog(L"[ERROR] [FORGE] 找不到 Java 路径"); return false; }
	
	std::wstring installerAbs;
	AppendLog(L"[FORGE] 正在寻找真实的 forge-*-installer.jar...");
	if (DirExistsW(tempRoot)) {
		std::error_code ec;
		for (auto& p : fs::recursive_directory_iterator(fs::path(tempRoot), ec)) {
			if (ec) break;
			if (!p.is_regular_file()) continue;
			std::wstring fname = p.path().filename().wstring();
			if (fname.find(L"forge") != std::wstring::npos &&
				fname.find(L"installer") != std::wstring::npos &&
				fname.find(L".jar") != std::wstring::npos)
			{
				installerAbs = p.path().wstring();
				AppendLog(L"[FORGE] 命中真实安装器: " + installerAbs);
				break;
			}
		}
	}
	
	if (installerAbs.empty()) {
		std::vector<std::wstring> fallbacks = {
			ToAbs_ExeDir(exeDir, L".minecraft\\temp\\forge-installer.jar"),
			ToAbs_ExeDir(exeDir, L".minecraft\\temp\\1.20.1-47.4.29\\forge-1.20.1-47.4.29-installer.jar")
		};
		for (auto& fb : fallbacks) {
			if (FileExistsW(fb)) { installerAbs = fb; AppendLog(L"[FORGE] 兜底命中: " + installerAbs); break; }
		}
	}
	
	if (installerAbs.empty()) { 
		AppendLog(L"[ERROR] [FORGE] 彻底找不到 installer.jar！"); 
		return false; 
	}
	AppendLog(L"[FORGE] 使用安装器: " + installerAbs);
	
	std::wstring gameRootDir = exeDir + L"\\.minecraft";
	std::wstring installCmd = L"\"" + javaAbs + L"\" -jar \"" + installerAbs + L"\" --installClient \"" + gameRootDir + L"\"";
	
	AppendLog(L"[FORGE] 正在拉起安装器（黑框框控制台模式）...");
	AppendLog(L"[FORGE] 执行命令: " + installCmd);
	
	STARTUPINFOW si{}; 
	si.cb = sizeof(si);
	
	PROCESS_INFORMATION pi{};
	std::vector<wchar_t> cmdBuffer(installCmd.begin(), installCmd.end());
	cmdBuffer.push_back(L'\0');
	
	BOOL ok = CreateProcessW(
							 nullptr, &cmdBuffer[0],
							 nullptr, nullptr, FALSE,
							 CREATE_NEW_CONSOLE,
							 nullptr,
							 gameRootDir.c_str(),
							 &si, &pi
							 );
	
	if (!ok) {
		AppendLog(L"[ERROR] [FORGE] 启动安装器失败，GetLastError() = " + std::to_wstring(GetLastError()));
		return false;
	}
	
	DWORD startTime = GetTickCount();
	bool finished = false;
	bool killed = false;
	
	for (;;) {
		if (g_bIsExitRequested) {
			TerminateProcess(pi.hProcess, 1);
			killed = true;
			break;
		}
		DWORD wr = WaitForSingleObject(pi.hProcess, 200);
		if (wr == WAIT_OBJECT_0) { finished = true; break; }
		if (wr == WAIT_FAILED) break;
		
		if (GetTickCount() - startTime > 300000) {
			AppendLog(L"[WRN] [FORGE] 安装超时（5分钟），强制终止！");
			TerminateProcess(pi.hProcess, 1);
			killed = true;
			break;
		}
	}
	
	DWORD exitCode = 0;
	if (finished) GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	
	if (killed || g_bIsExitRequested) {
		AppendLog(L"[ERROR] [FORGE] 安装被强制中断！");
		return false;
	}
	
	if (!finished) {
		AppendLog(L"[ERROR] [FORGE] 安装器进程异常退出或等待失败！");
		return false;
	}
	
	std::wstring expectedVerDir;
	std::wstring versionsRoot = exeDir + L"\\.minecraft\\versions";
	bool found = false;
	if (DirExistsW(versionsRoot)) {
		std::error_code ec;
		for (auto& p : fs::directory_iterator(fs::path(versionsRoot), ec)) {
			if (ec) break;
			if (!p.is_directory()) continue;
			std::wstring folderName = p.path().filename().wstring();
			if (folderName.find(mcVer + L"-forge") == 0) {
				expectedVerDir = p.path().wstring();
				found = true;
				break;
			}
		}
	}
	
	if (!found) {
		AppendLog(L"[ERROR] [FORGE] 安装器已退出（exitCode=" + std::to_wstring(exitCode) + L"），但未在 versions 下找到 " + mcVer + L"-forge 目录！");
		AppendLog(L"[ERROR] [FORGE] 这可能是因为：1. 网络问题导致依赖下载失败；2. 安装器内部报错但没输出；3. Java 版本不兼容。");
		return false;
	}
	
	AppendLog(L"[FORGE] Forge 安装流水线完美收官！目标目录: " + expectedVerDir);
	return true;
}

static std::wstring GetSelectedMcVersion()
{
	HWND h = GetDlgItem(g_hMainWnd, ID_COMBO_VER);
	if (!h) return L"1.20.1";
	int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
	if (sel < 0) return L"1.20.1";
	wchar_t buf[128] = {};
	SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)buf);
	return buf;
}
static std::wstring GetSelectedLoader()
{
	HWND h = GetDlgItem(g_hMainWnd, ID_COMBO_LOADER);
	if (!h) return L"Vanilla";
	int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
	if (sel < 0) return L"Vanilla";
	wchar_t buf[64] = {};
	SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)buf);
	if (_wcsicmp(buf, L"原版") == 0) return L"Vanilla";
	return buf;
}
static std::wstring GetSelectedJavaVersion()
{
	HWND h = GetDlgItem(g_hMainWnd, ID_COMBO_JAVA);
	if (!h) return L"auto";
	int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
	if (sel < 0) return L"auto";
	wchar_t buf[64] = {};
	SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)buf);
	if (_wcsicmp(buf, L"自动") == 0) return L"auto";
	return buf;
}

static std::vector<std::wstring> GetLocalVersions()
{
	std::vector<std::wstring> out;
	std::wstring versionsDir = GetGameDir() + L"\\versions";
	if (!DirExistsW(versionsDir)) return out;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((versionsDir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
		std::wstring name = fd.cFileName;
		std::wstring jsonPath = versionsDir + L"\\" + name + L"\\" + name + L".json";
		if (FileExistsW(jsonPath)) out.push_back(name);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

struct PickCtx { const std::vector<std::wstring>* versions; int pickedIndex; bool done; };
static BOOL CALLBACK SetChildFontProc(HWND hCtrl, LPARAM lp) { SendMessageW(hCtrl, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE; }
static LRESULT CALLBACK LocalVerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	PickCtx* ctx = (PickCtx*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
	switch (msg)
	{
		case WM_CREATE: {
		CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
		ctx = (PickCtx*)cs->lpCreateParams;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)ctx);
		CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 15, 15, 335, 200, hWnd, (HMENU)IDC_LOCAL_LIST, g_hInst, nullptr);
		CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 190, 235, 80, 28, hWnd, (HMENU)IDC_LOCAL_OK, g_hInst, nullptr);
		CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 235, 80, 28, hWnd, (HMENU)IDC_LOCAL_CANCEL, g_hInst, nullptr);
		HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		EnumChildWindows(hWnd, SetChildFontProc, (LPARAM)hFont);
		HWND hList = GetDlgItem(hWnd, IDC_LOCAL_LIST);
		if (ctx && ctx->versions) {
			for (auto& v : *ctx->versions) SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)v.c_str());
			if (!ctx->versions->empty()) SendMessageW(hList, LB_SETCURSEL, 0, 0);
		}
		return 0;
	}
		case WM_COMMAND: {
			int id = LOWORD(wParam);
			if (id == IDC_LOCAL_OK) {
				HWND hList = GetDlgItem(hWnd, IDC_LOCAL_LIST);
				int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
				if (ctx) { ctx->pickedIndex = (sel == LB_ERR) ? 0 : sel; ctx->done = true; }
				DestroyWindow(hWnd); return 0;
			}
			if (id == IDC_LOCAL_CANCEL) {
				if (ctx) { ctx->pickedIndex = -1; ctx->done = true; }
				DestroyWindow(hWnd); return 0;
			}
			if (id == IDC_LOCAL_LIST && HIWORD(wParam) == LBN_DBLCLK) {
				HWND hList = GetDlgItem(hWnd, IDC_LOCAL_LIST);
				int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
				if (ctx) { ctx->pickedIndex = (sel == LB_ERR) ? 0 : sel; ctx->done = true; }
				DestroyWindow(hWnd); return 0;
			}
			return 0;
		}
	case WM_CLOSE:
		if (ctx) { ctx->pickedIndex = -1; ctx->done = true; }
		DestroyWindow(hWnd);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool ShowLocalVersionDialog(HWND hParent, const std::vector<std::wstring>& versions, std::wstring& outVer)
{
	if (versions.empty()) return false;
	static const wchar_t* kClass = L"UMCL_PICK_VERSION";
	static bool classRegistered = false;
	if (!classRegistered) {
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc); wc.lpfnWndProc = LocalVerWndProc;
		wc.hInstance = g_hInst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = kClass;
		RegisterClassExW(&wc); classRegistered = true;
	}
	PickCtx ctx; ctx.versions = &versions; ctx.pickedIndex = -1; ctx.done = false;
	HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kClass, L"选择本地版本",
								WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 380, 320, hParent, nullptr, g_hInst, &ctx);
	if (!hDlg) return false;
	RECT rcP, rcD; GetWindowRect(hParent, &rcP); GetWindowRect(hDlg, &rcD);
	int w = rcD.right - rcD.left, h = rcD.bottom - rcD.top;
	int x = rcP.left + ((rcP.right - rcP.left) - w) / 2;
	int y = rcP.top + ((rcP.bottom - rcP.top) - h) / 2;
	SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
	ShowWindow(hDlg, SW_SHOW); UpdateWindow(hDlg);
	EnableWindow(hParent, FALSE);
	while (!ctx.done && IsWindow(hDlg)) {
		MSG msg;
		if (!GetMessageW(&msg, nullptr, 0, 0)) break;
		if (!IsDialogMessageW(hDlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
	}
	EnableWindow(hParent, TRUE);
	if (IsWindow(hDlg)) DestroyWindow(hDlg);
	if (ctx.pickedIndex < 0 || ctx.pickedIndex >= (int)versions.size()) return false;
	outVer = versions[ctx.pickedIndex];
	return true;
}

static const wchar_t* GetUIFontFace(bool bold = false)
{
	static const wchar_t* normal = L"Microsoft YaHei";
	static const wchar_t* normalBold = L"Microsoft YaHei";
	static bool init = false;
	if (!init) {
		OSVERSIONINFOEXW osvi = { sizeof(osvi) };
		DWORDLONG const dwlConditionMask = VerSetConditionMask(
															   VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL),
															   VER_MINORVERSION, VER_GREATER_EQUAL);
		osvi.dwMajorVersion = 10;
		osvi.dwMinorVersion = 0;
		osvi.dwPlatformId = VER_PLATFORM_WIN32_NT;
		if (VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_PLATFORMID, dwlConditionMask)) {
			normal = L"Microsoft YaHei UI";
			normalBold = L"Microsoft YaHei UI";
		}
		init = true;
	}
	return bold ? normalBold : normal;
}

static HFONT CreateFontByFace(const wchar_t* face, int pt, bool bold)
{
	LOGFONTW lf{}; lf.lfHeight = -MulDiv(pt, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72);
	lf.lfWeight = bold ? FW_BOLD : FW_NORMAL; lf.lfCharSet = DEFAULT_CHARSET;
	wcscpy_s(lf.lfFaceName, face); return CreateFontIndirectW(&lf);
}
static void CreateControls(HWND hWnd)
{
	g_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 10, 10, 860, 470, hWnd, (HMENU)ID_TAB_MAIN, g_hInst, nullptr);
	TCITEMW tci{}; tci.mask = TCIF_TEXT;
	tci.pszText = (LPWSTR)L"主页"; TabCtrl_InsertItem(g_hTab, 0, &tci);
	tci.pszText = (LPWSTR)L"下载"; TabCtrl_InsertItem(g_hTab, 1, &tci);
	g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 30, 60, 820, 390, hWnd, (HMENU)ID_EDIT_LOG, g_hInst, nullptr);
	SendMessageW(g_hLog, WM_SETFONT, (WPARAM)CreateFontByFace(L"宋体", 10, false), TRUE);
	g_hComboVer = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_NOINTEGRALHEIGHT, 30, 46, 160, 500, hWnd, (HMENU)ID_COMBO_VER, g_hInst, nullptr);
	SendMessageW(g_hComboVer, CB_ADDSTRING, 0, (LPARAM)L"1.20.1"); SendMessageW(g_hComboVer, CB_SETCURSEL, 0, 0);
	HWND hL = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 200, 46, 140, 200, hWnd, (HMENU)ID_COMBO_LOADER, g_hInst, nullptr);
	SendMessageW(hL, CB_ADDSTRING, 0, (LPARAM)L"原版"); SendMessageW(hL, CB_ADDSTRING, 0, (LPARAM)L"Fabric"); SendMessageW(hL, CB_ADDSTRING, 0, (LPARAM)L"Forge"); SendMessageW(hL, CB_SETCURSEL, 0, 0);
	HWND hJ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 350, 46, 120, 200, hWnd, (HMENU)ID_COMBO_JAVA, g_hInst, nullptr);
	SendMessageW(hJ, CB_ADDSTRING, 0, (LPARAM)L"自动"); SendMessageW(hJ, CB_ADDSTRING, 0, (LPARAM)L"8"); SendMessageW(hJ, CB_ADDSTRING, 0, (LPARAM)L"11"); SendMessageW(hJ, CB_ADDSTRING, 0, (LPARAM)L"17"); SendMessageW(hJ, CB_ADDSTRING, 0, (LPARAM)L"21"); SendMessageW(hJ, CB_ADDSTRING, 0, (LPARAM)L"25"); SendMessageW(hJ, CB_SETCURSEL, 0, 0);
	
	HFONT hFontYahei = CreateFontByFace(GetUIFontFace(), 10, false);
	
	HWND hDown = CreateWindowExW(0, L"BUTTON", L"开始下载", WS_CHILD | BS_PUSHBUTTON, 490, 46, 110, 28, hWnd, (HMENU)ID_BTN_DOWN, g_hInst, nullptr); SendMessageW(hDown, WM_SETFONT, (WPARAM)hFontYahei, TRUE);
	HWND hCheck = CreateWindowExW(0, L"BUTTON", L"检查资源", WS_CHILD | BS_PUSHBUTTON, 610, 46, 110, 28, hWnd, (HMENU)ID_BTN_CHECK, g_hInst, nullptr); SendMessageW(hCheck, WM_SETFONT, (WPARAM)hFontYahei, TRUE);
	HWND hLaunch = CreateWindowExW(0, L"BUTTON", L"启动游戏", WS_CHILD | BS_PUSHBUTTON, 730, 46, 110, 28, hWnd, (HMENU)ID_BTN_LAUNCH, g_hInst, nullptr); SendMessageW(hLaunch, WM_SETFONT, (WPARAM)hFontYahei, TRUE);
	ShowWindow(hDown, SW_HIDE); ShowWindow(hCheck, SW_HIDE); ShowWindow(hLaunch, SW_HIDE);
	TabCtrl_SetCurSel(g_hTab, 0);
	ShowWindow(GetDlgItem(hWnd, ID_EDIT_LOG), SW_SHOW);
	ShowWindow(GetDlgItem(hWnd, ID_COMBO_VER), SW_HIDE);
	ShowWindow(GetDlgItem(hWnd, ID_COMBO_LOADER), SW_HIDE);
	ShowWindow(GetDlgItem(hWnd, ID_COMBO_JAVA), SW_HIDE);
	ShowWindow(GetDlgItem(hWnd, ID_BTN_LAUNCH), SW_SHOW);
}

static std::wstring ExtractJsonStringValue(const std::string& content, const std::string& key)
{
	size_t p = content.find("\"" + key + "\"");
	if (p == std::string::npos) return L"";
	size_t colon = content.find(':', p + key.size() + 2);
	if (colon == std::string::npos) return L"";
	size_t q1 = content.find('"', colon + 1);
	if (q1 == std::string::npos) return L"";
	size_t q2 = content.find('"', q1 + 1);
	if (q2 == std::string::npos) return L"";
	return Utf8ToWide(content.substr(q1 + 1, q2 - q1 - 1));
}

static std::wstring ReadClasspathTxt(const std::wstring& txtPath)
{
	std::ifstream ifs(fs::path(txtPath), std::ios::binary);
	if (!ifs) return L"";
	std::stringstream ss; ss << ifs.rdbuf();
	std::string content = ss.str();
	std::wstring wide = Utf8ToWide(content);
	
	std::wstring cp;
	std::set<std::wstring> seen;
	size_t pos = 0;
	while (pos <= wide.size())
	{
		size_t nl = wide.find_first_of(L"\r\n", pos);
		std::wstring line = wide.substr(pos, (nl == std::wstring::npos ? wide.size() : nl) - pos);
		pos = (nl == std::wstring::npos) ? wide.size() + 1 : nl + 1;
		
		while (!line.empty() && (line.front() == L' ' || line.front() == L'\t')) line.erase(line.begin());
		while (!line.empty() && (line.back()  == L' ' || line.back()  == L'\t')) line.pop_back();
		if (line.empty()) continue;
		std::wstring norm = NormalizeWinPath(line);
		if (!FileExistsW(norm)) continue;
		if (seen.count(norm)) continue;
		seen.insert(norm);
		if (!cp.empty()) cp += L";";
		cp += norm;
	}
	return cp;
}

static void CollectArtifactPathsFromJson(const std::wstring& jsonAbs,
										 std::vector<std::wstring>& outRelPaths)
{
	if (!FileExistsW(jsonAbs)) return;
	std::ifstream ifs(fs::path(jsonAbs), std::ios::binary);
	if (!ifs) return;
	std::stringstream ss; ss << ifs.rdbuf();
	std::string content = ss.str();
	
	size_t pos = 0;
	while ((pos = content.find("\"artifact\"", pos)) != std::string::npos)
	{
		size_t braceStart = content.find('{', pos);
		if (braceStart == std::string::npos) break;
		
		int depth = 0; size_t braceEnd = std::string::npos;
		for (size_t i = braceStart; i < content.size(); ++i)
		{
			if (content[i] == '{') depth++;
			else if (content[i] == '}')
			{
				depth--;
				if (depth == 0) { braceEnd = i; break; }
			}
		}
		if (braceEnd == std::string::npos) break;
		std::string seg = content.substr(braceStart, braceEnd - braceStart + 1);
		
		size_t pPos = seg.find("\"path\"");
		if (pPos != std::string::npos)
		{
			size_t colon = seg.find(':', pPos + 6);
			if (colon != std::string::npos)
			{
				size_t q1 = seg.find('"', colon + 1);
				size_t q2 = (q1 != std::string::npos) ? seg.find('"', q1 + 1) : std::string::npos;
				if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1)
				{
					std::string rel = seg.substr(q1 + 1, q2 - q1 - 1);
					if (rel.size() > 4 &&
						rel.compare(rel.size() - 4, 4, ".jar") == 0)
					{
						outRelPaths.push_back(Utf8ToWide(rel));
					}
				}
			}
		}
		pos = braceEnd;
	}
}

static std::wstring BuildClasspath(const std::wstring& gameDir,
								   const std::wstring& actualVer,
								   bool strictForge)
{
	std::wstring exeDir = GetExeDir();
	std::wstring tempDir = gameDir + L"\\temp";
	EnsureDirectoryRecursiveW(tempDir);
	
	std::wstring baseMcVer = actualVer;
	size_t fPos = baseMcVer.find(L"-forge");
	if (fPos != std::wstring::npos) baseMcVer = baseMcVer.substr(0, fPos);
	size_t fbPos = baseMcVer.find(L"-fabric");
	if (fbPos != std::wstring::npos) baseMcVer = baseMcVer.substr(0, fbPos);
	
	bool isForgeVer  = (actualVer.find(L"-forge")  != std::wstring::npos);
	bool isFabricVer = (actualVer.find(L"-fabric") != std::wstring::npos);
	
	if (isForgeVer || isFabricVer)
	{
		std::wstring jsAbs = exeDir + L"\\Node(YouMustNeed)v13\\build_cp.js";
		std::wstring cpTxt = tempDir + L"\\cp.txt";
		std::wstring mainTxt = tempDir + L"\\launch_main.txt";
		
		if (!FileExistsW(jsAbs))
		{
			AppendLog(L"[ERROR] [CP] 找不到 build_cp.js: " + jsAbs);
			return L"";
		}
		
		DeleteFileW(cpTxt.c_str());
		DeleteFileW(mainTxt.c_str());
		
		std::vector<std::wstring> args;
		args.push_back(jsAbs);
		args.push_back(gameDir);
		args.push_back(actualVer);
		args.push_back(cpTxt);
		if (isForgeVer)  args.push_back(L"--forge");
		if (isFabricVer) args.push_back(L"--fabric");
		
		AppendLog(L"[CP] 调用 build_cp.js: ver=" + actualVer);
		if (!RunNodeScript(exeDir, L"Node(YouMustNeed)v13\\node.exe", args, exeDir))
		{
			AppendLog(L"[ERROR] [CP] build_cp.js 执行失败");
			return L"";
		}
		
		if (!FileExistsW(cpTxt))
		{
			AppendLog(L"[ERROR] [CP] build_cp.js 未生成 cp.txt: " + cpTxt);
			return L"";
		}
		
		std::wstring cp = ReadClasspathTxt(cpTxt);
		if (cp.empty())
		{
			AppendLog(L"[ERROR] [CP] cp.txt 为空或无有效 jar");
			return L"";
		}
		
		int cpCount = 1;
		for (wchar_t c : cp) if (c == L';') cpCount++;
		AppendLog(L"[CP] 从 cp.txt 读取成功，条目数: " + std::to_wstring(cpCount));
		return cp;
	}
	
	std::wstring cp;
	std::set<std::wstring> seen;
	auto appendJar = [&](const std::wstring& jarAbs)
	{
		if (jarAbs.empty()) return;
		std::wstring norm = NormalizeWinPath(jarAbs);
		if (seen.count(norm)) return;
		if (!FileExistsW(norm)) return;
		seen.insert(norm);
		if (!cp.empty()) cp += L";";
		cp += norm;
	};
	
	std::wstring libsRoot = gameDir + L"\\libraries";
	std::wstring vanillaJson = gameDir + L"\\versions\\" + baseMcVer + L"\\" + baseMcVer + L".json";
	if (FileExistsW(vanillaJson))
	{
		std::vector<std::wstring> relPaths;
		CollectArtifactPathsFromJson(vanillaJson, relPaths);
		for (const auto& rel : relPaths)
			appendJar(NormalizeWinPath(libsRoot + L"\\" + rel));
		AppendLog(L"[CP] 原版 JSON classpath 共 " + std::to_wstring(seen.size()) + L" 个 jar");
	}
	else
	{
		AppendLog(L"[WRN] [CP] 原版 JSON 缺失，退回 libraries 扫描（仅此分支）");
		if (DirExistsW(libsRoot))
		{
			std::error_code ec;
			for (auto& p : fs::recursive_directory_iterator(fs::path(libsRoot), ec))
			{
				if (ec) break; if (!p.is_regular_file()) continue;
				if (_wcsicmp(p.path().extension().wstring().c_str(), L".jar") != 0) continue;
				appendJar(p.path().wstring());
			}
		}
	}
	appendJar(NormalizeWinPath(gameDir + L"\\versions\\" + baseMcVer + L"\\" + baseMcVer + L".jar"));
	return cp;
}

static bool LaunchMinecraft(const std::wstring& gameDir, const std::wstring& mcVer, const std::wstring& jdkVer)
{
	AppendLog(L"[LAUNCH] 进入 LaunchMinecraft");
	if (g_bIsExitRequested) { AppendLog(L"[ERROR] 启动前已请求退出"); return false; }
	
	std::wstring resolved = ResolveJdkVersion(jdkVer);
	bool is32 = Is32BitSystem();
	if (is32 && (resolved == L"21" || resolved == L"25")) resolved = L"17";
	
	std::wstring javaExe;
	std::vector<std::wstring> order;
	order.push_back(resolved);
	const wchar_t* rest[] = { L"17", L"21", L"11", L"8", L"25" };
	for (auto r : rest) if (_wcsicmp(r, resolved.c_str()) != 0) order.push_back(r);
	for (auto& v : order) {
		if (is32 && (v == L"21" || v == L"25")) continue;
		std::wstring dir = GetJavaVersionDir(v);
		std::wstring exe = dir + L"\\bin\\java.exe";
		if (FileExistsW(exe)) { javaExe = exe; break; }
		std::wstring found = FindJavaExeRecursive(dir);
		if (!found.empty()) { javaExe = found; break; }
	}
	if (javaExe.empty()) { AppendLog(L"[ERROR] 找不到 Java 路径"); return false; }
	AppendLog(L"[LAUNCH] Java: " + javaExe);
	
	bool isModded = (mcVer.find(L"-forge") != std::wstring::npos || mcVer.find(L"-fabric") != std::wstring::npos);
	std::wstring actualVer = mcVer;
	std::wstring verDir = gameDir + L"\\versions\\" + actualVer;
	
	if (!isModded) {
		std::wstring versionsRoot = gameDir + L"\\versions";
		std::error_code ec;
		if (DirExistsW(versionsRoot)) {
			for (auto& p : fs::directory_iterator(fs::path(versionsRoot), ec)) {
				if (ec) break;
				if (!p.is_directory()) continue;
				std::wstring folderName = p.path().filename().wstring();
				if (folderName.find(mcVer + L"-forge") == 0 || folderName.find(mcVer + L"-fabric") == 0) {
					actualVer = folderName;
					verDir = p.path().wstring();
					isModded = true;
					break;
				}
			}
		}
	}
	AppendLog(L"[LAUNCH] 实际版本号: " + actualVer);
	
	bool isForge = (actualVer.find(L"-forge") != std::wstring::npos);
	std::wstring classpath = BuildClasspath(gameDir, actualVer, isForge);
	if (classpath.empty()) { AppendLog(L"[ERROR] classpath 为空"); return false; }
	
	std::wstring jsArgsAbs = GetExeDir() + L"\\Node(YouMustNeed)v13\\build_args.js";
	std::wstring argsTxtPath = gameDir + L"\\args.txt";
	
	std::vector<std::wstring> args;
	args.push_back(jsArgsAbs);
	args.push_back(gameDir);
	args.push_back(actualVer);
	args.push_back(L"Player");
	
	if (!RunNodeScript(GetExeDir(), L"Node(YouMustNeed)v13\\node.exe", args, GetExeDir())) {
		AppendLog(L"[ERROR] build_args.js 执行失败");
		return false;
	}
	
	std::ifstream ifs(fs::path(argsTxtPath), std::ios::binary);
	std::stringstream ss; ss << ifs.rdbuf();
	std::string jsContent = ss.str();
	ifs.close();
	
	// 5. C++ 强行接管：内存、Classpath、主类
	std::string finalTxt;
	
	// 1. 内存参数
	std::string xmxStr = WideToUtf8(GetXmxArg());
	for (char c : xmxStr) {
		if (c == ' ') finalTxt += "\r\n";
		else finalTxt += c;
	}
	finalTxt += "\r\n";
	
	// 2. Classpath
	finalTxt += "-cp\r\n";
	finalTxt += WideToUtf8(classpath) + "\r\n";
	
	// 3. 主类：直接读 JS 写的文件，不要自己瞎猜！
	std::string mainCls = "net.minecraft.client.main.Main";
	std::wstring mainTxtPath = gameDir + L"\\temp\\launch_main.txt";
	if (FileExistsW(mainTxtPath)) {
		std::string mc = ReadWholeFileA(mainTxtPath);
		std::wstring wMc = Utf8ToWide(mc);
		while (!wMc.empty() && (wMc.back() == L'\r' || wMc.back() == L'\n' || wMc.back() == L' ')) wMc.pop_back();
		if (!wMc.empty()) mainCls = WideToUtf8(wMc);
	}
	finalTxt += mainCls + "\r\n";
	
	// 4. 接上 JS 吐出的纯游戏参数
	finalTxt += jsContent;
	
	// 5. 写入文件
	FILE* fp = _wfopen(argsTxtPath.c_str(), L"wb");
	if (fp) {
		fwrite(finalTxt.c_str(), 1, finalTxt.size(), fp);
		fclose(fp);
	}
	
	AppendLog(L"[LAUNCH] 内存、CP、主类由 C++ 注入完成，游戏参数由 JS 注入完成");
	
	std::wstring cmdLine = L"\"" + javaExe + L"\" @\"" + argsTxtPath + L"\"";
	
	AppendLog(L"[LAUNCH] 命令行: " + cmdLine);
	
	STARTUPINFOW si{}; si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
	cmdBuffer.push_back(L'\0');
	
	BOOL ok = CreateProcessW(
							 nullptr, &cmdBuffer[0],
							 nullptr, nullptr, FALSE,
							 CREATE_NO_WINDOW, nullptr,
							 gameDir.c_str(),
							 &si, &pi);
	
	if (!ok) {
		AppendLog(L"[ERROR] CreateProcessW 失败，GetLastError() = " + std::to_wstring(GetLastError()));
		return false;
	}
	
	AppendLog(L"[LAUNCH] 已拉起 Java 进程 PID=" + std::to_wstring(pi.dwProcessId));
	
	DWORD waitRes = WaitForSingleObject(pi.hProcess, 3000);
	if (waitRes == WAIT_OBJECT_0) {
		DWORD exitCode = 0;
		GetExitCodeProcess(pi.hProcess, &exitCode);
		AppendLog(L"[ERROR] Java 进程瞬间退出，exitCode = " + std::to_wstring(exitCode));
		CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
		return false;
	}
	
	AppendLog(L"[LAUNCH] 3000ms 后进程仍在运行（正常）");
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	return true;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
		g_hMainWnd = hWnd; CreateControls(hWnd);
		AppendLog(L"[VER] 获取版本列表...");
		{
			std::wstring manifestPath = GetExeDir() + L"\\version_manifest_v2.json";
			HWND hCombo = g_hComboVer;
			std::thread([hCombo, manifestPath]() { FetchMinecraftVersionsThread(hCombo, manifestPath); }).detach();
		}
		return 0;
		case WM_APP_LOG: {
			std::wstring* p = (std::wstring*)lParam;
			if (p) { AppendLogDirect(*p); delete p; }
			return 0;
		}
		case WM_APP_FILL_VERS: {
			HWND hCombo = (HWND)wParam;
			VersionFetchResult* r = (VersionFetchResult*)lParam;
			OnFillVersions(hCombo, r);
			return 0;
		}
	case WM_APP_SWITCH_HOME:
		if (g_hTab) {
			TabCtrl_SetCurSel(g_hTab, 0);
			ShowWindow(GetDlgItem(hWnd, ID_EDIT_LOG), SW_SHOW);
			ShowWindow(GetDlgItem(hWnd, ID_COMBO_VER), SW_HIDE);
			ShowWindow(GetDlgItem(hWnd, ID_COMBO_LOADER), SW_HIDE);
			ShowWindow(GetDlgItem(hWnd, ID_COMBO_JAVA), SW_HIDE);
			ShowWindow(GetDlgItem(hWnd, ID_BTN_DOWN), SW_HIDE);
			ShowWindow(GetDlgItem(hWnd, ID_BTN_CHECK), SW_HIDE);
			ShowWindow(GetDlgItem(hWnd, ID_BTN_LAUNCH), SW_SHOW);
		}
		return 0;
		case WM_NOTIFY: {
			LPNMHDR nm = (LPNMHDR)lParam;
			if (nm && nm->code == TCN_SELCHANGE && nm->idFrom == ID_TAB_MAIN) {
				int sel = TabCtrl_GetCurSel(g_hTab);
				ShowWindow(GetDlgItem(hWnd, ID_EDIT_LOG), sel == 0 ? SW_SHOW : SW_HIDE);
				ShowWindow(GetDlgItem(hWnd, ID_COMBO_VER), sel == 1 ? SW_SHOW : SW_HIDE);
				ShowWindow(GetDlgItem(hWnd, ID_COMBO_LOADER), sel == 1 ? SW_SHOW : SW_HIDE);
				ShowWindow(GetDlgItem(hWnd, ID_COMBO_JAVA), sel == 1 ? SW_SHOW : SW_HIDE);
				ShowWindow(GetDlgItem(hWnd, ID_BTN_DOWN), sel == 1 ? SW_SHOW : SW_HIDE);
				ShowWindow(GetDlgItem(hWnd, ID_BTN_CHECK), sel == 1 ? SW_SHOW : SW_HIDE);
				ShowWindow(GetDlgItem(hWnd, ID_BTN_LAUNCH), sel == 0 ? SW_SHOW : SW_HIDE);
			}
			return 0;
		}
		case WM_COMMAND: {
			int id = LOWORD(wParam);
			if (id == ID_BTN_DOWN) {
				std::wstring mcVer = GetSelectedMcVersion();
				std::wstring loader = GetSelectedLoader();
				std::wstring jdkVer = GetSelectedJavaVersion();
				std::wstring exeDir = GetExeDir();
				std::thread([=]() {
					AppendLog(L"[JOB] 流水线开始");
					if (g_bIsExitRequested) return;
					
					if (!DownloadJavaForHost(jdkVer)) { AppendLog(L"[JOB] Java 失败"); return; }
					if (g_bIsExitRequested) return;
					
					if (!ParseVanillaWithNode(exeDir, mcVer)) { AppendLog(L"[JOB] 原版基础失败，流水线终止"); return; }
					if (g_bIsExitRequested) return;
					
					if (loader == L"Fabric") {
						if (!ParseFabricWithNode(exeDir, mcVer)) { AppendLog(L"[JOB] Fabric 失败"); return; }
					}
					else if (loader == L"Forge") {
						if (!ParseAndInstallForgeWithNode(exeDir, mcVer, jdkVer)) { AppendLog(L"[JOB] Forge 失败"); return; }
					}
					else if (loader == L"Vanilla") {
						AppendLog(L"[JOB] 仅原版，流水线结束");
					}
					else {
						AppendLog(L"[ERROR] 未知加载器");
						return;
					}
					
					if (g_bIsExitRequested) return;
					AppendLog(L"[JOB] 流水线全部完成");
					if (!g_bIsExitRequested && g_hMainWnd) PostMessageW(g_hMainWnd, WM_APP_SWITCH_HOME, 0, 0);
				}).detach();
			}
			else if (id == ID_BTN_CHECK) {
				std::wstring mcVer = GetSelectedMcVersion();
				std::wstring gameDir = GetGameDir();
				std::thread([=]() {
					RunFullCheck(gameDir, mcVer);
				}).detach();
			}
			else if (id == ID_BTN_LAUNCH) {
				AppendLog(L"[LAUNCH] 点击启动游戏");
				std::vector<std::wstring> lv = GetLocalVersions();
				if (lv.empty()) { MessageBoxW(hWnd, L"本地无已下载版本", L"提示", MB_OK); return 0; }
				AppendLog(L"[LAUNCH] 弹出选择版本对话框...");
				std::wstring chosen;
				bool ok = ShowLocalVersionDialog(hWnd, lv, chosen);
				AppendLog(L"[LAUNCH] 对话框返回: " + std::wstring(ok ? L"true" : L"false") + L"，选择: " + chosen);
				if (!ok || chosen.empty()) return 0;
				std::wstring gameDir = GetGameDir();
				std::wstring jdkVer = GetSelectedJavaVersion();
				std::thread([=]() {
					AppendLog(L"[LAUNCH] 子线程开始");
					bool launched = LaunchMinecraft(gameDir, chosen, jdkVer);
					AppendLog(L"[LAUNCH] LaunchMinecraft 返回: " + std::wstring(launched ? L"true" : L"false"));
				}).detach();
			}
			return 0;
		}
		case WM_ERASEBKGND: return 1;
	case WM_CLOSE:
		g_bIsExitRequested = true; 
		KillChildProcesses(); 
		CleanupTempFiles();
		DestroyWindow(hWnd); 
		return 0;
	case WM_DESTROY:
		g_bIsExitRequested = true; 
		KillChildProcesses(); 
		CleanupTempFiles();
		PostQuitMessage(0); 
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
	g_hInst = hInstance;
	GdiplusStartupInput gsi;
	if (GdiplusStartup(&g_gdiplusToken, &gsi, nullptr) != Ok) return 0;
	INITCOMMONCONTROLSEX icc{};
	icc.dwSize = sizeof(icc); icc.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES; InitCommonControlsEx(&icc);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"UMCL_MAIN";
	RegisterClassExW(&wc);
	HWND hWnd = CreateWindowExW(0, L"UMCL_MAIN", L"UMCL", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 600, nullptr, nullptr, hInstance, nullptr);
	if (!hWnd) { GdiplusShutdown(g_gdiplusToken); return 0; }
	ShowWindow(hWnd, nCmdShow); UpdateWindow(hWnd);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
	GdiplusShutdown(g_gdiplusToken);
	return (int)msg.wParam;
}
/*永久感谢：
Bang bang 93-提供mc下载
gh-proxy-提供JAVA加速服务
Adoptium-提供JAVA下载
Brecht Sanders-提供比什么东西都好用的编译器
Microsoft-提供Win32API（让我写鼠标指针控制的时候不再让我的手指无法形容）
感谢以上所有人
Thanks everyone
（我是小学生捐不了款，再次表示致谢）
*/
