#include "core/Util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace pifx {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string& hay, const std::string& needle) {
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::string format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof buf) return std::string(buf, std::max(0, n));
    std::string big((size_t)n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.resize((size_t)n);
    return big;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool makeDirs(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    std::string parent = parentDir(path);
    if (!parent.empty() && parent != path && !makeDirs(parent)) return false;
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (!b.empty() && b[0] == '/') return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

std::string parentDir(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    auto i = p.find_last_of('/');
    if (i == std::string::npos) return "";
    return i == 0 ? "/" : p.substr(0, i);
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    return h ? h : "/tmp";
}

std::string exeDir() {
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return parentDir(buf);
}

std::string runCapture(const std::string& cmd, int* status) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!p) {
        if (status) *status = -1;
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int st = ::pclose(p);
    if (status) *status = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    return out;
}

bool which(const std::string& prog) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    for (const auto& dir : split(path, ':')) {
        std::string p = joinPath(dir.empty() ? "." : dir, prog);
        if (::access(p.c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::string sysPath(const std::string& abs) {
    const char* root = std::getenv("PIFX_SYSROOT");
    if (!root || !*root || std::string(root) == "/") return abs;
    std::string r = root;
    while (!r.empty() && r.back() == '/') r.pop_back();
    return r + (abs.empty() || abs[0] != '/' ? "/" : "") + abs;
}

}  // namespace pifx
