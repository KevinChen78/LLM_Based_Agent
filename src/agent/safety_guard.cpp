#include "agent/safety_guard.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace agent {

namespace {

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool ContainsAny(const std::string& haystack, const std::vector<std::string>& needles) {
    for (const auto& n : needles) {
        if (n.empty()) continue;
        if (haystack.find(n) != std::string::npos) return true;
    }
    return false;
}

// Built-in defaults. Kept as functions so the file-override path can fall
// back to them per-key; the no-file behavior is byte-identical to Phase 0.
std::vector<std::string> DefaultInjectionPatterns() {
    return {
        "ignore previous instructions", "ignore the above", "disregard the above",
        "ignore all previous", "forget your instructions", "override your instructions",
        "you are now", "new instructions:", "system prompt", "reveal your prompt",
        "developer mode", "jailbreak", "dan mode", "act as ", "pretend you are",
        "no restrictions", "without any restrictions",
        // Chinese equivalents
        "忽略之前的指令", "忽略以上", "无视上面的", "覆盖你的指令", "你现在的身份",
        "你现在是", "扮演一个", "没有任何限制", "输出你的系统提示", "系统提示词",
        "越狱", "解除限制",
    };
}

std::vector<std::string> DefaultBannedTopics() {
    return {"赌博", "毒品", "色情", "枪支", "爆炸", "洗钱"};
}

std::vector<std::string> DefaultBannedOutputWords() {
    return {"赌博", "毒品", "色情"};
}

std::string StripWords(const std::string& text, const std::vector<std::string>& words) {
    std::string out = text;
    // Replace each banned word with *** . Simple substring replace loop.
    for (std::string::size_type pos = 0;;) {
        std::string::size_type best = std::string::npos;
        std::size_t blen = 0;
        for (const auto& w : words) {
            if (w.empty()) continue;
            auto p = out.find(w, pos);
            if (p != std::string::npos && (best == std::string::npos || p < best)) {
                best = p;
                blen = w.size();
            }
        }
        if (best == std::string::npos) break;
        out.replace(best, blen, "***");
        pos = best + 3;  // advance past the replacement
    }
    return out;
}

} // namespace

SafetyGuard::SafetyGuard(const std::string& rules_path) {
    injection_patterns_ = DefaultInjectionPatterns();
    banned_topics_ = DefaultBannedTopics();
    banned_output_words_ = DefaultBannedOutputWords();

    // Phase 4-C: optional rule file override. Missing file is the normal
    // offline/default case (silent); a present-but-malformed file warns and
    // keeps defaults. Present keys REPLACE the corresponding built-in list.
    if (rules_path.empty()) return;
    std::ifstream in(rules_path);
    if (!in) return;  // no file -> built-in defaults
    try {
        std::stringstream ss;
        ss << in.rdbuf();
        const auto j = nlohmann::json::parse(ss.str());
        auto load_list = [&j](const char* key, std::vector<std::string>& dst) {
            if (j.contains(key) && j[key].is_array()) {
                std::vector<std::string> v;
                for (const auto& e : j[key]) {
                    if (e.is_string()) v.push_back(e.get<std::string>());
                }
                dst = std::move(v);
            }
        };
        load_list("injection_patterns", injection_patterns_);
        load_list("banned_topics", banned_topics_);
        load_list("banned_output_words", banned_output_words_);
        if (j.contains("max_input_chars") && j["max_input_chars"].is_number_unsigned()) {
            max_input_chars = j["max_input_chars"].get<std::size_t>();
        }
        spdlog::info("SafetyGuard: loaded rule overrides from {}", rules_path);
    } catch (const std::exception& e) {
        injection_patterns_ = DefaultInjectionPatterns();
        banned_topics_ = DefaultBannedTopics();
        banned_output_words_ = DefaultBannedOutputWords();
        spdlog::warn("SafetyGuard: rules file {} malformed ({}), using built-in defaults.",
                     rules_path, e.what());
    }
}

InputGuardResult SafetyGuard::CheckInput(const std::string& user_message) const {
    InputGuardResult r;

    if (user_message.size() > max_input_chars) {
        r.is_safe = false;
        r.risk_type = "too_long";
        r.reason = "input exceeds max_input_chars=" + std::to_string(max_input_chars);
        r.refusal_reply = "您的消息有点长，能不能精简一下您的需求呢？例如城市、人数和预算。";
        return r;
    }

    const std::string lowered = ToLowerAscii(user_message);
    if (ContainsAny(lowered, injection_patterns_)) {
        r.is_safe = false;
        r.risk_type = "prompt_injection";
        r.reason = "matched injection pattern";
        r.refusal_reply = "抱歉，我只能帮您推荐团购套餐，无法处理这类请求。"
                          "您可以告诉我城市、用餐人数和预算。";
        return r;
    }

    if (ContainsAny(user_message, banned_topics_)) {
        r.is_safe = false;
        r.risk_type = "banned_topic";
        r.reason = "matched banned topic";
        r.refusal_reply = "抱歉，这类内容我无法提供帮助。"
                          "我可以为您推荐餐饮团购，请告诉我城市、人数和预算。";
        return r;
    }

    return r;  // safe
}

std::string SafetyGuard::MaskPii(const std::string& text) {
    std::string out = text;

    // Order matters: mask the longest digit runs first so the 11-digit mobile
    // regex does not match a substring of an 18-digit ID or 16-19 digit card.
    //
    // 18-digit Chinese resident ID (last char may be X/x).
    out = std::regex_replace(out, std::regex(R"(\d{17}[\dXx])"), "[身份证已隐藏]");

    // Long digit runs (16-19): bank-card-like numbers.
    out = std::regex_replace(out, std::regex(R"(\d{16,19})"), "[数字已隐藏]");

    // Chinese mobile: 1[3-9] followed by 9 digits.
    out = std::regex_replace(out, std::regex(R"(1[3-9]\d{9})"), "[手机已隐藏]");

    // Email addresses.
    out = std::regex_replace(out,
        std::regex(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"),
        "[邮箱已隐藏]");

    return out;
}

std::string SafetyGuard::StripBannedWords(const std::string& text) {
    return StripWords(text, DefaultBannedOutputWords());
}

std::string SafetyGuard::SanitizeOutputText(const std::string& text) const {
    return StripWords(MaskPii(text), banned_output_words_);
}

void SafetyGuard::SanitizeItems(std::vector<RecommendationItem>& items) const {
    for (auto& item : items) {
        item.title = SanitizeOutputText(item.title);
        item.reason = SanitizeOutputText(item.reason);
        for (auto& tag : item.tags) {
            tag = SanitizeOutputText(tag);
        }
    }
}

namespace {

bool Nearly(double a, double b, double eps) { return std::abs(a - b) <= eps; }

// A money claim (¥xx / xx元) is legit when it matches, for some candidate
// item: the exact price or original_price; or a derived figure — per-person
// price/p or total price*p for p within the item's own [min_people,
// max_people] span (capped at 1..20). Derived whitelist exists so honest
// phrasing like 人均96元 / 四人共1152元 is not killed.
bool MoneyAllowed(double v, const std::vector<RecommendationItem>& items) {
    for (const auto& it : items) {
        if (Nearly(v, it.price, 0.01) ||
            (it.original_price > 0 && Nearly(v, it.original_price, 0.01))) {
            return true;
        }
        int lo = it.min_people > 0 ? it.min_people : 1;
        int hi = it.max_people > 0 ? it.max_people : 0;
        if (hi < lo) continue;
        hi = std::min(hi, 20);
        for (int p = lo; p <= hi; ++p) {
            const double total = it.price * p;
            if (Nearly(v, total, 0.51)) return true;
            const double per = it.price / p;
            // 人均 claims round to an integer in either direction.
            if (Nearly(v, std::floor(per), 0.51) ||
                Nearly(v, std::ceil(per), 0.51)) {
                return true;
            }
        }
    }
    return false;
}

// A discount claim (xx折) is legit when some item's actual discount
// price/original_price*10 rounds to it (±0.5折 tolerance for phrasing).
bool DiscountAllowed(double zhe, const std::vector<RecommendationItem>& items) {
    for (const auto& it : items) {
        if (it.original_price <= 0) continue;
        const double actual = it.price * 10.0 / it.original_price;
        if (Nearly(zhe, actual, 0.51)) return true;
        // Or the claim derives a listed price: price == original * zhe / 10.
        if (Nearly(it.price, it.original_price * zhe / 10.0, 0.51)) return true;
    }
    return false;
}

} // namespace

FactCheckResult SafetyGuard::FactCheckReply(
    const std::string& reply,
    const std::vector<RecommendationItem>& items) const {
    FactCheckResult r;
    if (reply.empty()) return r;

    auto add_violation = [&r](const std::string& what) {
        r.ok = false;
        r.violations.push_back(what);
    };

    // ¥xx / xx元 — money claims. Matched on UTF-8 bytes (source is /utf-8).
    static const std::regex kMoney(R"((?:¥\s*(\d+(?:\.\d+)?))|(?:(\d+(?:\.\d+)?)\s*元))");
    for (std::sregex_iterator m(reply.begin(), reply.end(), kMoney), end;
         m != end; ++m) {
        const std::string num = (*m)[1].matched ? (*m)[1].str() : (*m)[2].str();
        const double v = std::stod(num);
        if (!MoneyAllowed(v, items)) {
            add_violation("money claim " + num + " not in candidate "
                          "price/original_price/derived set");
        }
    }

    // xx折 — discount claims.
    static const std::regex kDiscount(R"((\d+(?:\.\d+)?)\s*折)");
    for (std::sregex_iterator m(reply.begin(), reply.end(), kDiscount), end;
         m != end; ++m) {
        const double zhe = std::stod((*m)[1].str());
        if (!DiscountAllowed(zhe, items)) {
            add_violation("discount claim " + (*m)[1].str() +
                          "折 not derivable from candidate prices");
        }
    }

    return r;
}

} // namespace agent
