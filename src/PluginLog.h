#pragma once

#include <string>
#include <vector>

namespace PluginLog {

void init();
void shutdown();

void info(const char* module, const std::string& msg);
void warn(const char* module, const std::string& msg);
void error(const char* module, const std::string& msg);

// Log environment context (codepage, Maya version, OS, scene path).
// Called automatically by init(), but can be called again if needed.
void logEnvironment();

// Log a scan summary block (e.g. after RefChecker scan or batch export).
struct ScanSummary {
    std::string module;       // e.g. "RefChecker", "BatchExporter"
    std::string scenePath;
    int totalItems   = 0;
    int okItems      = 0;
    int missingItems = 0;
    int fixedItems   = 0;
    std::vector<std::string> notes; // optional extra lines
};
void logScanSummary(const ScanSummary& summary);

} // namespace PluginLog
