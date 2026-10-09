// Small helpers: strings, files, subprocesses, paths.
#pragma once
#include <string>
#include <vector>

namespace pifx {

std::string trim(const std::string& s);
std::string toLower(std::string s);
bool startsWith(const std::string& s, const std::string& p);
bool endsWith(const std::string& s, const std::string& p);
bool contains(const std::string& hay, const std::string& needle);   // case-insensitive
std::vector<std::string> split(const std::string& s, char sep);
std::vector<std::string> splitLines(const std::string& s);
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string shellQuote(const std::string& s);

bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);   // atomic (tmp + rename)
bool fileExists(const std::string& path);
bool isDir(const std::string& path);
bool makeDirs(const std::string& path);
std::vector<std::string> listDir(const std::string& path);           // names, sorted, no . / ..
std::string joinPath(const std::string& a, const std::string& b);
std::string parentDir(const std::string& path);
std::string homeDir();
std::string exeDir();

// Runs `cmd` through /bin/sh, returns stdout+stderr. *status = exit code (-1 on failure).
std::string runCapture(const std::string& cmd, int* status = nullptr);
bool which(const std::string& prog);

// Resolve an absolute path inside $PIFX_SYSROOT (tests point it at a fake tree).
std::string sysPath(const std::string& abs);

}  // namespace pifx
