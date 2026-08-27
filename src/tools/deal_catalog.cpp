#include "agent/deal_catalog.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#ifdef AGENT_HAVE_LIBPQ
#include <libpq-fe.h>
#endif

namespace agent {

DealCatalog::DealCatalog(const std::string& json_path, const std::string& pg_dsn) {
    if (!pg_dsn.empty()) {
        if (LoadFromPostgres(pg_dsn)) {
            source_ = "postgres";
        } else {
            spdlog::warn("DealCatalog: PostgreSQL load failed; falling back to file '{}'",
                         json_path.empty() ? "<empty>" : json_path);
        }
    }
    if (!loaded_) {
        LoadFromFile(json_path);
        if (loaded_) source_ = "file:" + json_path;
    }
    if (!loaded_) {
        spdlog::warn("DealCatalog: falling back to built-in dataset (path='{}' not usable)",
                     json_path.empty() ? "<empty>" : json_path);
        deals_ = BuiltInFallback();
        loaded_ = true;
        source_ = "builtin";
    }
    spdlog::info("DealCatalog: loaded {} deals (source={})", Size(), source_);
}

bool DealCatalog::LoadFromPostgres(const std::string& dsn) {
#ifdef AGENT_HAVE_LIBPQ
    PGconn* conn = PQconnectdb(dsn.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        spdlog::warn("DealCatalog: PQconnectdb failed: {}", PQerrorMessage(conn));
        PQfinish(conn);
        return false;
    }
    // Same 14 columns + ORDER BY item_id as the Python service's pg_load_deals,
    // so both control planes see identical rows in identical order.
    PGresult* res = PQexec(conn,
        "SELECT item_id, merchant_id, title, category, city, district,"
        " price, original_price, sold_count, rating,"
        " min_people, max_people, tags, description"
        " FROM groupbuy_items ORDER BY item_id");
    bool ok = false;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        nlohmann::json arr = nlohmann::json::array();
        const int n = PQntuples(res);
        for (int i = 0; i < n; ++i) {
            auto text = [&](int col) { return PQgetvalue(res, i, col); };
            nlohmann::json d;
            d["item_id"] = text(0);
            d["merchant_id"] = text(1);
            d["title"] = text(2);
            d["category"] = text(3);
            d["city"] = text(4);
            d["district"] = PQgetisnull(res, i, 5) ? nlohmann::json(nullptr)
                                                   : nlohmann::json(text(5));
            d["price"] = std::strtod(text(6), nullptr);
            d["original_price"] = PQgetisnull(res, i, 7)
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(std::strtod(text(7), nullptr));
            d["sold_count"] = std::strtoll(text(8), nullptr, 10);
            d["rating"] = std::strtod(text(9), nullptr);
            d["min_people"] = std::strtoll(text(10), nullptr, 10);
            d["max_people"] = std::strtoll(text(11), nullptr, 10);
            d["tags"] = nlohmann::json::parse(text(12), nullptr, /*allow_exceptions=*/false);
            if (d["tags"].is_discarded()) d["tags"] = nlohmann::json::array();
            d["description"] = text(13);
            arr.push_back(std::move(d));
        }
        // An empty table is more likely a seeding mistake than a real catalog —
        // fall through to the JSON file instead of serving nothing.
        if (!arr.empty()) {
            deals_ = std::move(arr);
            loaded_ = true;
            ok = true;
        } else {
            spdlog::warn("DealCatalog: groupbuy_items is empty; treating as failure");
        }
    } else {
        spdlog::warn("DealCatalog: PG query failed: {}", PQerrorMessage(conn));
    }
    PQclear(res);
    PQfinish(conn);
    return ok;
#else
    (void)dsn;
    spdlog::warn("DealCatalog: built without libpq (ENABLE_PG_CATALOG=OFF or PG not"
                 " found); ignoring PG_DSN");
    return false;
#endif
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

std::vector<std::string> DealCatalog::DistinctCategories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> cats;
    for (const auto& d : deals_) {
        if (d.contains("category") && d["category"].is_string()) {
            const std::string c = d["category"].get<std::string>();
            if (!c.empty()) cats.insert(c);
        }
    }
    return {cats.begin(), cats.end()};
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
