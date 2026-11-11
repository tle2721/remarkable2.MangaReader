#pragma once

#include "types.hpp"
#include "../../../backend/src/mangadex_client.hpp"
#include "../../../backend/src/aidoku_client.hpp"
#include "../../../backend/src/json.hpp"
#include "../../../backend/src/util.hpp"
#include <string>
#include <vector>
#include <regex>
#include <set>
#include <unordered_set>

namespace kindle {

class SourceManager {
private:
    mangadex::Client mangadex_client_;
    aidoku::Client aidoku_client_;
    std::string active_source_id_ = "mangadex";
    std::vector<SourceInfo> available_sources_;

public:
    SourceManager() : aidoku_client_("/mnt/us/manga/data/aidoku") {
        refreshSources();
    }

    void refreshSources() {
        available_sources_.clear();
        available_sources_.push_back({"mangadex", "MangaDex", "Official API (Multi-language)", true, false, "v5", "all"});
        available_sources_.push_back({"mangafox", "MangaFox", "FanFox & MangaHere (English)", true, false, "v1", "en"});
        available_sources_.push_back({"mangakatana", "MangaKatana", "Fast Scans (English)", true, false, "v1", "en"});

        if (aidoku_client_.isAvailableOnDisk() && aidoku_client_.ensureRunning()) {
            std::string err;
            Json::Value installed = aidoku_client_.listInstalledSources(&err);
            if (installed.isArray()) {
                for (const auto& item : installed.asArray()) {
                    SourceInfo s;
                    s.id = item["id"].str();
                    s.name = item["name"].str();
                    s.description = item["description"].str();
                    s.version = item["version"].str("1.0");
                    s.language = item["language"].str(item["lang"].str("en"));
                    s.is_installed = true;
                    s.is_aidoku = true;
                    available_sources_.push_back(s);
                }
            }
        }
    }

    const std::vector<SourceInfo>& getSources() const { return available_sources_; }
    const std::string& getActiveSourceId() const { return active_source_id_; }
    void setActiveSourceId(const std::string& id) { active_source_id_ = id; }

    bool isAidokuSource(const std::string& id) const {
        if (id == "mangadex" || id == "mangafox" || id == "mangakatana") return false;
        for (const auto& s : available_sources_) {
            if (s.id == id) return s.is_aidoku;
        }
        return false;
    }

    aidoku::Client& aidokuClient() { return aidoku_client_; }

    std::vector<SourceInfo> getAvailableAidokuSources(std::string* errOut = nullptr) {
        std::vector<SourceInfo> list;
        if (!aidoku_client_.ensureRunning()) {
            if (errOut) *errOut = "Aidoku server not running";
            return list;
        }
        Json::Value avail = aidoku_client_.listAvailableSources(errOut);
        if (avail.isArray()) {
            for (const auto& item : avail.asArray()) {
                SourceInfo s;
                s.id = item["id"].str();
                s.name = item["name"].str();
                s.description = item["description"].str();
                if (item["version"].isNumber()) {
                    s.version = "v" + std::to_string(static_cast<int>(item["version"].num()));
                } else {
                    s.version = item["version"].str("1.0");
                }
                std::string lang = item["language"].str(item["lang"].str());
                if (lang.empty()) {
                    auto dot = s.id.find('.');
                    lang = (dot != std::string::npos) ? s.id.substr(0, dot) : "en";
                }
                s.language = lang;
                s.is_aidoku = true;
                s.is_installed = false;
                for (const auto& inst : available_sources_) {
                    if (inst.id == s.id) {
                        s.is_installed = true;
                        break;
                    }
                }
                list.push_back(s);
            }
        }
        return list;
    }

    bool installAidokuSource(const std::string& sourceId, std::string* errOut = nullptr) {
        bool ok = aidoku_client_.installSource(sourceId, errOut);
        if (ok) refreshSources();
        return ok;
    }

    bool uninstallAidokuSource(const std::string& sourceId, std::string* errOut = nullptr) {
        bool ok = aidoku_client_.uninstallSource(sourceId, errOut);
        if (active_source_id_ == sourceId) {
            active_source_id_ = "mangadex";
        }
        refreshSources();
        return ok;
    }

    Json::Value getSourceConfig() {
        return aidoku_client_.getSourceConfig();
    }

    bool addSourceListUrl(const std::string& url, std::string* errOut = nullptr) {
        Json::Value cfg = aidoku_client_.getSourceConfig();
        Json::Value sl = cfg["source_lists"];
        if (!sl.isArray()) sl = Json::Value::array();
        bool exists = false;
        for (const auto& u : sl.asArray()) {
            if (u.str() == url) { exists = true; break; }
        }
        if (!exists) {
            sl.push(url);
            cfg["source_lists"] = sl;
            bool ok = aidoku_client_.saveSourceConfig(cfg, errOut);
            if (ok) aidoku_client_.restartServer();
            return ok;
        }
        return true;
    }

    bool removeSourceListUrl(const std::string& url, std::string* errOut = nullptr) {
        Json::Value cfg = aidoku_client_.getSourceConfig();
        Json::Value sl = Json::Value::array();
        if (cfg["source_lists"].isArray()) {
            for (const auto& u : cfg["source_lists"].asArray()) {
                if (u.str() != url) sl.push(u.str());
            }
        }
        cfg["source_lists"] = sl;
        bool ok = aidoku_client_.saveSourceConfig(cfg, errOut);
        if (ok) aidoku_client_.restartServer();
        return ok;
    }

    std::string getSourceDisplayName(const std::string& id) const {
        if (id == "all") return "All Sources";
        for (const auto& s : available_sources_) {
            if (s.id == id) return s.name;
        }
        return id.empty() ? "Local" : id;
    }

    std::vector<UnifiedManga> getPopularForSource(const std::string& srcId, std::string* errOut = nullptr) {
        if (srcId == "all") {
            std::vector<UnifiedManga> combined;
            std::string e1, e2, e3;
            auto md = fetchMangaDexPopular(&e1);
            auto mf = fetchMangaFoxPopular(&e2);
            auto mk = fetchMangaKatanaPopular(&e3);
            // Interleave results across sources so top rows showcase variety
            size_t maxLen = std::max({md.size(), mf.size(), mk.size()});
            for (size_t i = 0; i < maxLen; ++i) {
                if (i < md.size()) combined.push_back(md[i]);
                if (i < mf.size()) combined.push_back(mf[i]);
                if (i < mk.size()) combined.push_back(mk[i]);
            }
            for (const auto& s : available_sources_) {
                if (s.is_aidoku) {
                    auto ai = fetchAidokuPopular(s.id, nullptr);
                    combined.insert(combined.end(), ai.begin(), ai.end());
                    break; // fetchAidokuPopular("", ...) queries Aidoku once; filter per source or all
                }
            }
            if (combined.empty() && errOut) *errOut = !e1.empty() ? e1 : (!e2.empty() ? e2 : e3);
            return combined;
        }
        if (srcId == "mangafox") return fetchMangaFoxPopular(errOut);
        if (srcId == "mangakatana") return fetchMangaKatanaPopular(errOut);
        if (srcId == "mangadex") return fetchMangaDexPopular(errOut);
        return fetchAidokuPopular(srcId, errOut);
    }

    std::vector<UnifiedManga> getPopular(std::string* errOut = nullptr) {
        return getPopularForSource(active_source_id_, errOut);
    }

    std::vector<UnifiedManga> searchSource(const std::string& srcId, const std::string& query, std::string* errOut = nullptr) {
        if (srcId == "all") {
            std::vector<UnifiedManga> combined;
            std::string e1, e2, e3;
            auto md = searchMangaDex(query, &e1);
            auto mf = searchMangaFox(query, &e2);
            auto mk = searchMangaKatana(query, &e3);
            size_t maxLen = std::max({md.size(), mf.size(), mk.size()});
            for (size_t i = 0; i < maxLen; ++i) {
                if (i < md.size()) combined.push_back(md[i]);
                if (i < mf.size()) combined.push_back(mf[i]);
                if (i < mk.size()) combined.push_back(mk[i]);
            }
            bool hasAidoku = false;
            for (const auto& s : available_sources_) {
                if (s.is_aidoku) { hasAidoku = true; break; }
            }
            if (hasAidoku) {
                Json::Value items = aidoku_client_.searchMangas(query, nullptr);
                if (items.isArray()) {
                    for (const auto& m : items.asArray()) {
                        UnifiedManga um;
                        um.id = m["id"].str();
                        um.title = m["title"].str();
                        um.author = m["author"].str();
                        um.description = m["description"].str();
                        um.cover_url = m["cover_url"].str(m["cover"].str());
                        um.source_id = m["source"]["id"].str(m["source_id"].str());
                        um.status = m["status"].str();
                        if (!um.id.empty() && !um.source_id.empty()) combined.push_back(um);
                    }
                }
            }
            if (combined.empty() && errOut && !e1.empty() && !e2.empty() && !e3.empty()) {
                *errOut = e1;
            }
            return combined;
        }
        if (srcId == "mangafox") return searchMangaFox(query, errOut);
        if (srcId == "mangakatana") return searchMangaKatana(query, errOut);
        if (srcId == "mangadex") return searchMangaDex(query, errOut);
        return searchAidoku(srcId, query, errOut);
    }

    std::vector<UnifiedManga> search(const std::string& query, std::string* errOut = nullptr) {
        return searchSource(active_source_id_, query, errOut);
    }

    std::vector<UnifiedChapter> getChaptersForSource(const std::string& srcId, const std::string& mangaId, std::string* errOut = nullptr) {
        std::string resolved = (srcId.empty() || srcId == "all") ? "mangadex" : srcId;
        if (resolved == "mangafox") return fetchMangaFoxChapters(mangaId, errOut);
        if (resolved == "mangakatana") return fetchMangaKatanaChapters(mangaId, errOut);
        if (resolved == "mangadex") return fetchMangaDexChapters(mangaId, errOut);
        return fetchAidokuChapters(resolved, mangaId, errOut);
    }

    std::vector<UnifiedChapter> getChapters(const std::string& mangaId, std::string* errOut = nullptr) {
        return getChaptersForSource(active_source_id_, mangaId, errOut);
    }

    std::string downloadAidokuChapter(const std::string& sourceId,
                                      const std::string& mangaId,
                                      const std::string& chapterId,
                                      double chapterNum,
                                      bool hasChapterNum,
                                      const std::atomic<bool>* cancelFlag,
                                      const std::function<void(const std::string&)>& onStatus,
                                      std::string* errOut = nullptr) {
        return aidoku_client_.downloadChapter(sourceId, mangaId, chapterId, chapterNum, hasChapterNum, cancelFlag, onStatus, errOut);
    }

    mangadex::Client& mangadexClient() { return mangadex_client_; }

    std::vector<std::string> getChapterPageUrls(const UnifiedChapter& ch, std::string* errOut = nullptr) {
        if (ch.source_id == "mangafox") {
            return fetchMangaFoxPageUrls(ch.url.empty() ? ch.id : ch.url, errOut);
        }
        if (ch.source_id == "mangakatana") {
            return fetchMangaKatanaPageUrls(ch.url.empty() ? ch.id : ch.url, errOut);
        }
        return {};
    }

private:
    std::vector<UnifiedManga> fetchAidokuPopular(const std::string& sourceId, std::string* errOut) {
        std::vector<UnifiedManga> results;
        std::string aiErr;
        Json::Value items = aidoku_client_.searchMangas("", &aiErr);
        if (items.empty()) {
            items = aidoku_client_.searchMangas("a", &aiErr);
        }
        if (items.isArray()) {
            for (const auto& m : items.asArray()) {
                std::string sid = m["source"]["id"].str(m["source_id"].str());
                if (sid == sourceId) {
                    UnifiedManga um;
                    um.id = m["id"].str();
                    um.title = m["title"].str();
                    um.author = m["author"].str();
                    um.description = m["description"].str();
                    um.cover_url = m["cover_url"].str(m["cover"].str());
                    um.source_id = sourceId;
                    um.status = m["status"].str();
                    results.push_back(um);
                }
            }
        }
        if (results.empty() && !aiErr.empty() && errOut) {
            *errOut = aiErr;
        }
        return results;
    }

    std::vector<UnifiedManga> searchAidoku(const std::string& sourceId, const std::string& query, std::string* errOut) {
        std::vector<UnifiedManga> results;
        Json::Value items = aidoku_client_.searchMangas(query, errOut);
        if (items.isArray()) {
            for (const auto& m : items.asArray()) {
                std::string sid = m["source"]["id"].str(m["source_id"].str());
                if (sid == sourceId) {
                    UnifiedManga um;
                    um.id = m["id"].str();
                    um.title = m["title"].str();
                    um.author = m["author"].str();
                    um.description = m["description"].str();
                    um.cover_url = m["cover_url"].str(m["cover"].str());
                    um.source_id = sourceId;
                    um.status = m["status"].str();
                    results.push_back(um);
                }
            }
        }
        return results;
    }

    std::vector<UnifiedChapter> fetchAidokuChapters(const std::string& sourceId, const std::string& mangaId, std::string* errOut) {
        std::vector<UnifiedChapter> list;
        aidoku_client_.refreshChapters(sourceId, mangaId, nullptr);
        Json::Value chs = aidoku_client_.listCachedChapters(sourceId, mangaId, errOut);
        if (chs.isArray()) {
            for (const auto& item : chs.asArray()) {
                UnifiedChapter ch;
                ch.id = item["id"].str();
                if (item["chapter_num"].isNumber()) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.4g", item["chapter_num"].num());
                    ch.chapterNum = buf;
                } else {
                    ch.chapterNum = item["chapter_num"].str();
                }
                ch.title = item["title"].str();
                ch.scanlator = item["scanlator"].str();
                ch.source_id = sourceId;
                ch.url = item["url"].str();
                list.push_back(ch);
            }
        }
        return list;
    }
    std::vector<UnifiedManga> fetchMangaDexPopular(std::string* errOut) {
        std::vector<UnifiedManga> results;
        auto jsonList = mangadex_client_.queryMangaList("", "popular", errOut);
        if (jsonList.isArray()) {
            for (const auto& item : jsonList.asArray()) {
                UnifiedManga m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.cover_url = item["cover_url"].str();
                m.source_id = "mangadex";
                results.push_back(m);
            }
        }
        return results;
    }

    std::vector<UnifiedManga> searchMangaDex(const std::string& query, std::string* errOut) {
        std::vector<UnifiedManga> results;
        auto jsonList = mangadex_client_.queryMangaList(query, "search", errOut);
        if (jsonList.isArray()) {
            for (const auto& item : jsonList.asArray()) {
                UnifiedManga m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.cover_url = item["cover_url"].str();
                m.source_id = "mangadex";
                results.push_back(m);
            }
        }
        return results;
    }

    std::vector<UnifiedChapter> fetchMangaDexChapters(const std::string& mangaId, std::string* errOut) {
        std::vector<UnifiedChapter> results;
        auto jsonFeed = mangadex_client_.getChapters(mangaId, errOut);
        if (jsonFeed.isArray()) {
            for (const auto& item : jsonFeed.asArray()) {
                UnifiedChapter ch;
                ch.id = item["id"].str();
                char cbuf[32];
                std::snprintf(cbuf, sizeof(cbuf), "%.4g", item["chapter_num"].num(0.0));
                ch.chapterNum = cbuf;
                ch.title = item["title"].str();
                ch.scanlator = item["scanlator"].str();
                ch.source_id = "mangadex";
                results.push_back(ch);
            }
        }
        return results;
    }

    std::vector<UnifiedManga> fetchMangaKatanaPopular(std::string* errOut) {
        std::string html;
        if (!util::httpsGetString("https://mangakatana.com", html, 25)) {
            if (errOut) *errOut = "Failed connecting to MangaKatana";
            return {};
        }
        return parseKatanaMangaList(html);
    }

    std::vector<UnifiedManga> searchMangaKatana(const std::string& query, std::string* errOut) {
        std::string url = "https://mangakatana.com/?search=" + util::urlEncode(query);
        std::string html;
        if (!util::httpsGetString(url, html, 25)) {
            if (errOut) *errOut = "Search failed on MangaKatana";
            return {};
        }
        return parseKatanaMangaList(html);
    }

    std::vector<UnifiedManga> parseKatanaMangaList(const std::string& html) {
        std::vector<UnifiedManga> list;
        // Regex to parse MangaKatana items:
        // <a href="https://mangakatana.com/manga/(slug\.[0-9]+)"><img src="([^"]+)"
        // <h3 class="title"><a href="[^"]+">([^<]+)</a></h3>
        std::regex itemRegex(R"raw(<a href="https://mangakatana\.com/manga/([^"]+)"><img src="([^"]+)"[\s\S]*?<h3 class="title">\s*<a[^>]*>([^<]+)</a>)raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), itemRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            UnifiedManga m;
            m.id = match[1].str();
            m.cover_url = match[2].str();
            m.title = match[3].str();
            m.source_id = "mangakatana";
            m.status = "MangaKatana";
            list.push_back(m);
            if (list.size() >= 24) break;
        }
        return list;
    }

    std::vector<UnifiedChapter> fetchMangaKatanaChapters(const std::string& mangaSlug, std::string* errOut) {
        std::vector<UnifiedChapter> chapters;
        std::string url = "https://mangakatana.com/manga/" + mangaSlug;
        std::string html;
        if (!util::httpsGetString(url, html, 25)) {
            if (errOut) *errOut = "Failed loading chapters from MangaKatana";
            return {};
        }

        // Parse chapters: <a href="([^"]+/c([0-9\.]+))">Chapter ([^<]+)</a>
        std::regex chRegex(R"raw(<a href="(https://mangakatana\.com/manga/[^"]+/c([0-9\.]+))"[^>]*>([^<]+)</a>)raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), chRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            UnifiedChapter ch;
            ch.url = match[1].str();
            ch.id = match[1].str();
            ch.chapterNum = match[2].str();
            ch.title = match[3].str();
            ch.source_id = "mangakatana";
            chapters.push_back(ch);
        }
        std::reverse(chapters.begin(), chapters.end());
        return chapters;
    }

    std::vector<UnifiedManga> fetchMangaFoxPopular(std::string* errOut) {
        std::string html;
        if (!util::httpsGetString("https://fanfox.net/manga/", html, 25) &&
            !util::httpsGetString("https://www.mangahere.cc/directory/", html, 25)) {
            if (errOut) *errOut = "Failed connecting to MangaFox";
            return {};
        }
        return parseFoxMangaList(html);
    }

    std::vector<UnifiedManga> searchMangaFox(const std::string& query, std::string* errOut) {
        std::string html;
        std::string url1 = "https://fanfox.net/search?title=" + util::urlEncode(query);
        std::string url2 = "https://www.mangahere.cc/search?title=" + util::urlEncode(query);
        if (!util::httpsGetString(url1, html, 25) &&
            !util::httpsGetString(url2, html, 25)) {
            if (errOut) *errOut = "Search failed on MangaFox";
            return {};
        }
        return parseFoxMangaList(html);
    }

    std::vector<UnifiedManga> parseFoxMangaList(const std::string& html) {
        std::vector<UnifiedManga> list;
        std::unordered_set<std::string> seenIds;
        std::regex itemRegex(R"raw(<a[^>]*href="(/manga/[^"]+/)"[^>]*title="([^"]+)">[\s\S]*?<img[^>]*src="([^"]+)")raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), itemRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            std::string id = match[1].str();
            std::string img = match[3].str();

            // Skip star ratings and duplicate matches
            if (img.find("star-") != std::string::npos || img.find("logo") != std::string::npos) {
                continue;
            }
            if (seenIds.count(id)) {
                continue;
            }
            seenIds.insert(id);

            UnifiedManga m;
            m.id = id;
            m.title = match[2].str();
            if (img.rfind("//", 0) == 0) img = "https:" + img;
            m.cover_url = img;
            m.source_id = "mangafox";
            m.status = "MangaFox";
            list.push_back(m);
        }
        return list;
    }

    std::vector<UnifiedChapter> fetchMangaFoxChapters(const std::string& mangaId, std::string* errOut) {
        std::vector<UnifiedChapter> list;
        std::string slug = mangaId;
        if (slug.rfind("/manga/", 0) == 0) {
            slug = slug.substr(7);
        }
        while (!slug.empty() && slug.back() == '/') slug.pop_back();

        std::string html;
        std::string url1 = "https://fanfox.net/manga/" + slug + "/";
        std::string url2 = "https://www.mangahere.cc/manga/" + slug + "/";
        if (!util::httpsGetString(url1, html, 25) && !util::httpsGetString(url2, html, 25)) {
            if (errOut) *errOut = "Failed loading chapters from MangaFox";
            return {};
        }

        std::regex chRegex(R"raw(<a[^>]*href="(/manga/[^/]+/(?:v[0-9]+/)?c([0-9.]+)/1\.html)"[^>]*title="([^"]+)")raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), chRegex);
        auto words_end = std::sregex_iterator();

        std::set<std::string> seenUrls;
        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            std::string link = match[1].str();
            if (!slug.empty() && link.find(slug) == std::string::npos) continue;
            if (seenUrls.find(link) != seenUrls.end()) continue;
            seenUrls.insert(link);

            UnifiedChapter ch;
            ch.url = link;
            ch.id = link;
            ch.chapterNum = match[2].str();
            ch.title = match[3].str();
            ch.source_id = "mangafox";
            list.push_back(ch);
        }
        return list;
    }

    static std::string unpackDeanEdwards(const std::string& script) {
        size_t start = script.find("}('");
        if (start == std::string::npos) return "";
        start += 3;

        std::string tail = script.substr(start);
        std::regex paramRegex(R"raw(,(\d+),(\d+),'([^']+)'\.split\('\|'\))raw");
        std::smatch match;
        if (!std::regex_search(tail, match, paramRegex)) {
            return "";
        }

        std::string templateStr = tail.substr(0, match.position(0));
        std::string wordsStr = match[3].str();

        std::vector<std::string> dict;
        std::stringstream ss(wordsStr);
        std::string item;
        while (std::getline(ss, item, '|')) {
            dict.push_back(item);
        }

        auto decodeBase62 = [](const std::string& token) -> int {
            int val = 0;
            for (char c : token) {
                val *= 62;
                if (c >= '0' && c <= '9') val += (c - '0');
                else if (c >= 'a' && c <= 'z') val += (c - 'a' + 10);
                else if (c >= 'A' && c <= 'Z') val += (c - 'A' + 36);
                else return -1;
            }
            return val;
        };

        std::string result;
        result.reserve(templateStr.size() * 3);
        size_t i = 0;
        while (i < templateStr.size()) {
            if (std::isalnum(static_cast<unsigned char>(templateStr[i]))) {
                size_t tokenStart = i;
                while (i < templateStr.size() && std::isalnum(static_cast<unsigned char>(templateStr[i]))) {
                    i++;
                }
                std::string token = templateStr.substr(tokenStart, i - tokenStart);
                int idx = decodeBase62(token);
                if (idx >= 0 && idx < static_cast<int>(dict.size()) && !dict[idx].empty()) {
                    result += dict[idx];
                } else {
                    result += token;
                }
            } else {
                result += templateStr[i++];
            }
        }
        return result;
    }

    std::vector<std::string> fetchMangaFoxPageUrls(const std::string& chapterUrl, std::string* errOut) {
        std::string html;
        std::string fullUrl = chapterUrl;
        if (fullUrl.rfind("http", 0) != 0) {
            fullUrl = "https://fanfox.net" + chapterUrl;
        }

        std::string primaryUrl = fullUrl;
        std::string primaryError;
        if (!util::httpsGetString(primaryUrl, html, 25, &primaryError)) {
            std::string fallbackUrl = primaryUrl;
            if (primaryUrl.find("fanfox.net") != std::string::npos) {
                size_t pos = fallbackUrl.find("fanfox.net");
                fallbackUrl.replace(pos, 10, "www.mangahere.cc");
            } else if (primaryUrl.find("mangahere.cc") != std::string::npos) {
                size_t pos = fallbackUrl.find("www.mangahere.cc");
                if (pos != std::string::npos) {
                    fallbackUrl.replace(pos, 16, "fanfox.net");
                } else {
                    pos = fallbackUrl.find("mangahere.cc");
                    if (pos != std::string::npos) fallbackUrl.replace(pos, 12, "fanfox.net");
                }
            }
            std::string fallbackError;
            if (util::httpsGetString(fallbackUrl, html, 25, &fallbackError)) {
                fullUrl = fallbackUrl;
            } else if (errOut) {
                *errOut = "Could not load MangaFox chapter HTML (" +
                          (primaryError.empty() ? "request failed" : primaryError) +
                          "; fallback: " +
                          (fallbackError.empty() ? "request failed" : fallbackError) + ")";
            }
        }

        if (html.empty()) {
            if (errOut && errOut->empty()) {
                *errOut = "Could not load MangaFox chapter HTML (" + primaryUrl + ")";
            }
            return {};
        }

        size_t evalPos = html.find("eval(function(p,a,c,k,e,d)");
        if (evalPos == std::string::npos) {
            evalPos = html.find("eval(function(p,a,c,k,e,r)");
        }
        if (evalPos == std::string::npos) {
            if (errOut) *errOut = "MangaFox chapter script not found";
            return {};
        }

        size_t evalEnd = html.find("</script>", evalPos);
        std::string script = html.substr(evalPos, (evalEnd != std::string::npos) ? (evalEnd - evalPos) : 2000);
        std::string unpacked = unpackDeanEdwards(script);

        std::vector<std::string> pages;

        // 1. Try extracting direct image URLs (matching images with optional tokens/queries)
        std::regex urlRegex(R"raw((?:https?:)?//[^\s'",;]+\.(?:jpg|jpeg|png|webp)(?:\?[^\s'",;]*)?)raw");
        auto begin = std::sregex_iterator(unpacked.begin(), unpacked.end(), urlRegex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string u = it->str();
            if (u.rfind("//", 0) == 0) u = "https:" + u;
            if (pages.empty() || pages.back() != u) {
                pages.push_back(u);
            }
        }

        // 2. If no direct page URLs in the script, check for DM5 chapterfun.ashx mechanism
        if (pages.empty()) {
            std::smatch cidMatch, countMatch, guidMatch;
            std::regex cidRegex(R"raw(var\s+chapterid\s*=\s*(\d+);)raw");
            std::regex countRegex(R"raw(var\s+imagecount\s*=\s*(\d+);)raw");
            std::regex guidRegex(R"raw(guidkey\s*=\s*([^;]+);)raw");

            std::string cidStr, guidStr;
            int totalPages = 0;

            if (std::regex_search(html, cidMatch, cidRegex)) {
                cidStr = cidMatch[1].str();
            }
            if (std::regex_search(html, countMatch, countRegex)) {
                totalPages = std::atoi(countMatch[1].str().c_str());
            }
            if (std::regex_search(unpacked, guidMatch, guidRegex)) {
                std::string rawGuid = guidMatch[1].str();
                for (char c : rawGuid) {
                    if (std::isalnum(static_cast<unsigned char>(c))) {
                        guidStr += c;
                    }
                }
            }

            if (!cidStr.empty() && totalPages > 0) {
                std::string baseDir = fullUrl;
                size_t lastSlash = baseDir.rfind('/');
                if (lastSlash != std::string::npos) {
                    baseDir = baseDir.substr(0, lastSlash + 1);
                } else {
                    baseDir += "/";
                }

                // Batch fetch chapterfun.ashx (each call typically yields 2 pages)
                for (int p = 1; p <= totalPages; p += 2) {
                    std::string funUrl = baseDir + "chapterfun.ashx?cid=" + cidStr +
                                         "&page=" + std::to_string(p) +
                                         "&key=" + guidStr;
                    std::string funResp;
                    if (!util::httpsGetString(funUrl, funResp, 15)) {
                        break;
                    }

                    std::string funUnpacked = unpackDeanEdwards(funResp);
                    if (funUnpacked.empty()) funUnpacked = funResp;

                    std::smatch pixMatch, pvalMatch;
                    std::regex pixRegex(R"raw(var\s+pix\s*=\s*["']([^"']+)["'])raw");
                    std::regex pvalRegex(R"raw(var\s+pvalue\s*=\s*\[([^\]]+)\])raw");

                    std::string pix = "";
                    if (std::regex_search(funUnpacked, pixMatch, pixRegex)) {
                        pix = pixMatch[1].str();
                        if (pix.rfind("//", 0) == 0) pix = "https:" + pix;
                    }

                    if (std::regex_search(funUnpacked, pvalMatch, pvalRegex)) {
                        std::string rawList = pvalMatch[1].str();
                        std::stringstream ss(rawList);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            std::string cleaned;
                            for (char c : item) {
                                if (c != '"' && c != '\'' && c != ' ' && c != '\r' && c != '\n') {
                                    cleaned += c;
                                }
                            }
                            if (!cleaned.empty()) {
                                std::string fullImg = cleaned;
                                if (fullImg.rfind("http", 0) != 0 && fullImg.rfind("//", 0) != 0) {
                                    fullImg = pix + cleaned;
                                } else if (fullImg.rfind("//", 0) == 0) {
                                    fullImg = "https:" + fullImg;
                                }
                                if (pages.empty() || pages.back() != fullImg) {
                                    pages.push_back(fullImg);
                                }
                            }
                        }
                    } else {
                        auto bit = std::sregex_iterator(funUnpacked.begin(), funUnpacked.end(), urlRegex);
                        for (; bit != end; ++bit) {
                            std::string u = bit->str();
                            if (u.rfind("//", 0) == 0) u = "https:" + u;
                            if (pages.empty() || pages.back() != u) {
                                pages.push_back(u);
                            }
                        }
                    }

                    if (static_cast<int>(pages.size()) >= totalPages) {
                        break;
                    }
                }
            }
        }

        if (pages.empty() && errOut) {
            *errOut = "Failed to parse MangaFox page URLs";
        }
        return pages;
    }

    std::vector<std::string> fetchMangaKatanaPageUrls(const std::string& chapterUrl, std::string* errOut) {
        std::string html;
        std::string fullUrl = chapterUrl;
        if (fullUrl.rfind("http", 0) != 0) {
            fullUrl = "https://mangakatana.com" + chapterUrl;
        }

        if (!util::httpsGetString(fullUrl, html, 25)) {
            if (errOut) *errOut = "Could not load MangaKatana chapter HTML";
            return {};
        }

        std::vector<std::string> pages;
        size_t thzPos = html.find("var thzq=[");
        if (thzPos != std::string::npos) {
            size_t thzEnd = html.find("];", thzPos);
            std::string slice = html.substr(thzPos, (thzEnd != std::string::npos) ? (thzEnd - thzPos + 2) : 2000);
            std::regex urlRegex(R"raw(https?://[^\s'",;]+\.(?:jpg|jpeg|png|webp))raw");
            auto begin = std::sregex_iterator(slice.begin(), slice.end(), urlRegex);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                pages.push_back(it->str());
            }
        }

        if (pages.empty() && errOut) {
            *errOut = "Failed to parse MangaKatana page URLs";
        }
        return pages;
    }
};

} // namespace kindle
