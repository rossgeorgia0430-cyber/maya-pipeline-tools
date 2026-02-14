#pragma once
#ifndef FILEANALYZER_H
#define FILEANALYZER_H

#include <string>
#include <vector>
#include <set>

struct AnalyzedDep {
    std::string path;
    bool exists;
    int64_t size;
    std::string sizeStr;
    std::string type; // "reference", "texture", "cache"
};

struct AnalysisSummary {
    std::string file;
    int references;
    int textures;
    int caches;
    int missingReferences;
    int missingTextures;
    int missingCaches;
    int totalMissing;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class FileAnalyzer {
public:
    explicit FileAnalyzer(const std::string& filePath);

    bool analyze();

    AnalysisSummary summary() const;
    std::string getReport() const;
    std::vector<AnalyzedDep> getMissingFiles() const;

    // Public data
    std::vector<AnalyzedDep> references;
    std::vector<AnalyzedDep> textures;
    std::vector<AnalyzedDep> caches;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

private:
    bool analyzeMa();
    bool analyzeMb();

    std::vector<std::string> extractAsciiStrings(const std::vector<char>& data, int minLength = 6);
    std::vector<std::string> extractUtf16LeStrings(const std::vector<char>& data, int minLength = 6);
    bool looksLikePath(const std::string& value) const;

    void addReference(const std::string& path);
    void addTexture(const std::string& path);
    void addCache(const std::string& path);

    std::string normalizePath(const std::string& path) const;
    static int64_t getFileSize(const std::string& path);
    static std::string formatSize(int64_t size);
    static bool fileExists(const std::string& path);

    std::string filePath_;
    std::string fileDir_;

    std::set<std::string> seenReferences_;
    std::set<std::string> seenTextures_;
    std::set<std::string> seenCaches_;

public:
    static const std::set<std::string> REFERENCE_EXTS;
    static const std::set<std::string> TEXTURE_EXTS;
    static const std::set<std::string> CACHE_EXTS;
    static const std::set<std::string> PATH_EXTS;

private:
};

#endif // FILEANALYZER_H
