#include "agent/deal_catalog.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace agent {

DealCatalog::DealCatalog(const std::string& json_path) {
    LoadFromFile(json_path);
    if (!loaded_) {
        spdlog::warn("DealCatalog: falling back to built-in dataset (path='{}' not usable)",
                     json_path.empty() ? "<empty>" : json_path);
        deals_ = BuiltInFallback();
        loaded_ = true;
    }
    spdlog::info("DealCatalog: loaded {} deals (source={})",
                 Size(), loaded_ ? (deals_.empty() ? "empty" : "ok") : "fallback");
}

void DealCatalog::LoadFromFile(const std::string& path) {
    if (path.empty()) return;
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    try {
        auto root = nlohmann::json::parse(ss.str());
        if (root.contains("deals") && root["deals"].is_array()) {
            deals_ = root["deals"];
            loaded_ = true;
        } else if (root.is_array()) {
            // Tolerate a bare array of deals.
            deals_ = root;
            loaded_ = true;
        }
    } catch (const std::exception& e) {
        spdlog::warn("DealCatalog: failed to parse '{}': {}", path, e.what());
    }
}

nlohmann::json DealCatalog::Deals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deals_;   // deep copy
}

size_t DealCatalog::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deals_.size();
}

nlohmann::json DealCatalog::BuiltInFallback() {
    // Compact embedded dataset used when data/deals.json is unavailable
    // (e.g. running unit tests from the build directory). Mirrors a subset of
    // the file-based catalog so filtering/ranking remain exercisable offline.
    static const char* kFallback = R"({
      "deals": [
        {"item_id":"gb-20001","merchant_id":"m-30001","title":"海鲜大咖套餐（3-4 人）","category":"海鲜","city":"上海","district":"黄浦","price":288.0,"original_price":598.0,"sold_count":1200,"rating":4.7,"min_people":3,"max_people":4,"tags":["大虾","生蚝","扇贝","鱿鱼","海鲜"],"description":"含 8 种海鲜，分量足，人均不到 100 元"},
        {"item_id":"gb-20002","merchant_id":"m-30002","title":"精品海鲜双人餐","category":"海鲜","city":"上海","district":"静安","price":198.0,"original_price":398.0,"sold_count":800,"rating":4.5,"min_people":2,"max_people":2,"tags":["清蒸鲈鱼","蒜蓉扇贝","海鲜"],"description":"主打清蒸海鲜，口味清淡"},
        {"item_id":"gb-20003","merchant_id":"m-30003","title":"波士顿龙虾三人餐","category":"海鲜","city":"上海","district":"浦东","price":298.0,"original_price":688.0,"sold_count":500,"rating":4.8,"min_people":3,"max_people":3,"tags":["波士顿龙虾","海鲜拼盘","龙虾"],"description":"龙虾新鲜，适合三人聚餐"},
        {"item_id":"gb-20005","merchant_id":"m-30005","title":"老北京铜锅涮肉 4 人餐","category":"火锅","city":"北京","district":"朝阳","price":268.0,"original_price":528.0,"sold_count":900,"rating":4.6,"min_people":3,"max_people":4,"tags":["羊肉","铜锅","火锅"],"description":"地道老北京铜锅，适合冬季聚餐"},
        {"item_id":"gb-20006","merchant_id":"m-30006","title":"重庆老火锅 3 人餐（微辣可选）","category":"火锅","city":"北京","district":"海淀","price":228.0,"original_price":458.0,"sold_count":1100,"rating":4.4,"min_people":2,"max_people":3,"tags":["牛油","毛肚","火锅","微辣"],"description":"正宗牛油锅底，可调微辣"},
        {"item_id":"gb-20011","merchant_id":"m-30011","title":"深夜烧烤大拼盘（6 人）","category":"烧烤","city":"广州","district":"天河","price":328.0,"original_price":588.0,"sold_count":1300,"rating":4.4,"min_people":4,"max_people":6,"tags":["羊肉串","烤翅","烧烤","夜宵"],"description":"深夜烧烤大拼盘，朋友撸串首选"},
        {"item_id":"gb-20013","merchant_id":"m-30013","title":"正宗川菜水煮鱼 4 人餐","category":"川菜","city":"成都","district":"锦江","price":238.0,"original_price":428.0,"sold_count":880,"rating":4.6,"min_people":3,"max_people":4,"tags":["水煮鱼","川菜","辣"],"description":"正宗川味水煮鱼，嗜辣者必点"},
        {"item_id":"gb-20016","merchant_id":"m-30016","title":"粤式早茶全家福（6 人）","category":"中餐","city":"广州","district":"越秀","price":298.0,"original_price":528.0,"sold_count":1500,"rating":4.7,"min_people":4,"max_people":6,"tags":["虾饺","烧麦","早茶","粤菜","中餐"],"description":"经典粤式早茶拼盘，家庭聚会推荐"}
      ]
    })";
    try {
        auto root = nlohmann::json::parse(kFallback);
        return root.value("deals", nlohmann::json::array());
    } catch (const std::exception&) {
        return nlohmann::json::array();
    }
}

} // namespace agent
