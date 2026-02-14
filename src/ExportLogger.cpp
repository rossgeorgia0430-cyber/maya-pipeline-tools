#include "ExportLogger.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#ifdef _WIN32
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
#endif

ExportLogger::ExportLogger(const std::string& shotDir, int startFrame, int endFrame)
    : shotDir_(shotDir)
    , startFrame_(startFrame)
    , endFrame_(endFrame)
    , startTime_(std::time(nullptr))
{
}

void ExportLogger::ensureDir(const std::string& path) {
    if (path.empty()) return;
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) == 0) return;
    _wmkdir(utf8ToWide(path).c_str());
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return;
    mkdir(path.c_str(), 0755);
#endif
}

std::string ExportLogger::formatSize(int64_t size) {
    if (size <= 0) return "-";

    std::ostringstream oss;
    if (size < 1024) {
        oss << size << " B";
    } else if (size < 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << (size / 1024.0) << " KB";
    } else if (size < (int64_t)1024 * 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << (size / (1024.0 * 1024.0)) << " MB";
    } else {
        oss << std::fixed << std::setprecision(2) << (size / (1024.0 * 1024.0 * 1024.0)) << " GB";
    }
    return oss.str();
}

void ExportLogger::addEntry(const std::string& filePath,
                            const std::string& fileType,
                            const std::string& characterName,
                            int64_t fileSize,
                            double duration,
                            const std::vector<std::string>& warnings,
                            const std::vector<std::string>& errors) {
    LogEntry entry;
    entry.filePath = filePath;
    entry.fileType = fileType;
    entry.characterName = characterName;
    entry.fileSize = (fileSize > 0) ? fileSize : 0;
    entry.duration = duration;
    entry.warnings = warnings;
    entry.errors = errors;
    entries_.push_back(entry);
}

void ExportLogger::addWarning(const std::string& msg) {
    if (!msg.empty()) {
        warnings_.push_back(msg);
    }
}

void ExportLogger::addError(const std::string& msg) {
    if (!msg.empty()) {
        errors_.push_back(msg);
    }
}

std::string ExportLogger::getLogFilename() const {
    std::ostringstream oss;
    oss << startFrame_ << "-" << endFrame_ << ".log";
    return oss.str();
}

LogSummary ExportLogger::getSummary() const {
    LogSummary s;
    s.totalFiles = (int)entries_.size();
    s.successFiles = 0;
    s.failedFiles = 0;
    for (const auto& e : entries_) {
        if (e.errors.empty()) {
            s.successFiles++;
        } else {
            s.failedFiles++;
        }
    }
    s.globalWarnings = (int)warnings_.size();
    s.globalErrors = (int)errors_.size();
    s.duration = std::difftime(std::time(nullptr), startTime_);
    s.status = (s.globalErrors > 0 || s.failedFiles > 0) ? "FAILED" : "SUCCESS";
    return s;
}

std::vector<std::string> ExportLogger::buildLines() const {
    LogSummary s = getSummary();
    std::vector<std::string> lines;

    std::string sep70(70, '=');
    std::string sep70d(70, '-');

    lines.push_back(sep70);
    lines.push_back("Batch Animation Export Log");
    lines.push_back(sep70);

    // Current time
    time_t now = std::time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);

    lines.push_back(std::string("Time       : ") + timeBuf);
    lines.push_back(std::string("Shot Dir   : ") + shotDir_);

    {
        std::ostringstream oss;
        oss << "Frame Range: " << startFrame_ << "-" << endFrame_;
        lines.push_back(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Total Frames: " << (endFrame_ - startFrame_) << "f";
        lines.push_back(oss.str());
    }
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << "Total Time : " << s.duration << "s";
        lines.push_back(oss.str());
    }
    lines.push_back(std::string("Status     : ") + s.status);
    {
        std::ostringstream oss;
        oss << "Files      : " << s.totalFiles
            << " (ok: " << s.successFiles
            << ", fail: " << s.failedFiles << ")";
        lines.push_back(oss.str());
    }
    lines.push_back(sep70);

    if (!warnings_.empty()) {
        lines.push_back("");
        lines.push_back("[WARNINGS]");
        for (const auto& w : warnings_) {
            lines.push_back("  - " + w);
        }
    }

    if (!errors_.empty()) {
        lines.push_back("");
        lines.push_back("[ERRORS]");
        for (const auto& e : errors_) {
            lines.push_back("  - " + e);
        }
    }

    lines.push_back("");
    lines.push_back(sep70d);
    lines.push_back("Export Details");
    lines.push_back(sep70d);

    for (size_t i = 0; i < entries_.size(); ++i) {
        const LogEntry& entry = entries_[i];
        lines.push_back("");

        {
            std::ostringstream oss;
            std::string typeUpper = entry.fileType;
            for (auto& c : typeUpper) c = (char)toupper((unsigned char)c);
            oss << "  [" << (i + 1) << "] " << typeUpper;
            lines.push_back(oss.str());
        }

        if (!entry.characterName.empty()) {
            lines.push_back("      Character : " + entry.characterName);
        }
        lines.push_back("      File      : " + entry.filePath);
        lines.push_back("      Size      : " + formatSize(entry.fileSize));

        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << "      Duration  : " << entry.duration << "s";
            lines.push_back(oss.str());
        }

        for (const auto& w : entry.warnings) {
            lines.push_back("      [WARN] " + w);
        }
        for (const auto& e : entry.errors) {
            lines.push_back("      [ERROR] " + e);
        }

        lines.push_back(std::string("      Status    : ") + (entry.errors.empty() ? "OK" : "FAIL"));
    }

    lines.push_back("");
    lines.push_back(sep70);
    lines.push_back("END OF LOG");
    lines.push_back(sep70);

    return lines;
}

std::string ExportLogger::write() {
    std::string logName = getLogFilename();
    std::string logPath = shotDir_;
    if (!logPath.empty() && logPath.back() != '/' && logPath.back() != '\\') {
        logPath += "/";
    }
    logPath += logName;

    std::vector<std::string> lines = buildLines();
    std::string content;
    for (const auto& line : lines) {
        content += line + "\n";
    }

    try {
        ensureDir(shotDir_);
#ifdef _WIN32
        std::ofstream ofs(utf8ToWide(logPath), std::ios::out | std::ios::trunc);
#else
        std::ofstream ofs(logPath, std::ios::out | std::ios::trunc);
#endif
        if (ofs.is_open()) {
            ofs << content;
            ofs.close();
            return logPath;
        }
    } catch (...) {
        // Fall through to fallback
    }

    // Fallback: write beside current working directory
    std::string fallback = logName;
    std::ofstream ofs(fallback, std::ios::out | std::ios::trunc);
    if (ofs.is_open()) {
        ofs << content;
        ofs << "\n\n[LOGGER WARNING] Failed to write primary log to: " << logPath;
        ofs.close();
    }
    return fallback;
}
