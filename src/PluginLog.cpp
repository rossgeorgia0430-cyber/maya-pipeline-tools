#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MString.h>

#include <fstream>
#include <ctime>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#ifdef _WIN32
// Convert UTF-8 std::string to std::wstring
static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring wstr(wlen, L'\0');
    int ret = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], wlen);
    if (ret <= 0) return {};
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return wstr;
}

// Convert UTF-8 std::string to MString (preserve Unicode for Script Editor output)
static MString utf8ToMString(const std::string& utf8) {
    if (utf8.empty()) return MString();
    std::wstring w = utf8ToWide(utf8);
    if (!w.empty()) return MString(w.c_str());
    // Fallback: best-effort
    return MString(utf8.c_str());
}

// Convert MString to UTF-8 std::string safely
static std::string toUtf8(const MString& ms) {
    const wchar_t* wstr = ms.asWChar();
    if (!wstr || !*wstr) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string(ms.asChar());  // fallback
    std::string result(len, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    if (ret <= 0) return std::string(ms.asChar());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}
#else
static std::string toUtf8(const MString& ms) {
    return std::string(ms.asChar());
}

static MString utf8ToMString(const std::string& utf8) {
    return MString(utf8.c_str());
}
#endif

namespace PluginLog {

static std::mutex sMutex;
static std::string sLogPath;
static bool sFileOk = false;

static std::string getTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

static int64_t fileSizeBytes(const std::string& path) {
    if (path.empty()) return -1;
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
#endif
}

static void ensureDir(const std::string& dir) {
#ifdef _WIN32
    _wmkdir(utf8ToWide(dir).c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

static bool dirExists(const std::string& dir) {
#ifdef _WIN32
    struct _stat st;
    return (_wstat(utf8ToWide(dir).c_str(), &st) == 0 && (st.st_mode & S_IFDIR));
#else
    struct stat st;
    return (stat(dir.c_str(), &st) == 0 && (st.st_mode & S_IFDIR));
#endif
}

static void rotateIfNeeded() {
    if (sLogPath.empty()) return;
#ifdef _WIN32
    std::wstring wpath = utf8ToWide(sLogPath);
    struct _stat st;
    if (_wstat(wpath.c_str(), &st) == 0 && st.st_size > 10 * 1024 * 1024) {
        std::wstring wbak = utf8ToWide(sLogPath + ".bak");
        _wremove(wbak.c_str());
        _wrename(wpath.c_str(), wbak.c_str());
    }
#else
    struct stat st;
    if (stat(sLogPath.c_str(), &st) == 0 && st.st_size > 10 * 1024 * 1024) {
        std::string bak = sLogPath + ".bak";
        std::remove(bak.c_str());
        std::rename(sLogPath.c_str(), bak.c_str());
    }
#endif
}

// Open ofstream with UTF-8 path (MSVC supports wstring constructor)
static std::ofstream openLog(const std::string& path, std::ios_base::openmode mode) {
#ifdef _WIN32
    return std::ofstream(utf8ToWide(path), mode);
#else
    return std::ofstream(path, mode);
#endif
}

void init() {
    std::lock_guard<std::mutex> lock(sMutex);

    bool opened = false;

    // Try MAYA_APP_DIR/PipelineTools/ first (user-writable Maya prefs directory)
    // e.g. C:/Users/<user>/Documents/maya/2026/
    {
        MString appDir;
        MGlobal::executeCommand("internalVar -userAppDir", appDir);
        std::string dir = toUtf8(appDir);
        if (!dir.empty()) {
            if (dir.back() != '/' && dir.back() != '\\')
                dir.push_back('/');
            dir += "PipelineTools";
            ensureDir(dir);

            // Verify directory was created
            if (dirExists(dir)) {
                sLogPath = dir + "/PipelineTools.log";
                rotateIfNeeded();
                const int64_t priorSize = fileSizeBytes(sLogPath);
                std::ofstream ofs = openLog(sLogPath, std::ios::app | std::ios::binary);
                if (ofs.is_open()) {
                    sFileOk = true;
                    opened = true;
                    if (priorSize <= 0) {
                        // Help common Windows editors auto-detect UTF-8.
                        ofs << "\xEF\xBB\xBF";
                    }
                    ofs << "\n========================================\n"
                        << "  PipelineTools Session Start: " << getTimestamp() << "\n"
                        << "  Log path: " << sLogPath << "\n"
                        << "========================================\n";
                }
            }
        }
    }

    // Fallback to %TEMP%/MayaRefChecker/
    if (!opened) {
        const char* temp = std::getenv("TEMP");
        if (!temp || !*temp) temp = std::getenv("TMP");
        if (!temp || !*temp) temp = ".";

        std::string dir = std::string(temp);
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
            dir.push_back('/');
        dir += "MayaRefChecker";
        ensureDir(dir);

        sLogPath = dir + "/PipelineTools.log";
        rotateIfNeeded();

        const int64_t priorSize = fileSizeBytes(sLogPath);
        std::ofstream ofs = openLog(sLogPath, std::ios::app | std::ios::binary);
        if (ofs.is_open()) {
            sFileOk = true;
            if (priorSize <= 0) {
                ofs << "\xEF\xBB\xBF";
            }
            ofs << "\n========================================\n"
                << "  PipelineTools Session Start: " << getTimestamp() << "\n"
                << "  Log path: " << sLogPath << " (fallback)\n"
                << "========================================\n";
        } else {
            sFileOk = false;
        }
    }
}

void logEnvironment() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sFileOk) return;

    std::ofstream ofs = openLog(sLogPath, std::ios::app);
    if (!ofs.is_open()) return;

    ofs << "--- Environment ---\n";

    // Maya version
    MString mayaVer;
    MGlobal::executeCommand("about -v", mayaVer);
    ofs << "  Maya Version : " << toUtf8(mayaVer) << "\n";

    // Maya API version
    // NOTE: `about -api` returns a numeric value (e.g. 20260300). Capturing
    // it into an MString yields an empty string in Maya 2026 (and may vary
    // across versions), so use the int overload explicitly.
    int apiVerInt = 0;
    if (MGlobal::executeCommand("about -api", apiVerInt) == MS::kSuccess && apiVerInt > 0) {
        ofs << "  API Version  : " << apiVerInt << "\n";
    } else {
        // Best-effort fallback for unexpected output types.
        MString apiVerStr;
        MGlobal::executeCommand("about -api", apiVerStr);
        ofs << "  API Version  : " << toUtf8(apiVerStr) << "\n";
    }

    // OS info from Maya
    MString osInfo;
    MGlobal::executeCommand("about -os", osInfo);
    ofs << "  OS (Maya)    : " << toUtf8(osInfo) << "\n";

#ifdef _WIN32
    // System codepage (ACP)
    UINT acp = GetACP();
    ofs << "  System ACP   : " << acp << "\n";

    // Console codepage
    UINT ocp = GetOEMCP();
    ofs << "  OEM Codepage : " << ocp << "\n";

    // UTF-8 manifest check
    ofs << "  UTF-8 ACP    : " << (acp == 65001 ? "Yes" : "No") << "\n";
#endif

    // Current scene
    MString scenePath;
    MGlobal::executeCommand("file -q -sn", scenePath);
    std::string scene = toUtf8(scenePath);
    ofs << "  Scene        : " << (scene.empty() ? "(untitled)" : scene) << "\n";

    // Workspace
    MString workspace;
    MGlobal::executeCommand("workspace -q -rd", workspace);
    ofs << "  Workspace    : " << toUtf8(workspace) << "\n";

    ofs << "-------------------\n";
}

void logScanSummary(const ScanSummary& summary) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sFileOk) return;

    std::ofstream ofs = openLog(sLogPath, std::ios::app);
    if (!ofs.is_open()) return;

    ofs << "\n--- Scan Summary [" << summary.module << "] " << getTimestamp() << " ---\n";
    if (!summary.scenePath.empty()) {
        ofs << "  Scene   : " << summary.scenePath << "\n";
    }
    ofs << "  Total   : " << summary.totalItems << "\n";
    ofs << "  OK      : " << summary.okItems << "\n";
    ofs << "  Missing : " << summary.missingItems << "\n";
    if (summary.fixedItems > 0) {
        ofs << "  Fixed   : " << summary.fixedItems << "\n";
    }
    for (const auto& note : summary.notes) {
        ofs << "  * " << note << "\n";
    }
    ofs << "--- End Summary ---\n";
}

void shutdown() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sFileOk) return;

    std::ofstream ofs = openLog(sLogPath, std::ios::app);
    if (ofs.is_open()) {
        ofs << "[" << getTimestamp() << "][Info][Plugin] Session end.\n";
    }
    sFileOk = false;
}

static void write(const char* level, const char* module, const std::string& msg) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sFileOk) return;

    std::ofstream ofs = openLog(sLogPath, std::ios::app);
    if (ofs.is_open()) {
        ofs << "[" << getTimestamp() << "][" << level << "][" << module << "] " << msg << "\n";
    }
}

void info(const char* module, const std::string& msg) {
    // msg is treated as UTF-8 across the plugin; construct MString via wide-char
    // conversion to avoid mojibake in Script Editor on non-UTF-8 Windows ACP.
    std::string full = std::string("[") + module + "] " + msg;
    MString display = utf8ToMString(full);
    MGlobal::displayInfo(display);
    write("Info", module, msg);
}

void warn(const char* module, const std::string& msg) {
    std::string full = std::string("[") + module + "] " + msg;
    MString display = utf8ToMString(full);
    MGlobal::displayWarning(display);
    write("Warn", module, msg);
}

void error(const char* module, const std::string& msg) {
    std::string full = std::string("[") + module + "] " + msg;
    MString display = utf8ToMString(full);
    MGlobal::displayError(display);
    write("Error", module, msg);
}

} // namespace PluginLog
