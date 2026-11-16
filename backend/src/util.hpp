#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace util {

namespace fs = std::filesystem;

inline int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string urlEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

inline std::string hashSlug(const std::string& input) {
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 1099511628211ULL;
    for (unsigned char c : input) {
        h1 ^= c;
        h1 *= 1099511628211ULL;
        h2 = ((h2 << 5) + h2) ^ c;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx%08llx",
                  static_cast<unsigned long long>(h1),
                  static_cast<unsigned long long>(h2 & 0xFFFFFFFFULL));
    return std::string(buf);
}

inline std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Natural filename sort so page_2.jpg < page_10.jpg
inline bool naturalLess(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        unsigned char ca = static_cast<unsigned char>(a[ia]);
        unsigned char cb = static_cast<unsigned char>(b[ib]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t za = ia;
            while (za < a.size() && a[za] == '0') ++za;
            size_t zb = ib;
            while (zb < b.size() && b[zb] == '0') ++zb;

            size_t ea = za;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;
            size_t eb = zb;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;

            size_t lenA = ea - za;
            size_t lenB = eb - zb;
            if (lenA != lenB) return lenA < lenB;
            for (size_t k = 0; k < lenA; ++k) {
                if (a[za + k] != b[zb + k]) return a[za + k] < b[zb + k];
            }
            if ((ea - ia) != (eb - ib)) return (ea - ia) < (eb - ib);
            ia = ea;
            ib = eb;
        } else {
            unsigned char la = static_cast<unsigned char>(std::tolower(ca));
            unsigned char lb = static_cast<unsigned char>(std::tolower(cb));
            if (la != lb) return la < lb;
            ++ia;
            ++ib;
        }
    }
    return a.size() < b.size();
}

inline bool readFileToString(const fs::path& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streampos sz = ifs.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!out.empty()) {
        ifs.read(&out[0], static_cast<std::streamsize>(out.size()));
    }
    return true;
}

inline bool atomicWriteFile(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmpPath = path;
    tmpPath += ".tmp";

    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmpPath.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);

    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }
    return true;
}

struct CmdResult {
    int exitCode = -1;
    std::string stdoutData;
};

inline CmdResult runProcess(const std::vector<std::string>& args, int timeoutSec = 30) {
    CmdResult res;
    if (args.empty()) return res;

    int pipefd[2];
    if (::pipe(pipefd) != 0) return res;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return res;
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::close(pipefd[1]);

        std::vector<char*> cargs;
        cargs.reserve(args.size() + 1);
        for (const auto& s : args) {
            cargs.push_back(const_cast<char*>(s.c_str()));
        }
        cargs.push_back(nullptr);
        ::execvp(cargs[0], cargs.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    int flags = ::fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    char buf[8192];
    bool eof = false;

    while (!eof) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ::kill(pid, SIGKILL);
            break;
        }
        int remainMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;

        int pr = ::poll(&pfd, 1, std::min(remainMs, 250));
        if (pr > 0) {
            while (true) {
                ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
                if (n > 0) {
                    res.stdoutData.append(buf, static_cast<size_t>(n));
                } else if (n == 0) {
                    eof = true;
                    break;
                } else {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
                    break;
                }
            }
        }
    }
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
    }
    return res;
}

inline bool hasBinary(const char* absPath) {
    return ::access(absPath, X_OK) == 0;
}

inline std::string getUserAgentForUrl(const std::string& url) {
    if (url.find("mangadex.org") != std::string::npos) {
        return "rm2manga/1.0.2 (https://github.com/tle2721/remarkable2.MangaReader)";
    }
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
}

inline std::string getRefererForUrl(const std::string& url) {
    if (url.find("mfcdn.net") != std::string::npos ||
        url.find("fanfox.net") != std::string::npos ||
        url.find("mangafox") != std::string::npos ||
        url.find("zjcdn") != std::string::npos) {
        return "https://fanfox.net/";
    }
    if (url.find("mangahere") != std::string::npos) {
        return "https://www.mangahere.cc/";
    }
    if (url.find("mangakatana") != std::string::npos) {
        return "https://mangakatana.com/";
    }
    if (url.find("mkklcdn") != std::string::npos ||
        url.find("2xstorage") != std::string::npos ||
        url.find("manganato") != std::string::npos ||
        url.find("mangakakalot") != std::string::npos) {
        return "https://chapmanganato.to/";
    }
    if (url.find("asuracomic") != std::string::npos) {
        return "https://asuracomic.net/";
    }
    if (url.find("toonily") != std::string::npos) {
        return "https://toonily.com/";
    }
    if (url.find("mangadex.org") != std::string::npos) {
        return "https://mangadex.org/";
    }
    size_t schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
        size_t hostEnd = url.find('/', schemeEnd + 3);
        if (hostEnd != std::string::npos) {
            return url.substr(0, hostEnd + 1);
        }
        return url + "/";
    }
    return "";
}

inline std::vector<std::string> candidateCurlPaths() {
    static const char* kPaths[] = {
        "/home/root/.vellum/bin/curl",
        "/opt/bin/curl",
        "/usr/bin/curl",
        "/bin/curl",
        "/opt/homebrew/bin/curl"
    };
    std::vector<std::string> out;
    for (const char* p : kPaths) {
        if (hasBinary(p)) out.emplace_back(p);
    }
    return out;
}

inline std::vector<std::string> candidateWgetPaths() {
    static const char* kPaths[] = {
        "/home/root/.vellum/bin/wget",
        "/opt/bin/wget",
        "/usr/bin/wget",
        "/bin/wget"
    };
    std::vector<std::string> out;
    for (const char* p : kPaths) {
        if (hasBinary(p)) out.emplace_back(p);
    }
    return out;
}

inline bool httpsGetString(const std::string& url, std::string& bodyOut, int timeoutSec = 25) {
    std::string ua = getUserAgentForUrl(url);
    std::string ref = getRefererForUrl(url);

    for (const auto& curlBin : candidateCurlPaths()) {
        std::vector<std::string> args = {
            curlBin, "-g", "-fsSL", "--max-time", std::to_string(timeoutSec),
            "-A", ua
        };
        if (!ref.empty()) {
            args.push_back("-e");
            args.push_back(ref);
        }
        args.push_back(url);
        auto r = runProcess(args, timeoutSec + 5);
        if (r.exitCode == 0 && !r.stdoutData.empty()) {
            bodyOut = std::move(r.stdoutData);
            return true;
        }
    }

    for (const auto& wgetBin : candidateWgetPaths()) {
        std::vector<std::string> args = {
            wgetBin, "-q", "-T", std::to_string(timeoutSec),
            "-U", ua
        };
        if (!ref.empty()) {
            args.push_back("--header=Referer: " + ref);
        }
        args.push_back("-O");
        args.push_back("-");
        args.push_back(url);
        auto r = runProcess(args, timeoutSec + 5);
        if (r.exitCode == 0 && !r.stdoutData.empty()) {
            bodyOut = std::move(r.stdoutData);
            return true;
        }
    }
    return false;
}

inline bool httpsDownloadFile(const std::string& url, const fs::path& dest, int timeoutSec = 45) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    fs::path tmp = dest;
    tmp += ".part";

    std::string ua = getUserAgentForUrl(url);
    std::string ref = getRefererForUrl(url);
    bool ok = false;

    for (const auto& curlBin : candidateCurlPaths()) {
        std::vector<std::string> args = {
            curlBin, "-g", "-fsSL", "--max-time", std::to_string(timeoutSec),
            "-A", ua
        };
        if (!ref.empty()) {
            args.push_back("-e");
            args.push_back(ref);
        }
        args.push_back("-o");
        args.push_back(tmp.string());
        args.push_back(url);
        auto r = runProcess(args, timeoutSec + 5);
        if (r.exitCode == 0 && fs::exists(tmp, ec) && fs::file_size(tmp, ec) > 0) {
            ok = true;
            break;
        }
    }

    if (!ok) {
        for (const auto& wgetBin : candidateWgetPaths()) {
            std::vector<std::string> args = {
                wgetBin, "-q", "-T", std::to_string(timeoutSec),
                "-U", ua
            };
            if (!ref.empty()) {
                args.push_back("--header=Referer: " + ref);
            }
            args.push_back("-O");
            args.push_back(tmp.string());
            args.push_back(url);
            auto r = runProcess(args, timeoutSec + 5);
            if (r.exitCode == 0 && fs::exists(tmp, ec) && fs::file_size(tmp, ec) > 0) {
                ok = true;
                break;
            }
        }
    }

    if (ok) {
        if (::rename(tmp.c_str(), dest.c_str()) == 0) {
            return true;
        }
    }
    fs::remove(tmp, ec);
    return false;
}

inline uint64_t directorySizeBytes(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

}  // namespace util
