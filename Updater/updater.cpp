#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>
#include <shellapi.h>
#include <string>
#include <fstream>
#include <regex>
#include <iostream>
#include <vector>

#pragma comment(lib, "urlmon.lib")

// ==========================================
// 1. 宽字符转窄字符（解决 MinGW 的 ifstream 报错）
// ==========================================
std::string WStringToString(const std::wstring& wstr) {
	if (wstr.empty()) return std::string();
	int size_needed = WideCharToMultiByte(CP_ACP, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
	std::string strTo(size_needed, 0);
	WideCharToMultiByte(CP_ACP, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
	return strTo;
}

// ==========================================
// 2. UTF-8 转宽字符
// ==========================================
std::wstring Utf8ToWide(const std::string& str) {
	if (str.empty()) return std::wstring();
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
	std::wstring wstrTo(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
	return wstrTo;
}

// ==========================================
// 3. 获取当前程序所在绝对路径
// ==========================================
std::wstring g_exeDir;
void InitExeDir() {
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
	if (n > 0) {
		std::wstring full(buf, n);
		size_t pos = full.find_last_of(L"\\/");
		if (pos != std::wstring::npos) {
			g_exeDir = full.substr(0, pos);
		}
	}
}

// ==========================================
// 4. 终极正版公式：0x04C11DB7 (纯原生位运算，不使用任何查找表)
// ==========================================
unsigned int calculate_crc32_raw(const std::wstring& filepath) {
	// 使用纯粹的 Windows 底层文件读取，绝不掺杂任何 C++ 流的自动转换
	HANDLE hFile = CreateFileW(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return 0;
	
	unsigned int crc = 0xFFFFFFFF; // 初始值
	unsigned char buffer[65536];
	DWORD bytesRead = 0;
	
	while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
		for (DWORD i = 0; i < bytesRead; i++) {
			// 🚨 核心公式：正向多项式
			// 1. 把当前字节放到 32 位整数的最高 8 位 (左移 24 位)
			crc ^= (unsigned int)buffer[i] << 24;
			
			// 2. 纯位运算暴力推演 8 次
			for (int j = 0; j < 8; j++) {
				if (crc & 0x80000000) { // 判断最高位是不是 1
					crc = (crc << 1) ^ 0x04C11DB7; // 是 1 就异或正版多项式
				} else {
					crc = crc << 1; // 是 0 就继续左移
				}
			}
		}
	}
	
	CloseHandle(hFile);
	return crc ^ 0xFFFFFFFF; // 最终异或取反
}

// ==========================================
// 5. 极简 JSON 解析
// ==========================================
std::string getJsonValue(const std::string& jsonStr, const std::string& key) {
	std::regex reg("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"");
	std::smatch match;
	if (std::regex_search(jsonStr, match, reg)) return match[1].str();
	return "";
}

// ==========================================
// 6. 无头杀手
// ==========================================
void KillProcesses() {
	std::wstring cmdLine = L"cmd.exe /c taskkill /F /IM UMCL.exe /IM node.exe /IM powershell.exe >nul 2>&1";
	STARTUPINFOW si = { sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = { 0 };
	
	if (CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, 1000); 
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
}

// ==========================================
// 7. 主函数
// ==========================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
	InitExeDir();
	
	std::string jsonUrl = "https://upumcl.pages.dev/latest.json";
	std::wstring localJson = g_exeDir + L"\\temp_latest.json";
	
	HRESULT hr = URLDownloadToFileW(NULL, Utf8ToWide(jsonUrl).c_str(), localJson.c_str(), 0, NULL);
	if (FAILED(hr)) {
		MessageBoxW(NULL, L"无法连接到更新服务器，请检查网络。", L"更新失败", MB_ICONERROR);
		return 1;
	}
	
	std::ifstream ifs(WStringToString(localJson));
	std::string jsonStr((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
	ifs.close();
	DeleteFileW(localJson.c_str());
	
	std::string channel = getJsonValue(jsonStr, "channel");
	std::string patchUrl = getJsonValue(jsonStr, "patch_url");
	std::string patchCrcStr = getJsonValue(jsonStr, "patch_crc32");
	std::string patchLocalPath = getJsonValue(jsonStr, "patch");
	
	if (channel == "disgood") {
		std::string fileName = "build_cp.js"; 
		size_t lastSlash = patchUrl.find_last_of('/');
		if (lastSlash != std::string::npos) {
			fileName = patchUrl.substr(lastSlash + 1);
		}
		
		std::wstring tempPatchFile = g_exeDir + L"\\temp_patch_file";
		HRESULT hrPatch = URLDownloadToFileW(NULL, Utf8ToWide(patchUrl).c_str(), tempPatchFile.c_str(), 0, NULL);
		if (FAILED(hrPatch)) {
			MessageBoxW(NULL, L"补丁文件下载失败！", L"更新失败", MB_ICONERROR);
			return 1;
		}
		
		// 这里用正版多项式算出的结果
		unsigned int actualCrc = calculate_crc32_raw(tempPatchFile);
		char actualCrcStr[9];
		sprintf_s(actualCrcStr, "%08X", actualCrc);
		
		if (_stricmp(patchCrcStr.c_str(), actualCrcStr) != 0) {
			std::wstring errMsg = L"补丁文件损坏或哈希不匹配！\n\n";
			errMsg += L"预期值: " + Utf8ToWide(patchCrcStr) + L"\n";
			errMsg += L"实际值: " + Utf8ToWide(actualCrcStr);
			MessageBoxW(NULL, errMsg.c_str(), L"安全拦截", MB_ICONWARNING);
			DeleteFileW(tempPatchFile.c_str());
			return 1;
		}
		
		KillProcesses();
		
		std::wstring wPatchLocalPath = Utf8ToWide(patchLocalPath);
		if (wPatchLocalPath.find(L"./") == 0) wPatchLocalPath = wPatchLocalPath.substr(2);
		for (auto& c : wPatchLocalPath) if (c == L'/') c = L'\\';
		
		std::wstring targetPath = g_exeDir + L"\\" + wPatchLocalPath + L"\\" + Utf8ToWide(fileName);
		
		if (MoveFileExW(tempPatchFile.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			MessageBoxW(NULL, L"热修复补丁安装成功！即将启动主程序。", L"更新成功", MB_ICONINFORMATION);
			
			std::wstring mainExe = g_exeDir + L"\\UMCL.exe";
			ShellExecuteW(NULL, L"open", mainExe.c_str(), NULL, g_exeDir.c_str(), SW_SHOWNORMAL);
			return 0;
		} else {
			MessageBoxW(NULL, L"覆盖文件失败，可能是因为权限不足或文件被占用。", L"错误", MB_ICONERROR);
			return 1;
		}
	}
	
	return 0;
}
