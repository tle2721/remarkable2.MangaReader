#pragma once

// Runtime adapter for user-installed `multi.mangadex` Aidoku extension.
// Not active or visible unless the user explicitly installs `multi.mangadex`
// from their own Aidoku source list (`rakuyomi` v1.22.2 uses WASM ABI 0.6 and
// cannot execute `multi.mangadex` v8 WASM ABI 0.7 directly).

#include "json.hpp"
#include "util.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mangadex {

namespace fs = std::filesystem;

inline constexpr const char* kExtSourceId = "multi.mangadex";

inline std::string pickLocalized(const Json::Value& dict, const Json::Value& altTitles = Json::Value()) {
    if (dict.isString()) return dict.str();
    if (dict.isObject()) {
        if (dict.has("en") && !dict["en"].str().empty()) return dict["en"].str();
        if (altTitles.isArray()) {
            for (const auto& alt : altTitles.asArray()) {
                if (alt.isObject() && alt.has("en") && !alt["en"].str().empty()) {
                    return alt["en"].str();
                }
            }
        }
        if (dict.has("ja-ro") && !dict["ja-ro"].str().empty()) return dict["ja-ro"].str();
        if (dict.has("ko-ro") && !dict["ko-ro"].str().empty()) return dict["ko-ro"].str();
        for (const auto& kv : dict.asObject()) {
            if (!kv.second.str().empty()) return kv.second.str();
        }
    }
    return "";
}

class Client {
    std::string apiBase_ = "https://api.mangadex.org";

public:
    Client() {
        if (const char* envBase = std::getenv("RM2MANGA_MANGADEX_API")) {
            apiBase_ = envBase;
        }
    }

    static Json::Value parseMangaObject(const Json::Value& dataObj) {
        Json::Value out = Json::Value::object();
        std::string id = dataObj["id"].str();
        const Json::Value& attrs = dataObj["attributes"];

        out["id"] = id;
        Json::Value src = Json::Value::object();
        src["id"] = kExtSourceId;
        src["name"] = "MangaDex";
        src["version"] = 8;
        out["source"] = src;
        out["source_id"] = kExtSourceId;
        out["source_name"] = "MangaDex";

        std::string title = pickLocalized(attrs["title"], attrs["altTitles"]);
        out["title"] = title.empty() ? id : title;
        out["description"] = pickLocalized(attrs["description"]);
        out["status"] = attrs["status"].str();
        out["year"] = attrs["year"].integer(0);

        std::string author;
        std::string coverFile;
        if (dataObj["relationships"].isArray()) {
            for (const auto& rel : dataObj["relationships"].asArray()) {
                std::string rtype = rel["type"].str();
                if (rtype == "cover_art" && coverFile.empty()) {
                    coverFile = rel["attributes"]["fileName"].str();
                } else if ((rtype == "author" || rtype == "artist") && author.empty()) {
                    author = rel["attributes"]["name"].str();
                }
            }
        }
        out["author"] = author;
        if (!coverFile.empty() && !id.empty()) {
            out["cover_url"] = "https://uploads.mangadex.org/covers/" + id + "/" + coverFile + ".256.jpg";
        } else {
            out["cover_url"] = "";
        }
        return out;
    }

    Json::Value queryMangaList(const std::string& query, const std::string& mode, std::string* errOut = nullptr) {
        std::string url = apiBase_ + "/manga?limit=24"
                          "&includes%5B%5D=cover_art&includes%5B%5D=author"
                          "&contentRating%5B%5D=safe&contentRating%5B%5D=suggestive&contentRating%5B%5D=erotica"
                          "&hasAvailableChapters=true"
                          "&availableTranslatedLanguage%5B%5D=en";
        if (mode == "latest") {
            url += "&order%5BlatestUploadedChapter%5D=desc";
        } else if (mode == "popular" || mode == "discover" || query.empty()) {
            url += "&order%5BfollowedCount%5D=desc";
        } else {
            url += "&title=" + util::urlEncode(query) + "&order%5Brelevance%5D=desc";
        }

        std::string body;
        if (!util::httpsGetString(url, body, 25)) {
            if (errOut) *errOut = "Failed to reach MangaDex API (check Wi-Fi)";
            return Json::Value::array();
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || !root["data"].isArray()) {
            if (errOut) *errOut = "Unexpected response from MangaDex";
            return Json::Value::array();
        }

        Json::Value list = Json::Value::array();
        for (const auto& item : root["data"].asArray()) {
            list.push(parseMangaObject(item));
        }
        return list;
    }

    Json::Value getMangaDetails(const std::string& mangaId, std::string* errOut = nullptr) {
        std::string url = apiBase_ + "/manga/" + util::urlEncode(mangaId) +
                          "?includes%5B%5D=cover_art&includes%5B%5D=author";
        std::string body;
        if (!util::httpsGetString(url, body, 20)) {
            if (errOut) *errOut = "Failed to fetch manga details from MangaDex";
            return Json::Value();
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || !root["data"].isObject()) {
            if (errOut) *errOut = "Invalid MangaDex manga metadata";
            return Json::Value();
        }
        return parseMangaObject(root["data"]);
    }

    Json::Value getChapters(const std::string& mangaId, std::string* errOut = nullptr) {
        Json::Value chapters = Json::Value::array();
        int offset = 0;
        const int limit = 500;

        for (int page = 0; page < 6; ++page) {
            std::string url = apiBase_ + "/manga/" + util::urlEncode(mangaId) +
                              "/feed?translatedLanguage%5B%5D=en"
                              "&limit=" + std::to_string(limit) +
                              "&offset=" + std::to_string(offset) +
                              "&order%5Bchapter%5D=asc&order%5Bvolume%5D=asc"
                              "&includes%5B%5D=scanlation_group"
                              "&contentRating%5B%5D=safe&contentRating%5B%5D=suggestive&contentRating%5B%5D=erotica";
            std::string body;
            if (!util::httpsGetString(url, body, 30)) {
                if (page == 0 && errOut) *errOut = "Failed to fetch chapter feed from MangaDex";
                break;
            }
            bool ok = false;
            Json::Value root = Json::parse(body, &ok);
            if (!ok || !root["data"].isArray()) {
                if (page == 0 && errOut) *errOut = "Invalid chapter feed JSON";
                break;
            }

            const auto& arr = root["data"].asArray();
            for (const auto& ch : arr) {
                const Json::Value& attrs = ch["attributes"];
                if (!attrs["externalUrl"].isNull() && !attrs["externalUrl"].str().empty() &&
                    attrs["pages"].integer(0) <= 0) {
                    continue;
                }

                Json::Value item = Json::Value::object();
                item["id"] = ch["id"].str();
                item["source_id"] = kExtSourceId;
                item["manga_id"] = mangaId;

                std::string chStr = attrs["chapter"].str();
                if (!chStr.empty()) {
                    item["chapter_num"] = std::strtod(chStr.c_str(), nullptr);
                }
                std::string volStr = attrs["volume"].str();
                if (!volStr.empty()) {
                    item["volume_num"] = std::strtod(volStr.c_str(), nullptr);
                }
                item["title"] = attrs["title"].str();

                std::string groupName;
                if (ch["relationships"].isArray()) {
                    for (const auto& rel : ch["relationships"].asArray()) {
                        if (rel["type"].str() == "scanlation_group") {
                            groupName = rel["attributes"]["name"].str();
                            if (!groupName.empty()) break;
                        }
                    }
                }
                item["scanlator"] = groupName;
                item["read"] = false;
                item["downloaded"] = false;
                chapters.push(std::move(item));
            }

            int total = static_cast<int>(root["total"].integer(0));
            offset += static_cast<int>(arr.size());
            if (arr.empty() || offset >= total) break;
        }
        return chapters;
    }

    bool downloadChapterPages(const std::string& chapterId,
                              const fs::path& destDir,
                              const std::atomic<bool>* cancelFlag,
                              const std::function<void(const std::string&, int)>& onProgress,
                              std::string* errOut = nullptr) {
        std::string url = apiBase_ + "/at-home/server/" + util::urlEncode(chapterId);
        std::string body;
        if (!util::httpsGetString(url, body, 25)) {
            if (errOut) *errOut = "Failed to contact MangaDex At-Home server";
            return false;
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || root["result"].str() != "ok") {
            if (errOut) *errOut = "MangaDex At-Home returned an error";
            return false;
        }

        std::string baseUrl = root["baseUrl"].str();
        const Json::Value& chap = root["chapter"];
        std::string hash = chap["hash"].str();
        const auto& dataArr = chap["data"].asArray();
        const auto& saverArr = chap["dataSaver"].asArray();

        if (baseUrl.empty() || hash.empty() || dataArr.empty()) {
            if (errOut) *errOut = "No readable pages found for this chapter";
            return false;
        }

        std::error_code ec;
        fs::path tmpDir = destDir;
        tmpDir += ".downloading";
        fs::remove_all(tmpDir, ec);
        fs::create_directories(tmpDir, ec);

        size_t total = dataArr.size();
        for (size_t i = 0; i < total; ++i) {
            if (cancelFlag && cancelFlag->load()) {
                fs::remove_all(tmpDir, ec);
                if (errOut) *errOut = "Download cancelled";
                return false;
            }

            int pct = static_cast<int>(((i + 1) * 100) / total);
            if (onProgress) {
                onProgress("Downloading page " + std::to_string(i + 1) + " of " + std::to_string(total) + "...", pct);
            }

            std::string fileName = dataArr[i].str();
            std::string ext = ".jpg";
            auto dot = fileName.find_last_of('.');
            if (dot != std::string::npos) {
                ext = util::toLower(fileName.substr(dot));
            }

            char idxBuf[32];
            std::snprintf(idxBuf, sizeof(idxBuf), "%04zu%s", i + 1, ext.c_str());
            fs::path pagePath = tmpDir / idxBuf;

            std::string pageUrl = baseUrl + "/data/" + hash + "/" + fileName;
            bool pageOk = util::httpsDownloadFile(pageUrl, pagePath, 40);

            if (!pageOk && i < saverArr.size()) {
                std::string saverName = saverArr[i].str();
                std::string saverExt = ".jpg";
                auto sdot = saverName.find_last_of('.');
                if (sdot != std::string::npos) saverExt = util::toLower(saverName.substr(sdot));
                std::snprintf(idxBuf, sizeof(idxBuf), "%04zu%s", i + 1, saverExt.c_str());
                pagePath = tmpDir / idxBuf;
                std::string saverUrl = baseUrl + "/data-saver/" + hash + "/" + saverName;
                pageOk = util::httpsDownloadFile(saverUrl, pagePath, 40);
            }

            if (!pageOk) {
                fs::remove_all(tmpDir, ec);
                if (errOut) *errOut = "Failed downloading page " + std::to_string(i + 1);
                return false;
            }
        }

        fs::remove_all(destDir, ec);
        fs::create_directories(destDir.parent_path(), ec);
        fs::rename(tmpDir, destDir, ec);
        if (ec) {
            if (errOut) *errOut = "Failed finalizing chapter folder";
            return false;
        }
        return true;
    }
};

}  // namespace mangadex
