#include "FileAnalyzer.h"

#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sys/stat.h>
#include <cstring>

#ifdef _WIN32
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
#endif

// Static member initialization
const std::set<std::string> FileAnalyzer::REFERENCE_EXTS = {".ma", ".mb", ".fbx", ".abc"};
const std::set<std::string> FileAnalyzer::TEXTURE_EXTS = {
    ".png", ".jpg", ".jpeg", ".tif", ".tiff", ".exr",
    ".tga", ".bmp", ".tx", ".hdr", ".psd", ".dds"
};
const std::set<std::string> FileAnalyzer::CACHE_EXTS = {".abc", ".fbx"};

// Build PATH_EXTS as union of REFERENCE_EXTS and TEXTURE_EXTS
static std::set<std::string> buildPathExts() {
    std::set<std::string> s = FileAnalyzer::REFERENCE_EXTS;
    s.insert(FileAnalyzer::TEXTURE_EXTS.begin(), FileAnalyzer::TEXTURE_EXTS.end());
    return s;
}
const std::set<std::string> FileAnalyzer::PATH_EXTS = buildPathExts();

// Helper: get file extension in lowercase
static std::string getLowerExt(const std::string& path) {
    size_t dotPos = path.rfind('.');
    if (dotPos == std::string::npos) return "";
    std::string ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

// Helper: get directory from path
static std::string getDirname(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    size_t pos = p.rfind('/');
    if (pos == std::string::npos) return ".";
    return p.substr(0, pos);
}

// Helper: get basename from path
static std::string getBasename(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    size_t pos = p.rfind('/');
    if (pos == std::string::npos) return p;
    return p.substr(pos + 1);
}

// Helper: normalize path (forward slashes, resolve . and ..)
static std::string normalizePath(const std::string& path) {
    std::string p = path;
    for (auto& c : p) {
        if (c == '\\') c = '/';
    }
    // Simple normalization: remove trailing slashes, collapse //
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// Helper: check if path is absolute
static bool isAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    if (path.size() >= 2 && std::isalpha((unsigned char)path[0]) && path[1] == ':') return true;
    return false;
}

// Helper: join paths
static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    std::string d = dir;
    if (d.back() != '/' && d.back() != '\\') d += '/';
    return d + file;
}

FileAnalyzer::FileAnalyzer(const std::string& filePath)
    : filePath_(filePath)
{
    // Normalize the file path
    for (auto& c : filePath_) {
        if (c == '\\') c = '/';
    }
    fileDir_ = getDirname(filePath_);
}

bool FileAnalyzer::analyze() {
    references.clear();
    textures.clear();
    caches.clear();
    errors.clear();
    warnings.clear();
    seenReferences_.clear();
    seenTextures_.clear();
    seenCaches_.clear();

    if (!fileExists(filePath_)) {
        errors.push_back("File does not exist: " + filePath_);
        return false;
    }

    std::string ext = getLowerExt(filePath_);
    if (ext == ".ma") {
        return analyzeMa();
    }
    if (ext == ".mb") {
        return analyzeMb();
    }

    errors.push_back("Unsupported file extension: " + ext);
    return false;
}

bool FileAnalyzer::analyzeMa() {
#ifdef _WIN32
    std::ifstream ifs(utf8ToWide(filePath_), std::ios::in | std::ios::binary);
#else
    std::ifstream ifs(filePath_, std::ios::in | std::ios::binary);
#endif
    if (!ifs.is_open()) {
        errors.push_back("Failed to read file: " + filePath_);
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // Reference pattern: file ... "path.ma|mb|fbx|abc" ;
    // NOTE: .ma files are multi-line; do not anchor to beginning-of-input.
    std::regex refPattern(
        // Use a custom raw-string delimiter because the pattern itself can contain `)"`.
        R"PT(file\s+[^;]*?"([^"\r\n]+\.(?:ma|mb|fbx|abc))"\s*;)PT",
        std::regex::icase
    );

    auto refBegin = std::sregex_iterator(content.begin(), content.end(), refPattern);
    auto refEnd = std::sregex_iterator();
    for (auto it = refBegin; it != refEnd; ++it) {
        addReference((*it)[1].str());
    }

    // String attribute pattern: setAttr "..." -type "string" "path"
    std::regex attrPattern(
        "setAttr\\s+\"[^\"]+\"\\s+-type\\s+\"string\"\\s+\"([^\"\\r\\n]+)\"",
        std::regex::icase
    );

    auto attrBegin = std::sregex_iterator(content.begin(), content.end(), attrPattern);
    auto attrEnd = std::sregex_iterator();
    for (auto it = attrBegin; it != attrEnd; ++it) {
        std::string rawPath = (*it)[1].str();
        std::string ext = getLowerExt(rawPath);
        if (TEXTURE_EXTS.count(ext)) {
            addTexture(rawPath);
        } else if (CACHE_EXTS.count(ext)) {
            addCache(rawPath);
        }
    }

    return true;
}

bool FileAnalyzer::analyzeMb() {
#ifdef _WIN32
    std::ifstream ifs(utf8ToWide(filePath_), std::ios::in | std::ios::binary);
#else
    std::ifstream ifs(filePath_, std::ios::in | std::ios::binary);
#endif
    if (!ifs.is_open()) {
        errors.push_back("Failed to read file: " + filePath_);
        return false;
    }

    std::vector<char> data((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
    ifs.close();

    std::vector<std::string> strings = extractAsciiStrings(data, 6);
    std::vector<std::string> utf16Strings = extractUtf16LeStrings(data, 6);
    strings.insert(strings.end(), utf16Strings.begin(), utf16Strings.end());

    for (const auto& value : strings) {
        if (!looksLikePath(value)) continue;

        std::string ext = getLowerExt(value);
        if (ext == ".ma" || ext == ".mb") {
            addReference(value);
        } else if (CACHE_EXTS.count(ext)) {
            addCache(value);
        } else if (TEXTURE_EXTS.count(ext)) {
            addTexture(value);
        }
    }

    return true;
}

std::vector<std::string> FileAnalyzer::extractAsciiStrings(const std::vector<char>& data, int minLength) {
    std::vector<std::string> strings;
    std::string current;

    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char byte = (unsigned char)data[i];
        if (byte >= 32 && byte <= 126) {
            current += (char)byte;
        } else {
            if ((int)current.size() >= minLength) {
                strings.push_back(current);
            }
            current.clear();
        }
    }
    if ((int)current.size() >= minLength) {
        strings.push_back(current);
    }

    return strings;
}

std::vector<std::string> FileAnalyzer::extractUtf16LeStrings(const std::vector<char>& data, int minLength) {
    std::vector<std::string> strings;
    std::string chars;

    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        unsigned char lo = (unsigned char)data[i];
        unsigned char hi = (unsigned char)data[i + 1];
        if (hi == 0 && lo >= 32 && lo <= 126) {
            chars += (char)lo;
        } else {
            if ((int)chars.size() >= minLength) {
                strings.push_back(chars);
            }
            chars.clear();
        }
    }
    if ((int)chars.size() >= minLength) {
        strings.push_back(chars);
    }

    return strings;
}

bool FileAnalyzer::looksLikePath(const std::string& value) const {
    if (value.empty()) return false;

    // Trim quotes
    std::string v = value;
    while (!v.empty() && (v.front() == '"' || v.front() == '\'' || v.front() == ' ')) v.erase(v.begin());
    while (!v.empty() && (v.back() == '"' || v.back() == '\'' || v.back() == ' ')) v.pop_back();

    std::string ext = getLowerExt(v);
    if (PATH_EXTS.find(ext) == PATH_EXTS.end()) return false;

    if (v.find('/') != std::string::npos || v.find('\\') != std::string::npos) return true;
    if (v.size() >= 2 && std::isalpha((unsigned char)v[0]) && v[1] == ':') return true;
    if (v.substr(0, 2) == "./" || v.substr(0, 3) == "../") return true;
    if (v.find('$') != std::string::npos || v.find('%') != std::string::npos) return true;

    // Allow bare filenames with valid extensions
    return getBasename(v) == v;
}

void FileAnalyzer::addReference(const std::string& path) {
    std::string normalized = normalizePath(path);
    if (seenReferences_.count(normalized)) return;
    seenReferences_.insert(normalized);

    AnalyzedDep dep;
    dep.path = normalized;
    dep.exists = fileExists(normalized);
    dep.size = dep.exists ? getFileSize(normalized) : 0;
    dep.sizeStr = formatSize(dep.size);
    dep.type = "reference";
    references.push_back(dep);
}

void FileAnalyzer::addTexture(const std::string& path) {
    std::string normalized = normalizePath(path);
    if (seenTextures_.count(normalized)) return;
    seenTextures_.insert(normalized);

    AnalyzedDep dep;
    dep.path = normalized;
    dep.exists = fileExists(normalized);
    dep.size = dep.exists ? getFileSize(normalized) : 0;
    dep.sizeStr = formatSize(dep.size);
    dep.type = "texture";
    textures.push_back(dep);
}

void FileAnalyzer::addCache(const std::string& path) {
    std::string normalized = normalizePath(path);
    if (seenCaches_.count(normalized)) return;
    seenCaches_.insert(normalized);

    AnalyzedDep dep;
    dep.path = normalized;
    dep.exists = fileExists(normalized);
    dep.size = dep.exists ? getFileSize(normalized) : 0;
    dep.sizeStr = formatSize(dep.size);
    dep.type = "cache";
    caches.push_back(dep);
}

std::string FileAnalyzer::normalizePath(const std::string& path) const {
    std::string value = path;
    // Trim whitespace and quotes
    while (!value.empty() && (value.front() == '"' || value.front() == '\'' || value.front() == ' '))
        value.erase(value.begin());
    while (!value.empty() && (value.back() == '"' || value.back() == '\'' || value.back() == ' '))
        value.pop_back();

    // Remove Maya reference copy number suffix {N}
    std::regex copyNum(R"(\{\d+\}$)");
    value = std::regex_replace(value, copyNum, "");

    // Expand environment variables on Windows
#ifdef _WIN32
    if (value.find('%') != std::string::npos || value.find('$') != std::string::npos) {
        std::wstring wvalue = utf8ToWide(value);
        wchar_t wexpanded[32768];
        DWORD len = ExpandEnvironmentStringsW(wvalue.c_str(), wexpanded, 32768);
        if (len > 0 && len < 32768) {
            int u8len = WideCharToMultiByte(CP_UTF8, 0, wexpanded, -1, nullptr, 0, nullptr, nullptr);
            if (u8len > 0) {
                std::string utf8result(u8len, '\0');
                int ret = WideCharToMultiByte(CP_UTF8, 0, wexpanded, -1, &utf8result[0], u8len, nullptr, nullptr);
                if (ret > 0 && !utf8result.empty() && utf8result.back() == '\0') utf8result.pop_back();
                value = utf8result;
            }
        }
    }
#endif

    // Normalize separators
    for (auto& c : value) {
        if (c == '\\') c = '/';
    }

    if (value.empty()) return value;

    // Make absolute if relative
    if (!isAbsolutePath(value)) {
        value = joinPath(fileDir_, value);
    }

    // Normalize
    for (auto& c : value) {
        if (c == '\\') c = '/';
    }

    return value;
}

int64_t FileAnalyzer::getFileSize(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) == 0) {
        return (int64_t)st.st_size;
    }
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return (int64_t)st.st_size;
    }
#endif
    return 0;
}

std::string FileAnalyzer::formatSize(int64_t size) {
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

bool FileAnalyzer::fileExists(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    return (_wstat(utf8ToWide(path).c_str(), &st) == 0 && (st.st_mode & S_IFREG));
#else
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG));
#endif
}

AnalysisSummary FileAnalyzer::summary() const {
    AnalysisSummary s;
    s.file = filePath_;
    s.references = (int)references.size();
    s.textures = (int)textures.size();
    s.caches = (int)caches.size();

    s.missingReferences = 0;
    for (const auto& r : references) if (!r.exists) s.missingReferences++;
    s.missingTextures = 0;
    for (const auto& t : textures) if (!t.exists) s.missingTextures++;
    s.missingCaches = 0;
    for (const auto& c : caches) if (!c.exists) s.missingCaches++;

    s.totalMissing = s.missingReferences + s.missingTextures + s.missingCaches;
    s.errors = errors;
    s.warnings = warnings;
    return s;
}

std::string FileAnalyzer::getReport() const {
    std::ostringstream oss;
    std::string sep60(60, '=');

    oss << sep60 << "\n";
    oss << "Maya File Dependency Report\n";
    oss << "File: " << filePath_ << "\n";
    oss << sep60 << "\n";

    if (!errors.empty()) {
        oss << "\n[Errors]\n";
        for (const auto& e : errors) {
            oss << "  - " << e << "\n";
        }
    }

    if (!warnings.empty()) {
        oss << "\n[Warnings]\n";
        for (const auto& w : warnings) {
            oss << "  - " << w << "\n";
        }
    }

    oss << "\n[References] " << references.size() << "\n";
    int missingRefs = 0;
    for (const auto& r : references) if (!r.exists) missingRefs++;
    if (missingRefs > 0) oss << "  Missing: " << missingRefs << "\n";
    for (const auto& r : references) {
        oss << "  [" << (r.exists ? "OK" : "MISSING") << "] " << r.path << " (" << r.sizeStr << ")\n";
    }

    oss << "\n[Textures] " << textures.size() << "\n";
    int missingTex = 0;
    for (const auto& t : textures) if (!t.exists) missingTex++;
    if (missingTex > 0) oss << "  Missing: " << missingTex << "\n";
    for (const auto& t : textures) {
        oss << "  [" << (t.exists ? "OK" : "MISSING") << "] " << t.path << " (" << t.sizeStr << ")\n";
    }

    oss << "\n[Caches] " << caches.size() << "\n";
    int missingCache = 0;
    for (const auto& c : caches) if (!c.exists) missingCache++;
    if (missingCache > 0) oss << "  Missing: " << missingCache << "\n";
    for (const auto& c : caches) {
        oss << "  [" << (c.exists ? "OK" : "MISSING") << "] " << c.path << " (" << c.sizeStr << ")\n";
    }

    oss << "\n" << sep60 << "\n";
    oss << "Total Missing: " << (missingRefs + missingTex + missingCache) << "\n";

    // Large files warning
    std::vector<const AnalyzedDep*> largeFiles;
    for (const auto& r : references) {
        if (r.size > 100 * 1024 * 1024) largeFiles.push_back(&r);
    }
    for (const auto& c : caches) {
        if (c.size > 100 * 1024 * 1024) largeFiles.push_back(&c);
    }
    if (!largeFiles.empty()) {
        oss << "\nLarge files (>100MB):\n";
        for (const auto* f : largeFiles) {
            oss << "  - " << f->path << " (" << f->sizeStr << ")\n";
        }
    }

    oss << sep60 << "\n";
    return oss.str();
}

std::vector<AnalyzedDep> FileAnalyzer::getMissingFiles() const {
    std::vector<AnalyzedDep> missing;
    for (const auto& r : references) {
        if (!r.exists) {
            AnalyzedDep d = r;
            d.type = "reference";
            missing.push_back(d);
        }
    }
    for (const auto& t : textures) {
        if (!t.exists) {
            AnalyzedDep d = t;
            d.type = "texture";
            missing.push_back(d);
        }
    }
    for (const auto& c : caches) {
        if (!c.exists) {
            AnalyzedDep d = c;
            d.type = "cache";
            missing.push_back(d);
        }
    }
    return missing;
}
