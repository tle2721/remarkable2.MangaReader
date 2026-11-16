#pragma once

#include "json.hpp"
#include "util.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace aidoku {

namespace fs = std::filesystem;

struct HttpResponse {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

class Client {
    std::string socketPath_;
    std::string serverBin_;
    std::string homeDir_;
    std::mutex spawnMutex_;
    pid_t spawnedPid_ = -1;

public:
    explicit Client(const fs::path& defaultHomeDir) {
        const char* envSock = std::getenv("RM2MANGA_AIDOKU_SOCK");
        if (!envSock) envSock = std::getenv("RM2MANGA_RAKUYOMI_SOCK");

        const char* envBin = std::getenv("RM2MANGA_AIDOKU_SERVER");
        if (!envBin) envBin = std::getenv("RM2MANGA_RAKUYOMI_SERVER");

        const char* envHome = std::getenv("RM2MANGA_AIDOKU_HOME");
        if (!envHome) envHome = std::getenv("RM2MANGA_RAKUYOMI_HOME");

        socketPath_ = envSock ? envSock : "/tmp/rakuyomi.sock";

        if (envBin) {
            serverBin_ = envBin;
        } else if (::access("./backend/aidoku-server", X_OK) == 0) {
            serverBin_ = fs::absolute("./backend/aidoku-server").string();
        } else if (::access("/home/root/xovi/exthome/appload/rm2manga/backend/aidoku-server", X_OK) == 0) {
            serverBin_ = "/home/root/xovi/exthome/appload/rm2manga/backend/aidoku-server";
        } else if (::access("/home/root/.vellum/bin/aidoku-server", X_OK) == 0) {
            serverBin_ = "/home/root/.vellum/bin/aidoku-server";
        } else {
            serverBin_ = "/home/root/xovi/exthome/appload/koreader/plugins/rakuyomi.koplugin/server";
        }

        std::error_code ec;
        homeDir_ = envHome ? envHome : fs::absolute(defaultHomeDir, ec).string();
        ensureSettingsInitialized();
    }

    ~Client() {
        if (spawnedPid_ > 0) {
            int st = 0;
            ::waitpid(spawnedPid_, &st, WNOHANG);
        }
    }

    const std::string& homeDir() const { return homeDir_; }
    std::string settingsFilePath() const { return (fs::path(homeDir_) / "settings.json").string(); }

    void ensureSettingsInitialized() {
        std::error_code ec;
        fs::create_directories(fs::path(homeDir_) / "sources", ec);
        fs::create_directories(fs::path(homeDir_) / "downloads", ec);

        fs::path cfgPath = fs::path(homeDir_) / "settings.json";
        if (!fs::exists(cfgPath, ec)) {
            std::string initial =
                "{\n"
                "  \"source_lists\": [],\n"
                "  \"languages\": [\"en\"]\n"
                "}\n";
            util::atomicWriteFile(cfgPath, initial);
        }
    }

    static std::string formatSourceConfigPretty(const Json::Value& sourceLists,
                                                const Json::Value& languages) {
        std::string out = "{\n  \"source_lists\": [";
        const auto& sl = sourceLists.asArray();
        if (!sl.empty()) {
            out += "\n";
            for (size_t i = 0; i < sl.size(); ++i) {
                out += "    " + Json::stringify(Json::Value(sl[i].str()));
                if (i + 1 < sl.size()) out += ",";
                out += "\n";
            }
            out += "  ";
        }
        out += "],\n  \"languages\": [";
        const auto& langs = languages.asArray();
        for (size_t i = 0; i < langs.size(); ++i) {
            if (i > 0) out += ", ";
            out += Json::stringify(Json::Value(langs[i].str()));
        }
        out += "]\n}";
        return out;
    }

    Json::Value getSourceConfig() const {
        fs::path cfgPath = fs::path(homeDir_) / "settings.json";
        std::string raw;
        Json::Value parsed = Json::Value::object();
        if (util::readFileToString(cfgPath, raw) && !raw.empty()) {
            bool ok = false;
            Json::Value tmp = Json::parse(raw, &ok);
            if (ok && tmp.isObject()) {
                parsed = std::move(tmp);
            }
        }

        Json::Value sl = Json::Value::array();
        if (parsed["source_lists"].isArray()) {
            for (const auto& u : parsed["source_lists"].asArray()) {
                std::string s = util::trim(u.str());
                if (!s.empty()) sl.push(s);
            }
        }

        Json::Value langs = Json::Value::array();
        if (parsed["languages"].isArray()) {
            for (const auto& l : parsed["languages"].asArray()) {
                std::string s = util::trim(l.str());
                if (!s.empty()) langs.push(s);
            }
        }
        if (langs.empty()) {
            langs.push("en");
        }

        Json::Value out = Json::Value::object();
        out["source_lists"] = sl;
        out["languages"] = langs;
        out["config_path"] = cfgPath.string();
        out["raw_json"] = formatSourceConfigPretty(sl, langs);
        return out;
    }

    bool saveSourceConfig(const Json::Value& req, std::string* errOut = nullptr) {
        fs::path cfgPath = fs::path(homeDir_) / "settings.json";
        Json::Value existing = Json::Value::object();
        {
            std::string curRaw;
            if (util::readFileToString(cfgPath, curRaw) && !curRaw.empty()) {
                bool ok = false;
                Json::Value tmp = Json::parse(curRaw, &ok);
                if (ok && tmp.isObject()) existing = std::move(tmp);
            }
        }

        Json::Value newSourceLists = Json::Value::array();
        Json::Value newLangs = Json::Value::array();

        if (req.has("raw_json") && req["raw_json"].isString()) {
            bool ok = false;
            Json::Value parsed = Json::parse(req["raw_json"].str(), &ok);
            if (!ok || !parsed.isObject()) {
                if (errOut) *errOut = "Invalid JSON syntax — expected {\"source_lists\": [...], \"languages\": [...]}";
                return false;
            }
            if (parsed.has("source_lists") && !parsed["source_lists"].isArray()) {
                if (errOut) *errOut = "\"source_lists\" must be a JSON array of URLs";
                return false;
            }
            if (parsed.has("languages") && !parsed["languages"].isArray()) {
                if (errOut) *errOut = "\"languages\" must be a JSON array of language codes";
                return false;
            }
            for (const auto& u : parsed["source_lists"].asArray()) {
                std::string s = util::trim(u.str());
                if (!s.empty()) newSourceLists.push(s);
            }
            for (const auto& l : parsed["languages"].asArray()) {
                std::string s = util::trim(l.str());
                if (!s.empty()) newLangs.push(s);
            }
        } else {
            if (req["source_lists"].isArray()) {
                for (const auto& u : req["source_lists"].asArray()) {
                    std::string s = util::trim(u.str());
                    if (!s.empty()) newSourceLists.push(s);
                }
            }
            if (req["languages"].isArray()) {
                for (const auto& l : req["languages"].asArray()) {
                    std::string s = util::trim(l.str());
                    if (!s.empty()) newLangs.push(s);
                }
            }
        }

        for (const auto& u : newSourceLists.asArray()) {
            const std::string& url = u.str();
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
                if (errOut) *errOut = "Source list URL must start with https:// or http://";
                return false;
            }
        }

        if (newLangs.empty()) {
            newLangs.push("en");
        }

        existing["source_lists"] = std::move(newSourceLists);
        existing["languages"] = std::move(newLangs);

        // If only source_lists and languages are present, write pretty JSON for easy user editing
        std::string serialized;
        if (existing.asObject().size() <= 2) {
            serialized = formatSourceConfigPretty(existing["source_lists"], existing["languages"]) + "\n";
        } else {
            serialized = Json::stringify(existing) + "\n";
        }

        if (!util::atomicWriteFile(cfgPath, serialized)) {
            if (errOut) *errOut = "Failed to write " + cfgPath.string();
            return false;
        }

        restartServer();
        return true;
    }

    bool isAvailableOnDisk() const {
        return ::access(serverBin_.c_str(), X_OK) == 0 || fs::exists(socketPath_);
    }

    bool healthCheck(int timeoutSec = 2) {
        auto r = rawRequest("GET", "/health-check", "", timeoutSec);
        return r.ok && r.status >= 200 && r.status < 300;
    }

    void stopRunningServerLocked(bool onlyForeignHome) {
#ifdef __linux__
        std::error_code ec;
        if (fs::exists("/proc", ec)) {
            for (const auto& entry : fs::directory_iterator("/proc", ec)) {
                std::string pidStr = entry.path().filename().string();
                if (pidStr.empty() || !std::all_of(pidStr.begin(), pidStr.end(), ::isdigit)) continue;
                pid_t pid = static_cast<pid_t>(std::stol(pidStr));
                if (pid <= 1) continue;

                std::string cmdline;
                if (!util::readFileToString(entry.path() / "cmdline", cmdline) || cmdline.empty()) continue;

                std::vector<std::string> args;
                size_t start = 0;
                while (start < cmdline.size()) {
                    size_t nul = cmdline.find('\0', start);
                    if (nul == std::string::npos) nul = cmdline.size();
                    if (nul > start) args.push_back(cmdline.substr(start, nul - start));
                    start = nul + 1;
                }
                if (args.empty()) continue;

                bool isAidokuSrv = (args[0] == serverBin_) ||
                                   (args[0].size() >= 23 &&
                                    args[0].find("rakuyomi.koplugin/server") != std::string::npos) ||
                                   (args[0].find("aidoku-server") != std::string::npos);
                if (!isAidokuSrv) continue;

                std::string argHome = args.size() >= 2 ? args[1] : "";
                if (!onlyForeignHome || argHome != homeDir_) {
                    ::kill(pid, SIGTERM);
                    for (int w = 0; w < 15; ++w) {
                        if (::kill(pid, 0) != 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }
        }
#else
        (void)onlyForeignHome;
#endif
        if (!onlyForeignHome && spawnedPid_ > 0) {
            ::kill(spawnedPid_, SIGTERM);
            for (int w = 0; w < 20; ++w) {
                int st = 0;
                if (::waitpid(spawnedPid_, &st, WNOHANG) == spawnedPid_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            spawnedPid_ = -1;
        }
    }

    bool restartServer() {
        {
            std::lock_guard<std::mutex> lock(spawnMutex_);
            if (::access(serverBin_.c_str(), X_OK) == 0) {
                stopRunningServerLocked(false);
                std::error_code ec;
                if (fs::exists(socketPath_, ec)) {
                    ::unlink(socketPath_.c_str());
                }
            }
        }
        return ensureRunning();
    }

    bool ensureRunning() {
        {
            std::lock_guard<std::mutex> lock(spawnMutex_);
            // Ensure no foreign Aidoku instance (e.g. different homeDir) holds /tmp/rakuyomi.sock
            if (::access(serverBin_.c_str(), X_OK) == 0) {
                stopRunningServerLocked(true);
            }
        }

        if (healthCheck(1)) return true;

        std::lock_guard<std::mutex> lock(spawnMutex_);
        if (healthCheck(1)) return true;

        if (::access(serverBin_.c_str(), X_OK) != 0) {
            return false;
        }

        ensureSettingsInitialized();

        std::error_code ec;
        if (fs::exists(socketPath_, ec)) {
            ::unlink(socketPath_.c_str());
        }

        pid_t pid = ::fork();
        if (pid == 0) {
            ::setsid();
            int logFd = ::open("/tmp/rm2manga-aidoku.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (logFd >= 0) {
                ::dup2(logFd, STDOUT_FILENO);
                ::dup2(logFd, STDERR_FILENO);
                ::close(logFd);
            }
            ::execl(serverBin_.c_str(), serverBin_.c_str(), homeDir_.c_str(), nullptr);
            _exit(127);
        }
        if (pid > 0) {
            spawnedPid_ = pid;
        }

        for (int i = 0; i < 65; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            if (healthCheck(1)) {
                return true;
            }
        }
        return false;
    }

    Json::Value requestJson(const std::string& method,
                            const std::string& path,
                            const Json::Value* bodyObj = nullptr,
                            int timeoutSec = 45,
                            std::string* errOut = nullptr) {
        if (!ensureRunning()) {
            if (errOut) *errOut = "Aidoku server is not available";
            return Json::Value();
        }

        std::string bodyStr;
        if (bodyObj != nullptr) {
            bodyStr = Json::stringify(*bodyObj);
        }

        HttpResponse resp = rawRequest(method, path, bodyStr, timeoutSec);
        if (!resp.ok) {
            if (errOut) *errOut = resp.error.empty() ? "Aidoku request failed" : resp.error;
            return Json::Value();
        }

        bool parseOk = false;
        Json::Value parsed = resp.body.empty() ? Json::Value::object() : Json::parse(resp.body, &parseOk);
        if (resp.status < 200 || resp.status >= 300) {
            if (errOut) {
                std::string msg = parsed["message"].str();
                *errOut = msg.empty() ? ("HTTP " + std::to_string(resp.status)) : msg;
            }
            return Json::Value();
        }
        if (!resp.body.empty() && !parseOk) {
            if (errOut) *errOut = "Invalid JSON from Aidoku server";
            return Json::Value();
        }
        return parsed;
    }

    Json::Value getLibrary(std::string* err = nullptr) {
        return requestJson("GET", "/library", nullptr, 25, err);
    }

    Json::Value searchMangas(const std::string& query, std::string* err = nullptr) {
        Json::Value raw = requestJson("GET", "/mangas?q=" + util::urlEncode(query), nullptr, 45, err);
        if (!raw.isArray()) return Json::Value::array();

        // Normalize both Manga[] and SourceMangaSearchResults[] shapes
        Json::Value flat = Json::Value::array();
        for (const auto& item : raw.asArray()) {
            if (item.isObject() && item["mangas"].isArray()) {
                Json::Value srcInfo = item["source_information"];
                for (const auto& m : item["mangas"].asArray()) {
                    Json::Value copy = m;
                    if (!copy.has("source") && srcInfo.isObject()) {
                        copy["source"] = srcInfo;
                    }
                    flat.push(std::move(copy));
                }
            } else if (item.isObject() && item.has("id")) {
                flat.push(item);
            }
        }
        return flat;
    }

    bool addToLibrary(const std::string& sourceId, const std::string& mangaId, std::string* err = nullptr) {
        std::string localErr;
        requestJson("POST", "/mangas/" + util::urlEncode(sourceId) + "/" + util::urlEncode(mangaId) + "/add-to-library",
                    nullptr, 25, &localErr);
        if (err) *err = localErr;
        return localErr.empty();
    }

    bool removeFromLibrary(const std::string& sourceId, const std::string& mangaId, std::string* err = nullptr) {
        std::string localErr;
        requestJson("POST", "/mangas/" + util::urlEncode(sourceId) + "/" + util::urlEncode(mangaId) + "/remove-from-library",
                    nullptr, 25, &localErr);
        if (err) *err = localErr;
        return localErr.empty();
    }

    Json::Value listCachedChapters(const std::string& sourceId, const std::string& mangaId, std::string* err = nullptr) {
        return requestJson("GET", "/mangas/" + util::urlEncode(sourceId) + "/" + util::urlEncode(mangaId) + "/chapters",
                           nullptr, 30, err);
    }

    bool refreshChapters(const std::string& sourceId, const std::string& mangaId, std::string* err = nullptr) {
        std::string localErr;
        requestJson("POST", "/mangas/" + util::urlEncode(sourceId) + "/" + util::urlEncode(mangaId) + "/refresh-chapters",
                    nullptr, 60, &localErr);
        if (err) *err = localErr;
        return localErr.empty();
    }

    bool markChapterRead(const std::string& sourceId,
                         const std::string& mangaId,
                         const std::string& chapterId,
                         std::string* err = nullptr) {
        std::string localErr;
        requestJson("POST",
                    "/mangas/" + util::urlEncode(sourceId) + "/" + util::urlEncode(mangaId) +
                        "/chapters/" + util::urlEncode(chapterId) + "/mark-as-read",
                    nullptr, 15, &localErr);
        if (err) *err = localErr;
        return localErr.empty();
    }

    std::string downloadChapter(const std::string& sourceId,
                                const std::string& mangaId,
                                const std::string& chapterId,
                                double chapterNum,
                                bool hasChapterNum,
                                const std::atomic<bool>* cancelFlag,
                                const std::function<void(const std::string&)>& onStatus,
                                std::string* errOut = nullptr) {
        // 1. Try job-based async endpoint so we can monitor & cancel cleanly
        Json::Value jobReq = Json::Value::object();
        jobReq["source_id"] = sourceId;
        jobReq["manga_id"] = mangaId;
        jobReq["chapter_id"] = chapterId;
        if (hasChapterNum) {
            jobReq["chapter_num"] = chapterNum;
        }

        std::string jobErr;
        Json::Value jobIdVal = requestJson("POST", "/jobs/download-chapter", &jobReq, 20, &jobErr);
        if (jobErr.empty() && jobIdVal.isString() && !jobIdVal.str().empty()) {
            std::string jobId = jobIdVal.str();
            if (onStatus) onStatus("Downloading from " + sourceId + "...");

            for (int tick = 0; tick < 360; ++tick) {  // up to ~2 minutes
                if (cancelFlag && cancelFlag->load()) {
                    requestJson("DELETE", "/jobs/" + util::urlEncode(jobId), nullptr, 5, nullptr);
                    if (errOut) *errOut = "Download cancelled";
                    return "";
                }

                std::string pollErr;
                Json::Value st = requestJson("GET", "/jobs/" + util::urlEncode(jobId), nullptr, 15, &pollErr);
                if (!pollErr.empty()) {
                    if (errOut) *errOut = pollErr;
                    return "";
                }
                std::string jtype = st["type"].str();
                if (jtype == "COMPLETED") {
                    std::string cbzPath = st["data"].str();
                    if (!cbzPath.empty()) return cbzPath;
                    break;
                }
                if (jtype == "ERROR") {
                    std::string emsg = st["data"]["message"].str("Chapter download failed");
                    if (errOut) *errOut = emsg;
                    return "";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
            }
        }

        // 2. Fallback to direct synchronous download endpoint
        std::string path = "/mangas/" + util::urlEncode(sourceId) + "/" + util::urlEncode(mangaId) +
                           "/chapters/" + util::urlEncode(chapterId) + "/download";
        if (hasChapterNum) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6g", chapterNum);
            path += "?chapter_num=" + util::urlEncode(buf);
        }
        std::string syncErr;
        Json::Value direct = requestJson("POST", path, nullptr, 90, &syncErr);
        if (syncErr.empty() && direct.isString() && !direct.str().empty()) {
            return direct.str();
        }
        if (errOut) *errOut = syncErr.empty() ? (jobErr.empty() ? "Download failed" : jobErr) : syncErr;
        return "";
    }

    Json::Value listInstalledSources(std::string* err = nullptr) {
        return requestJson("GET", "/installed-sources", nullptr, 15, err);
    }

    Json::Value listAvailableSources(std::string* err = nullptr) {
        return requestJson("GET", "/available-sources", nullptr, 35, err);
    }

    bool installSource(const std::string& sourceId, std::string* err = nullptr) {
        std::string localErr;
        requestJson("POST", "/available-sources/" + util::urlEncode(sourceId) + "/install", nullptr, 45, &localErr);
        if (err) *err = localErr;
        return localErr.empty();
    }

    bool uninstallSource(const std::string& sourceId, std::string* err = nullptr) {
        std::string localErr;
        requestJson("DELETE", "/installed-sources/" + util::urlEncode(sourceId), nullptr, 20, &localErr);
        if (err) *err = localErr;
        return localErr.empty();
    }

private:
    static std::string decodeChunked(const std::string& raw) {
        std::string out;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t lineEnd = raw.find("\r\n", pos);
            if (lineEnd == std::string::npos) break;
            std::string hexLine = raw.substr(pos, lineEnd - pos);
            size_t semi = hexLine.find(';');
            if (semi != std::string::npos) hexLine.resize(semi);
            size_t chunkLen = std::strtoull(hexLine.c_str(), nullptr, 16);
            pos = lineEnd + 2;
            if (chunkLen == 0) break;
            if (pos + chunkLen > raw.size()) {
                out.append(raw.data() + pos, raw.size() - pos);
                break;
            }
            out.append(raw.data() + pos, chunkLen);
            pos += chunkLen + 2;
        }
        return out;
    }

    HttpResponse rawRequest(const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            int timeoutSec) {
        HttpResponse res;
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            res.error = "socket() failed";
            return res;
        }

        struct timeval tv {};
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            res.error = "Failed to connect to " + socketPath_;
            return res;
        }

        std::string req = method + " " + path + " HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Accept: application/json\r\n"
                          "Connection: close\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/json\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        } else if (method == "POST" || method == "PUT") {
            req += "Content-Length: 0\r\n";
        }
        req += "\r\n";
        req += body;

        size_t sent = 0;
        while (sent < req.size()) {
            ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
            if (n <= 0) {
                ::close(fd);
                res.error = "send() failed";
                return res;
            }
            sent += static_cast<size_t>(n);
        }

        std::string raw;
        char buf[8192];
        size_t headerEnd = std::string::npos;
        int64_t contentLength = -1;
        bool isChunked = false;

        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                raw.append(buf, static_cast<size_t>(n));
                if (headerEnd == std::string::npos) {
                    size_t idx = raw.find("\r\n\r\n");
                    if (idx != std::string::npos) {
                        headerEnd = idx + 4;
                        std::string hdrs = util::toLower(raw.substr(0, idx));
                        size_t clPos = hdrs.find("content-length:");
                        if (clPos != std::string::npos) {
                            size_t valStart = clPos + 15;
                            while (valStart < hdrs.size() && hdrs[valStart] == ' ') ++valStart;
                            contentLength = std::strtoll(hdrs.c_str() + valStart, nullptr, 10);
                        }
                        if (hdrs.find("transfer-encoding: chunked") != std::string::npos) {
                            isChunked = true;
                        }
                    }
                }
                if (headerEnd != std::string::npos && contentLength >= 0) {
                    if (raw.size() - headerEnd >= static_cast<size_t>(contentLength)) {
                        break;
                    }
                }
                if (headerEnd != std::string::npos && isChunked) {
                    if (raw.find("\r\n0\r\n\r\n", headerEnd - 2) != std::string::npos) {
                        break;
                    }
                }
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                break;
            }
        }
        ::close(fd);

        if (headerEnd == std::string::npos) {
            res.error = "Malformed HTTP response from Aidoku server";
            return res;
        }

        // Parse status code from first line: HTTP/1.1 200 OK
        size_t sp1 = raw.find(' ');
        if (sp1 != std::string::npos && sp1 < headerEnd) {
            res.status = std::atoi(raw.c_str() + sp1 + 1);
        }

        std::string payload = raw.substr(headerEnd);
        if (isChunked) {
            res.body = decodeChunked(payload);
        } else if (contentLength >= 0 && payload.size() > static_cast<size_t>(contentLength)) {
            res.body = payload.substr(0, static_cast<size_t>(contentLength));
        } else {
            res.body = std::move(payload);
        }
        res.ok = (res.status > 0);
        return res;
    }
};

}  // namespace aidoku
