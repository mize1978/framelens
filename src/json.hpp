// json.hpp — house.json を読むための自作最小 JSON パーサ。
// 数値・配列・オブジェクトの必要最小限のみ対応。外部ライブラリなし。
#pragma once
#include <string>
#include <vector>

namespace fl {

// 空白を読み飛ばす
inline size_t jsSkipWS(const std::string& s, size_t p) {
    while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) ++p;
    return p;
}

// "key": <数値> を取り出す。見つからなければ def を返す。
inline double jsNumber(const std::string& s, const char* key, double def = -1.0) {
    std::string k = std::string("\"") + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return def;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return def;
    p = jsSkipWS(s, p + 1);
    try { return std::stod(s.substr(p)); } catch (...) { return def; }
}

// "key": "文字列" を取り出す。見つからなければ def を返す。
inline std::string jsString(const std::string& s, const char* key,
                            const char* def = "") {
    std::string k = std::string("\"") + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return def;
    auto q = s.find('"', p + k.size());
    if (q == std::string::npos) return def;
    q = s.find('"', q + 1);                 // skip ':'とスペースの先の '"'
    if (q == std::string::npos) return def;
    auto e = s.find('"', q + 1);
    if (e == std::string::npos) return def;
    return s.substr(q + 1, e - q - 1);
}

// "key": [{...},{...},...] の各オブジェクトを文字列スライスで返す。
inline std::vector<std::string> jsArrayObjs(const std::string& s, const char* key) {
    std::vector<std::string> result;
    std::string k = std::string("\"") + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return result;
    p = s.find('[', p + k.size());
    if (p == std::string::npos) return result;
    ++p;
    int depth = 0;
    size_t objStart = std::string::npos;
    for (; p < s.size(); ++p) {
        if (s[p] == '{') {
            if (depth == 0) objStart = p;
            ++depth;
        } else if (s[p] == '}') {
            --depth;
            if (depth == 0 && objStart != std::string::npos) {
                result.push_back(s.substr(objStart, p - objStart + 1));
                objStart = std::string::npos;
            }
        } else if (s[p] == ']' && depth == 0) break;
    }
    return result;
}

} // namespace fl
