#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>

// colors
#define R  "\x1b[31m" // red
#define G  "\x1b[32m" // green
#define Y  "\x1b[33m" // yellow
#define B  "\x1b[34m" // blue
#define M  "\x1b[35m" // magenta
#define C  "\x1b[36m" // cyan
#define RST "\x1b[0m"  // reset

// priv add
BOOL SetPriv(LPCSTR priv) {
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return FALSE;
    if (!LookupPrivilegeValueA(NULL, priv, &luid)) { CloseHandle(hToken); return FALSE; }
    
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    
    BOOL res = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
    return res;
}

// adding all privs in token
void EnableAllPrivs(HANDLE hToken) {
    DWORD sz;
    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &sz);
    PTOKEN_PRIVILEGES pPrivs = (PTOKEN_PRIVILEGES)malloc(sz);
    if (GetTokenInformation(hToken, TokenPrivileges, pPrivs, sz, &sz)) {
        for (DWORD i = 0; i < pPrivs->PrivilegeCount; i++) {
            pPrivs->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
        }
        AdjustTokenPrivileges(hToken, FALSE, pPrivs, sz, NULL, NULL);
    }
    free(pPrivs);
}

// search PID by name
DWORD GetPid(const char* name) {
    DWORD pid = 0;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { sizeof(pe) };
    if (Process32First(h, &pe)) {
        do {
            if (stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(h, &pe));
    }
    CloseHandle(h);
    return pid;
}

void PrintHelp() {
	printf(C "\n=== Super System Shell ===" RST "\n\n");
	printf(Y "Usage:" RST " sss.exe [flag]\n");
	printf("  " C "-beSystem" RST "        Launch CMD as " B "SYSTEM" RST "\n");
	printf("  " C "-beSuperSystem" RST "   Launch CMD as " M "TrustedInstaller" RST " + All Privileges\n");
	printf("  " C "-h" RST "               Show help\n\n");
}

void SetupConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

int main(int argc, char* argv[]) {

    SetupConsole();

    if (argc < 2 || strcmp(argv[1], "-h") == 0) {
        PrintHelp();
        return 0;
    }

    int mode = 0; 
    if (strcmp(argv[1], "-beSystem") == 0) mode = 1;
    else if (strcmp(argv[1], "-beSuperSystem") == 0) mode = 2;

    if (mode == 0) {
        printf(R "\n[!] Unknown flag: %s" RST "\n", argv[1]);
        PrintHelp();
        return 1;
    }

    if (!SetPriv(SE_DEBUG_NAME) || !SetPriv(SE_IMPERSONATE_NAME)) {
        printf(R "[!] Failed to set privileges. Run as Administrator!" RST "\n");
        return 1;
    }

    // getting SYSTEM with winlogon
    DWORD winPid = GetPid("winlogon.exe");
    HANDLE hWinProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winPid);
    HANDLE hWinToken, hSysTokenImp;
    
    if (!hWinProc || !OpenProcessToken(hWinProc, TOKEN_DUPLICATE, &hWinToken)) {
        printf(R "[!] Failed to get winlogon token. Error: %lu" RST "\n", GetLastError());
        return 1;
    }

    // creating Impersonation Token for current thread
    DuplicateTokenEx(hWinToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &hSysTokenImp);
    if (!SetThreadToken(NULL, hSysTokenImp)) {
        printf("[!] SetThreadToken (SYSTEM) failed: %lu\n", GetLastError());
        return 1;
    }

    HANDLE hFinalToken = NULL;

    // 2. logic of switching modes
    if (mode == 2) {
        // searching TrustedInstaller
        DWORD tiPid = GetPid("TrustedInstaller.exe");
        if (tiPid == 0) {
            system("sc start TrustedInstaller > nul");
            Sleep(1000);
            tiPid = GetPid("TrustedInstaller.exe");
        }
        
        HANDLE hTiProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, tiPid);
        HANDLE hTiToken;
        if (hTiProc && OpenProcessToken(hTiProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTiToken)) {
            // Создаем Primary Token для запуска процесса
            DuplicateTokenEx(hTiToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hFinalToken);
            EnableAllPrivs(hFinalToken);
            printf(B "[*]" RST " Mode: " M "SuperSystem (TrustedInstaller) + all privileges." RST "\n");
            CloseHandle(hTiToken);
            CloseHandle(hTiProc);
        } else {
            printf(R "[!] Failed to get TI token: %lu" RST "\n", GetLastError());
            return 1;
        }
    } else {
        DuplicateTokenEx(hWinToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hFinalToken);
        printf(B "[*]" RST " Mode: " B "System" RST "\n");
    }

    // 3. launching CMD
    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = { 0 };
    if (CreateProcessWithTokenW(hFinalToken, 0, L"C:\\Windows\\System32\\cmd.exe", NULL, 0, NULL, NULL, &si, &pi)) {
        printf(G "[+]" RST " Success! PID: " Y "%lu" RST "\n", pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        printf(R "[!] CreateProcessWithTokenW failed: %lu" RST "\n", GetLastError());
    }

    // cleanup
    CloseHandle(hWinToken);
    CloseHandle(hWinProc);
    CloseHandle(hSysTokenImp);
    if (hFinalToken) CloseHandle(hFinalToken);

    return 0;
}
