#pragma once
#ifndef EXPORTLOGGER_H
#define EXPORTLOGGER_H

#include <string>
#include <vector>
#include <ctime>

struct LogEntry {
    std::string filePath;
    std::string fileType;
    std::string characterName;
    int64_t fileSize;
    double duration;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct LogSummary {
    int totalFiles;
    int successFiles;
    int failedFiles;
    int globalWarnings;
    int globalErrors;
    double duration;
    std::string status;
};

class ExportLogger {
public:
    ExportLogger(const std::string& shotDir, int startFrame, int endFrame);

    void addEntry(const std::string& filePath,
                  const std::string& fileType,
                  const std::string& characterName = "",
                  int64_t fileSize = 0,
                  double duration = 0.0,
                  const std::vector<std::string>& warnings = {},
                  const std::vector<std::string>& errors = {});

    void addWarning(const std::string& msg);
    void addError(const std::string& msg);

    std::string getLogFilename() const;
    LogSummary getSummary() const;
    std::string write();

private:
    static void ensureDir(const std::string& path);
    static std::string formatSize(int64_t size);
    std::vector<std::string> buildLines() const;

    std::string shotDir_;
    int startFrame_;
    int endFrame_;
    std::vector<LogEntry> entries_;
    std::vector<std::string> warnings_;
    std::vector<std::string> errors_;
    time_t startTime_;
};

#endif // EXPORTLOGGER_H
