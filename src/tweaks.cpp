// XYZ Optimizer - motor de otimizacoes
// Cada tweak guarda o valor original antes de mudar, para poder reverter.

#include "tweaks.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace xyz {
namespace {

// ---------------------------------------------------------------- strings
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring WinDir() {
    wchar_t buf[MAX_PATH];
    UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring(L"C:\\Windows");
    return std::wstring(buf, n);
}

std::wstring Sys32(const wchar_t* rel) { return WinDir() + L"\\System32\\" + rel; }

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring FormatBytes(unsigned long long b) {
    unsigned long long mb = b / (1024ULL * 1024ULL);
    return std::to_wstring(mb) + L" MB";
}

}  // namespace

std::wstring DataDir() {
    std::wstring base = Env(L"APPDATA");
    if (base.empty()) base = WinDir();
    std::wstring dir = base + L"\\XYZOptimizer";
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

namespace {

// ---------------------------------------------------------------- processos
bool RunCmd(const std::wstring& cmdline, DWORD* exitCode = nullptr, DWORD timeoutMs = 180000) {
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        if (exitCode) *exitCode = static_cast<DWORD>(-1);
        return false;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 0;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        code = static_cast<DWORD>(-2);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return wait != WAIT_TIMEOUT;
}

bool RunSys(const wchar_t* exe, const std::wstring& args, DWORD* code = nullptr, DWORD timeoutMs = 180000) {
    return RunCmd(Quote(Sys32(exe)) + L" " + args, code, timeoutMs);
}

// O script nao pode conter aspas duplas (use aspas simples).
bool RunPowerShell(const std::wstring& script, DWORD timeoutMs = 600000) {
    DWORD code = 1;
    std::wstring args = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + L"\"";
    bool ran = RunSys(L"WindowsPowerShell\\v1.0\\powershell.exe", args, &code, timeoutMs);
    return ran && code == 0;
}

bool IsProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------- backup
std::mutex g_backupMx;
std::map<std::string, std::string> g_backup;
bool g_backupLoaded = false;

std::wstring BackupFile() { return DataDir() + L"\\backup.txt"; }

void LoadBackupLocked() {
    if (g_backupLoaded) return;
    g_backupLoaded = true;
    const fs::path file = fs::path(BackupFile());
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        g_backup[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

void SaveBackupLocked() {
    const fs::path file = fs::path(BackupFile());
    std::ofstream out(file, std::ios::trunc);
    for (const auto& kv : g_backup) out << kv.first << '\t' << kv.second << '\n';
}

// ---------------------------------------------------------------- registro
enum class Root { LM, CU };
enum class VType { Dword, Str };

struct RegEntry {
    Root root;
    const wchar_t* sub;
    const wchar_t* name;
    VType type;
    DWORD dw;
    const wchar_t* str;
};

RegEntry D(Root r, const wchar_t* sub, const wchar_t* name, DWORD v) {
    return RegEntry{r, sub, name, VType::Dword, v, nullptr};
}
RegEntry S(Root r, const wchar_t* sub, const wchar_t* name, const wchar_t* v) {
    return RegEntry{r, sub, name, VType::Str, 0, v};
}

HKEY RootKey(Root r) { return r == Root::LM ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

std::string RegKeyId(const RegEntry& e) {
    return std::string("reg|") + (e.root == Root::LM ? "LM" : "CU") + "|" + ToUtf8(e.sub) + "|" + ToUtf8(e.name);
}

void BackupOriginal(const RegEntry& e) {
    std::lock_guard<std::mutex> lk(g_backupMx);
    LoadBackupLocked();
    std::string id = RegKeyId(e);
    if (g_backup.count(id) != 0) return;  // ja temos o valor original

    wchar_t data[1024];
    DWORD size = sizeof(data);
    DWORD type = 0;
    LONG rc = RegGetValueW(RootKey(e.root), e.sub, e.name, RRF_RT_REG_DWORD | RRF_RT_REG_SZ, &type, data, &size);
    if (rc == ERROR_SUCCESS) {
        if (type == REG_DWORD) {
            DWORD v = 0;
            std::memcpy(&v, data, sizeof(v));
            g_backup[id] = "D|" + std::to_string(static_cast<unsigned long>(v));
        } else {
            g_backup[id] = "S|" + ToUtf8(std::wstring(data));
        }
    } else {
        g_backup[id] = "N|";  // o valor nao existia
    }
    SaveBackupLocked();
}

bool ApplyEntry(const RegEntry& e) {
    BackupOriginal(e);
    LONG rc;
    if (e.type == VType::Dword) {
        rc = RegSetKeyValueW(RootKey(e.root), e.sub, e.name, REG_DWORD, &e.dw, sizeof(DWORD));
    } else {
        DWORD bytes = static_cast<DWORD>((std::wcslen(e.str) + 1) * sizeof(wchar_t));
        rc = RegSetKeyValueW(RootKey(e.root), e.sub, e.name, REG_SZ, e.str, bytes);
    }
    return rc == ERROR_SUCCESS;
}

bool RevertEntry(const RegEntry& e) {
    std::string val;
    {
        std::lock_guard<std::mutex> lk(g_backupMx);
        LoadBackupLocked();
        auto it = g_backup.find(RegKeyId(e));
        if (it == g_backup.end()) return true;  // nada para restaurar
        val = it->second;
        g_backup.erase(it);
        SaveBackupLocked();
    }
    LONG rc = ERROR_SUCCESS;
    if (val.compare(0, 2, "D|") == 0) {
        DWORD v = static_cast<DWORD>(std::strtoul(val.c_str() + 2, nullptr, 10));
        rc = RegSetKeyValueW(RootKey(e.root), e.sub, e.name, REG_DWORD, &v, sizeof(v));
    } else if (val.compare(0, 2, "S|") == 0) {
        std::wstring s = FromUtf8(val.substr(2));
        DWORD bytes = static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t));
        rc = RegSetKeyValueW(RootKey(e.root), e.sub, e.name, REG_SZ, s.c_str(), bytes);
    } else {
        rc = RegDeleteKeyValueW(RootKey(e.root), e.sub, e.name);
        if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    return rc == ERROR_SUCCESS;
}

bool ApplyRegs(const std::vector<RegEntry>& entries, const LogFn& log) {
    bool ok = true;
    for (const auto& e : entries) {
        if (!ApplyEntry(e)) {
            log(L"  falhou no registro: " + std::wstring(e.name));
            ok = false;
        }
    }
    return ok;
}

bool RevertRegs(const std::vector<RegEntry>& entries, const LogFn& log) {
    bool ok = true;
    for (const auto& e : entries) {
        if (!RevertEntry(e)) {
            log(L"  falhou ao restaurar: " + std::wstring(e.name));
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------- servicos e tarefas
bool ServiceDisable(const wchar_t* name, const LogFn& log) {
    std::wstring n(name);
    DWORD code = 0;
    if (!RunSys(L"sc.exe", L"config " + n + L" start= disabled", &code, 30000)) return false;
    if (code == 1060) {
        log(L"  servico " + n + L" nao existe neste Windows, ignorado.");
        return true;
    }
    RunSys(L"sc.exe", L"stop " + n, nullptr, 30000);
    return code == 0;
}

bool ServiceRestore(const wchar_t* name, const wchar_t* startType) {
    std::wstring n(name);
    DWORD code = 0;
    bool ran = RunSys(L"sc.exe", L"config " + n + L" start= " + startType, &code, 30000);
    return ran && (code == 0 || code == 1060);
}

void TaskSet(const wchar_t* path, bool enable) {
    std::wstring args = L"/Change /TN \"" + std::wstring(path) + L"\" " + (enable ? L"/Enable" : L"/Disable");
    RunSys(L"schtasks.exe", args, nullptr, 30000);
}

const wchar_t* const kTelemetryTasks[] = {
    L"\\Microsoft\\Windows\\Application Experience\\Microsoft Compatibility Appraiser",
    L"\\Microsoft\\Windows\\Application Experience\\ProgramDataUpdater",
    L"\\Microsoft\\Windows\\Customer Experience Improvement Program\\Consolidator",
    L"\\Microsoft\\Windows\\Customer Experience Improvement Program\\UsbCeip",
    L"\\Microsoft\\Windows\\Feedback\\Siuf\\DmClient",
};

// ---------------------------------------------------------------- mouse
bool MouseApply(const LogFn& log) {
    int cur[3] = {1, 6, 10};
    int speed = 10;
    SystemParametersInfoW(SPI_GETMOUSE, 0, cur, 0);
    SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0);
    {
        std::lock_guard<std::mutex> lk(g_backupMx);
        LoadBackupLocked();
        if (g_backup.count("mouse") == 0) {
            g_backup["mouse"] = "M|" + std::to_string(cur[0]) + "," + std::to_string(cur[1]) + "," +
                                std::to_string(cur[2]) + "," + std::to_string(speed);
            SaveBackupLocked();
        }
    }
    int off[3] = {0, 0, 0};
    BOOL a = SystemParametersInfoW(SPI_SETMOUSE, 0, off, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    BOOL b = SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(10)),
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    if (!a || !b) log(L"  o Windows recusou a mudanca do mouse.");
    return a && b;
}

bool MouseRevert(const LogFn& log) {
    std::string val;
    {
        std::lock_guard<std::mutex> lk(g_backupMx);
        LoadBackupLocked();
        auto it = g_backup.find("mouse");
        if (it == g_backup.end()) {
            log(L"  nada para restaurar.");
            return true;
        }
        val = it->second;
        g_backup.erase(it);
        SaveBackupLocked();
    }
    int v[4] = {1, 6, 10, 10};
    std::stringstream ss(val.substr(2));
    std::string tok;
    int i = 0;
    while (i < 4 && std::getline(ss, tok, ',')) v[i++] = std::atoi(tok.c_str());
    int params[3] = {v[0], v[1], v[2]};
    BOOL a = SystemParametersInfoW(SPI_SETMOUSE, 0, params, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    BOOL b = SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(v[3])),
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    return a && b;
}

// ---------------------------------------------------------------- energia
bool PowerHigh(const LogFn& log) {
    DWORD code = 1;
    bool ran = RunSys(L"powercfg.exe", L"/setactive SCHEME_MIN", &code, 30000);
    if (!ran || code != 0) {
        log(L"  nao foi possivel ativar o plano Alto Desempenho neste PC.");
        return false;
    }
    return true;
}

bool PowerBalanced(const LogFn&) {
    DWORD code = 1;
    bool ran = RunSys(L"powercfg.exe", L"/setactive SCHEME_BALANCED", &code, 30000);
    return ran && code == 0;
}

// ---------------------------------------------------------------- apps
bool RemoveAppx(const std::vector<std::wstring>& names, const LogFn& log) {
    std::wstring list;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) list += L",";
        list += L"'" + names[i] + L"'";
    }
    std::wstring ps =
        L"$n=" + list +
        L"; foreach($x in $n){ "
        L"Get-AppxPackage -AllUsers -Name $x | Remove-AppxPackage -AllUsers -ErrorAction SilentlyContinue; "
        L"Get-AppxProvisionedPackage -Online | Where-Object { $_.DisplayName -like $x } | "
        L"Remove-AppxProvisionedPackage -Online -ErrorAction SilentlyContinue | Out-Null }";
    log(L"  removendo pacotes (pode levar alguns minutos)...");
    return RunPowerShell(ps, 900000);
}

bool RemoveOneDrive(const LogFn& log) {
    RunSys(L"taskkill.exe", L"/f /im OneDrive.exe", nullptr, 30000);
    std::wstring win = WinDir();
    std::wstring candidates[2] = {win + L"\\SysWOW64\\OneDriveSetup.exe", win + L"\\System32\\OneDriveSetup.exe"};
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(fs::path(c), ec)) {
            DWORD code = 1;
            bool ran = RunCmd(Quote(c) + L" /uninstall", &code, 300000);
            return ran;
        }
    }
    log(L"  instalador do OneDrive nao encontrado (talvez ja removido).");
    return true;
}

// ---------------------------------------------------------------- limpeza
unsigned long long PathSize(const fs::path& p) {
    std::error_code ec;
    unsigned long long total = 0;
    if (fs::is_regular_file(p, ec)) {
        auto s = fs::file_size(p, ec);
        return ec ? 0ULL : static_cast<unsigned long long>(s);
    }
    ec.clear();
    if (fs::is_directory(p, ec)) {
        fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            std::error_code ec2;
            if (it->is_regular_file(ec2)) {
                auto s = it->file_size(ec2);
                if (!ec2) total += static_cast<unsigned long long>(s);
            }
            it.increment(ec);
        }
    }
    return total;
}

unsigned long long CleanDirContents(const fs::path& dir) {
    unsigned long long freed = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    ec.clear();
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        entries.push_back(it->path());
    }
    for (const auto& p : entries) {
        unsigned long long sz = PathSize(p);
        std::error_code ec2;
        fs::remove_all(p, ec2);
        if (!ec2) freed += sz;
    }
    return freed;
}

bool CleanTemp(const LogFn& log) {
    unsigned long long freed = 0;
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) freed += CleanDirContents(fs::path(std::wstring(tmp, n)));
    freed += CleanDirContents(fs::path(WinDir() + L"\\Temp"));
    log(L"  liberado: " + FormatBytes(freed));
    return true;
}

bool CleanDumps(const LogFn& log) {
    unsigned long long freed = 0;
    std::wstring pd = Env(L"PROGRAMDATA");
    std::wstring la = Env(L"LOCALAPPDATA");
    if (!pd.empty()) {
        freed += CleanDirContents(fs::path(pd + L"\\Microsoft\\Windows\\WER\\ReportArchive"));
        freed += CleanDirContents(fs::path(pd + L"\\Microsoft\\Windows\\WER\\ReportQueue"));
    }
    if (!la.empty()) freed += CleanDirContents(fs::path(la + L"\\CrashDumps"));
    freed += CleanDirContents(fs::path(WinDir() + L"\\Minidump"));
    log(L"  liberado: " + FormatBytes(freed));
    return true;
}

bool EmptyBin(const LogFn& log) {
    HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSBAR | SHERB_NOSOUND);
    if (FAILED(hr)) log(L"  a lixeira ja estava vazia ou nao pode ser esvaziada.");
    return true;
}

// ---------------------------------------------------------------- Roblox
bool SetXmlValue(std::string& xml, const std::string& name, const std::string& tag, int value) {
    std::string key = "name=\"" + name + "\">";
    std::string v = std::to_string(value);
    size_t pos = xml.find(key);
    if (pos != std::string::npos) {
        size_t start = pos + key.size();
        size_t end = xml.find('<', start);
        if (end == std::string::npos) return false;
        xml.replace(start, end - start, v);
        return true;
    }
    size_t props = xml.find("</Properties>");
    if (props == std::string::npos) return false;
    xml.insert(props, "<" + tag + " name=\"" + name + "\">" + v + "</" + tag + ">");
    return true;
}

fs::path RobloxSettingsFile() {
    return fs::path(Env(L"LOCALAPPDATA")) / L"Roblox" / L"GlobalBasicSettings_13.xml";
}

fs::path RobloxBackupFile() { return fs::path(DataDir()) / L"roblox_settings.bak"; }

bool RobloxPotatoApply(const LogFn& log) {
    fs::path file = RobloxSettingsFile();
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        log(L"  abra o Roblox uma vez para criar o arquivo de configuracoes e tente de novo.");
        return false;
    }
    if (IsProcessRunning(L"RobloxPlayerBeta.exe")) {
        log(L"  feche o Roblox e aplique de novo.");
        return false;
    }
    fs::path bak = RobloxBackupFile();
    if (!fs::exists(bak, ec)) fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);

    std::string xml;
    {
        std::ifstream in(file, std::ios::binary);
        xml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bool a = SetXmlValue(xml, "GraphicsQualityLevel", "int", 1);
    bool b = SetXmlValue(xml, "SavedQualityLevel", "token", 1);
    if (!a && !b) {
        log(L"  formato do arquivo do Roblox nao reconhecido.");
        return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    out.flush();
    if (!out.good()) return false;
    log(L"  qualidade no nivel 1. Se o Roblox estiver em modo Automatico, troque para Manual uma vez.");
    return true;
}

bool RobloxPotatoRevert(const LogFn& log) {
    fs::path bak = RobloxBackupFile();
    std::error_code ec;
    if (!fs::exists(bak, ec)) {
        log(L"  nenhum backup encontrado.");
        return true;
    }
    if (IsProcessRunning(L"RobloxPlayerBeta.exe")) {
        log(L"  feche o Roblox e reverta de novo.");
        return false;
    }
    fs::copy_file(bak, RobloxSettingsFile(), fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(bak, ec);
    return true;
}

// ---------------------------------------------------------------- construcao
Tweak Make(const wchar_t* id, Category c, const wchar_t* title, const wchar_t* desc, bool rec, bool rev) {
    Tweak t;
    t.id = id;
    t.category = c;
    t.title = title;
    t.description = desc;
    t.recommended = rec;
    t.revertible = rev;
    return t;
}

Tweak MakeReg(const wchar_t* id, Category c, const wchar_t* title, const wchar_t* desc, bool rec,
              const std::vector<RegEntry>& entries) {
    Tweak t = Make(id, c, title, desc, rec, true);
    t.apply = [entries](const LogFn& log) { return ApplyRegs(entries, log); };
    t.revert = [entries](const LogFn& log) { return RevertRegs(entries, log); };
    return t;
}

Tweak MakeAppx(const wchar_t* id, const wchar_t* title, const wchar_t* desc, bool rec,
               const std::vector<std::wstring>& names) {
    Tweak t = Make(id, Category::Bloat, title, desc, rec, false);
    t.apply = [names](const LogFn& log) { return RemoveAppx(names, log); };
    return t;
}

}  // namespace

bool CreateRestorePoint(const LogFn& log) {
    std::wstring ps =
        L"Enable-ComputerRestore -Drive 'C:\\'; "
        L"Checkpoint-Computer -Description 'XYZ Optimizer' -RestorePointType MODIFY_SETTINGS";
    bool ok = RunPowerShell(ps, 180000);
    if (!ok) log(L"  o Windows nao criou o ponto (a protecao do sistema pode estar desligada).");
    return ok;
}

std::vector<Tweak> BuildTweaks() {
    std::vector<Tweak> v;

    // ===================== DESEMPENHO =====================
    {
        Tweak t = Make(L"input_mouse", Category::Input, L"Mouse sem aceleração",
                       L"Desliga a precisão do ponteiro e fixa a velocidade em 6/11. A mira fica 1:1 com a mão.",
                       true, true);
        t.apply = MouseApply;
        t.revert = MouseRevert;
        v.push_back(t);
    }
    {
        Tweak t = Make(L"perf_power", Category::Input, L"Plano Alto Desempenho",
                       L"Mantém o clock da CPU alto e estável, sem oscilar entre uma ação e outra.", true, true);
        t.apply = PowerHigh;
        t.revert = PowerBalanced;
        v.push_back(t);
    }
    v.push_back(MakeReg(
        L"perf_gamedvr", Category::Input, L"Desligar Game DVR",
        L"Remove a captura de tela em segundo plano da Xbox Game Bar, que gasta CPU e GPU.", true,
        {D(Root::CU, L"System\\GameConfigStore", L"GameDVR_Enabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", 0),
         D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", 0)}));
    v.push_back(MakeReg(
        L"perf_gamemode", Category::Input, L"Modo Jogo do Windows",
        L"Ativa o Modo Jogo: prioriza o jogo em primeiro plano e reduz interrupções do Windows.", true,
        {D(Root::CU, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1),
         D(Root::CU, L"Software\\Microsoft\\GameBar", L"AllowAutoGameMode", 1)}));
    v.push_back(MakeReg(
        L"input_sticky", Category::Input, L"Sem pop-up de aderência",
        L"Evita que apertar Shift 5 vezes ou segurar teclas abra janelas de acessibilidade no meio do jogo.", true,
        {S(Root::CU, L"Control Panel\\Accessibility\\StickyKeys", L"Flags", L"506"),
         S(Root::CU, L"Control Panel\\Accessibility\\Keyboard Response", L"Flags", L"122"),
         S(Root::CU, L"Control Panel\\Accessibility\\ToggleKeys", L"Flags", L"58")}));
    v.push_back(MakeReg(
        L"perf_visual", Category::Input, L"Efeitos visuais mínimos",
        L"Desliga transparência e animações. Ganho pequeno, mais visível em PCs fracos. Reinicie a sessão.", true,
        {D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects", L"VisualFXSetting", 2),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", 0),
         S(Root::CU, L"Control Panel\\Desktop\\WindowMetrics", L"MinAnimate", L"0"),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAnimations", 0)}));

    // ===================== BLOATWARE =====================
    v.push_back(MakeAppx(
        L"bloat_ms", L"Apps inúteis da Microsoft",
        L"Notícias, Clima, Dicas, Solitaire, Feedback, Seu Telefone, Mixed Reality, Teams e similares.", true,
        {L"Microsoft.BingNews", L"Microsoft.BingWeather", L"Microsoft.GetHelp", L"Microsoft.Getstarted",
         L"Microsoft.MicrosoftOfficeHub", L"Microsoft.MicrosoftSolitaireCollection", L"Microsoft.People",
         L"Microsoft.WindowsFeedbackHub", L"Microsoft.YourPhone", L"Microsoft.ZuneMusic", L"Microsoft.ZuneVideo",
         L"Microsoft.MixedReality.Portal", L"Microsoft.Microsoft3DViewer", L"Microsoft.3DBuilder",
         L"Microsoft.SkypeApp", L"Microsoft.Todos", L"Microsoft.PowerAutomateDesktop", L"Microsoft.WindowsMaps",
         L"Microsoft.windowscommunicationsapps", L"MicrosoftTeams", L"MSTeams", L"Clipchamp.Clipchamp",
         L"Microsoft.549981C3F5F10", L"Microsoft.MicrosoftJournal"}));
    v.push_back(MakeAppx(
        L"bloat_3p", L"Apps de terceiros pré-instalados",
        L"Candy Crush, TikTok, Disney+, Netflix, Spotify, Instagram, Facebook e outros que vêm de fábrica.", true,
        {L"*CandyCrush*", L"*king.com*", L"*TikTok*", L"*Disney*", L"*Netflix*", L"*Spotify*", L"*Instagram*",
         L"*Facebook*", L"*Twitter*", L"*LinkedIn*", L"*AmazonPrimeVideo*", L"*Duolingo*", L"*Hulu*",
         L"*Pinterest*", L"*PicsArt*", L"*BubbleWitch*"}));
    v.push_back(MakeAppx(
        L"bloat_xbox", L"Apps do Xbox (opcional)",
        L"Remove o app Xbox e a Game Bar. Não use se joga Game Pass ou Minecraft Bedrock.", false,
        {L"Microsoft.XboxApp", L"Microsoft.XboxGamingOverlay", L"Microsoft.XboxGameOverlay",
         L"Microsoft.GamingApp"}));
    {
        Tweak t = Make(L"bloat_onedrive", Category::Bloat, L"Remover OneDrive",
                       L"Desinstala o OneDrive. Arquivos já sincronizados continuam na sua pasta de usuário.",
                       false, false);
        t.apply = RemoveOneDrive;
        v.push_back(t);
    }
    v.push_back(MakeReg(
        L"bloat_suggest", Category::Bloat, L"Bloquear apps e anúncios sugeridos",
        L"Impede o Windows de reinstalar apps sozinho e de mostrar sugestões no Iniciar.", true,
        {D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"ContentDeliveryAllowed", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"OemPreInstalledAppsEnabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"PreInstalledAppsEnabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"PreInstalledAppsEverEnabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SilentInstalledAppsEnabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SoftLandingEnabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SystemPaneSuggestionsEnabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338387Enabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338388Enabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338389Enabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-353694Enabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-353696Enabled", 0),
         D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableWindowsConsumerFeatures", 1)}));
    v.push_back(MakeReg(
        L"bloat_widgets", Category::Bloat, L"Widgets, Notícias e Copilot",
        L"Desliga widgets, feed de notícias, busca em destaque e o Copilot do Windows.", true,
        {D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests", 0),
         D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Feeds", L"EnableFeeds", 0),
         D(Root::CU, L"Software\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", 1),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\SearchSettings", L"IsDynamicSearchBoxEnabled", 0)}));

    // ===================== PRIVACIDADE =====================
    {
        std::vector<RegEntry> regs = {
            D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 0),
            D(Root::LM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\DataCollection", L"AllowTelemetry", 0),
            D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"DoNotShowFeedbackNotifications", 1)};
        Tweak t = Make(L"priv_telemetry", Category::Privacy, L"Desativar telemetria",
                       L"Para o serviço DiagTrack, as tarefas de coleta e define coleta mínima de dados.", true, true);
        t.apply = [regs](const LogFn& log) {
            bool ok = ApplyRegs(regs, log);
            ok = ServiceDisable(L"DiagTrack", log) && ok;
            ok = ServiceDisable(L"dmwappushservice", log) && ok;
            for (const wchar_t* task : kTelemetryTasks) TaskSet(task, false);
            return ok;
        };
        t.revert = [regs](const LogFn& log) {
            bool ok = RevertRegs(regs, log);
            ok = ServiceRestore(L"DiagTrack", L"auto") && ok;
            ok = ServiceRestore(L"dmwappushservice", L"demand") && ok;
            for (const wchar_t* task : kTelemetryTasks) TaskSet(task, true);
            return ok;
        };
        v.push_back(t);
    }
    v.push_back(MakeReg(
        L"priv_adid", Category::Privacy, L"Sem ID de publicidade",
        L"Desliga o ID de anúncios e o rastreio de apps mais usados.", true,
        {D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 0),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_TrackProgs", 0),
         D(Root::CU, L"Control Panel\\International\\User Profile", L"HttpAcceptLanguageOptOut", 1)}));
    v.push_back(MakeReg(
        L"priv_bgapps", Category::Privacy, L"Apps em segundo plano",
        L"Impede apps da Store de rodarem escondidos gastando CPU, RAM e rede.", true,
        {D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled", 1),
         D(Root::CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BackgroundAppGlobalToggle", 0),
         D(Root::LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy", L"LetAppsRunInBackground", 2)}));
    {
        std::vector<RegEntry> regs = {
            D(Root::LM, L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", L"Disabled", 1)};
        Tweak t = Make(L"priv_wer", Category::Privacy, L"Relatório de erros do Windows",
                       L"Desliga o envio de relatórios de falhas e o serviço WerSvc.", true, true);
        t.apply = [regs](const LogFn& log) {
            bool ok = ApplyRegs(regs, log);
            ok = ServiceDisable(L"WerSvc", log) && ok;
            return ok;
        };
        t.revert = [regs](const LogFn& log) {
            bool ok = RevertRegs(regs, log);
            ok = ServiceRestore(L"WerSvc", L"demand") && ok;
            return ok;
        };
        v.push_back(t);
    }

    // ===================== ROBLOX =====================
    {
        Tweak t = Make(L"roblox_potato", Category::Roblox, L"Gráfico de batata",
                       L"Qualidade gráfica no nível 1 para o máximo de FPS. Feche o Roblox antes de aplicar.", false,
                       true);
        t.apply = RobloxPotatoApply;
        t.revert = RobloxPotatoRevert;
        v.push_back(t);
    }

    // ===================== LIMPEZA =====================
    {
        Tweak t = Make(L"clean_temp", Category::Clean, L"Arquivos temporários",
                       L"Apaga as pastas Temp do usuário e do Windows. Arquivos em uso são ignorados.", true, false);
        t.apply = CleanTemp;
        v.push_back(t);
    }
    {
        Tweak t = Make(L"clean_bin", Category::Clean, L"Esvaziar lixeira",
                       L"Remove definitivamente tudo que está na Lixeira de todas as unidades.", false, false);
        t.apply = EmptyBin;
        v.push_back(t);
    }
    {
        Tweak t = Make(L"clean_dumps", Category::Clean, L"Relatórios e dumps de falha",
                       L"Remove CrashDumps, Minidump e as filas de relatório de erro do Windows.", true, false);
        t.apply = CleanDumps;
        v.push_back(t);
    }

    return v;
}

}  // namespace xyz
