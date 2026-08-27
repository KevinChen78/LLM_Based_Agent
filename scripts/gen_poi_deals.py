#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate group-buying deals from real AMap POI data — Phase 7-B.

Scope (user directive 2026-08-27, quota-driven): only 深圳市南山区 gets real
merchant data; the synthetic Nanshan deals were removed from
gen_city_deals.py at the same time.

Inputs:
  - data/pois_南山区.json   (place/around circle search, district-filtered)
  - data/pois_raw.json      (4-city text search; 深圳+南山区 rows merged in)
Both are produced by scripts/fetch_pois.py (gitignored data/, not committed).

Rules:
  - Category mapping: AMap typecode leaf (third level) -> catalog category,
    explicit dict below. Unmapped leaves are SKIPPED and counted — never
    force-fitted. Non-food POIs (market/beauty/sports) skipped too.
  - deal attributes are deterministic: all variation derives from
    md5(poi_id) (Python's built-in hash() is process-randomized — do not
    use). rating prefers the POI's real AMap rating when present.
  - id isolation: item_id gb-4xxxxx / merchant_id m-4xxxxx (no clash with
    2xxxx seed+Wuhan or 26-28xxx synthetic city blocks).
  - Idempotent: re-running removes only gb-4xxxx records and regenerates.
  - district is normalized to the catalog convention "南山" (no 区 suffix)
    so retrieval district filters keep working.

Run from the project root:
    python scripts/gen_poi_deals.py
"""
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DATA_PATH = os.path.join(PROJECT, "data", "deals.json")
POI_FILES = ["pois_南山区.json", "pois_raw.json"]

ITEM_BASE = 40001          # gb-40001..
MERCHANT_BASE = 40001      # m-40001..
TARGET_CITY = "深圳"
TARGET_ADNAME = "南山区"
DISTRICT = "南山"          # catalog convention: no 区 suffix

# AMap typecode leaf (segment after last ';') -> catalog category.
# Explicit allowlist; anything not listed here is skipped and counted.
TYPECODE_MAP = {
    "中餐厅": "中餐",
    "综合酒楼": "中餐",
    "特色/地方风味餐厅": "中餐",
    "西北菜": "中餐",
    "清真菜馆": "中餐",
    "火锅店": "火锅",
    "湖南菜(湘菜)": "湘菜",
    "四川菜(川菜)": "川菜",
    "东北菜": "东北菜",
    "湖北菜": "湖北菜",
    "广东菜(粤菜)": "粤菜",
    "潮州菜": "粤菜",
    "上海菜": "本帮菜",
    "北京菜": "北京菜",
    "云南菜": "云南菜",
    "台湾菜": "台湾菜",
    "日本料理": "日料",
    "吉野家": "日料",
    "韩国料理": "烤肉",
    "西餐厅(综合风味)": "西餐",
    "美式风味": "西餐",
    "意式菜品餐厅": "西餐",
    "麦当劳": "西餐",        # 西式快餐(与 Phase 3-B 西餐/汉堡 同口径)
    "肯德基": "西餐",
    "海鲜酒楼": "海鲜",
    "小龙虾": "小龙虾",
    "烧烤店": "烧烤",
    "快餐厅": "小吃",
    "冷饮店": "甜品",
    "糕饼店": "甜品",
    "咖啡厅": "甜品",        # 咖啡/下午茶就近并入甜品
    "星巴克咖啡": "甜品",
    "茶艺馆": "甜品",
    "自助餐": "自助餐",
    "素食": "素食",
}

# Per-category price anchor (matches the synthetic generators' price bands).
CATEGORY_PRICE = {
    "中餐": 158, "火锅": 228, "湘菜": 168, "川菜": 148, "东北菜": 168,
    "湖北菜": 148, "粤菜": 198, "本帮菜": 208, "北京菜": 228,
    "云南菜": 98, "台湾菜": 138, "日料": 288, "西餐": 168, "烤肉": 198,
    "海鲜": 298, "小龙虾": 178, "烧烤": 158, "小吃": 58, "甜品": 48,
    "自助餐": 288, "素食": 138,
}


def stable_int(poi_id):
    """Deterministic per-POI variation source (md5; built-in hash() of str
    is randomized per process and would break byte-identical reruns)."""
    return int(hashlib.md5(poi_id.encode("utf-8")).hexdigest()[:12], 16)


def map_category(typecode):
    """First '|' segment whose leaf maps wins; None -> caller skips."""
    for seg in (typecode or "").split("|"):
        leaf = seg.split(";")[-1].strip()
        if leaf in TYPECODE_MAP:
            return TYPECODE_MAP[leaf], leaf
    leafs = [s.split(";")[-1].strip() for s in (typecode or "").split("|")]
    return None, "|".join(leafs)


def load_pois():
    pois = {}
    for name in POI_FILES:
        path = os.path.join(PROJECT, "data", name)
        try:
            with open(path, encoding="utf-8") as f:
                payload = json.load(f)
        except FileNotFoundError:
            continue
        for p in payload.get("pois", []):
            if p.get("adname") == TARGET_ADNAME and p.get("poi_id"):
                pois[p["poi_id"]] = p   # first file wins on dup ids
    return [pois[k] for k in sorted(pois)]


def gen():
    with open(DATA_PATH, "r", encoding="utf-8") as f:
        root = json.load(f)
    deals = root.get("deals", [])

    # Drop only this script's own id block.
    def is_generated(d):
        iid = d.get("item_id", "")
        if not iid.startswith("gb-"):
            return False
        try:
            n = int(iid[3:])
        except ValueError:
            return False
        return ITEM_BASE <= n < ITEM_BASE + 100000

    deals = [d for d in deals if not is_generated(d)]

    # User directive 2026-08-27: 南山区由真实 POI 数据独家承接 —— 清除任何
    # 残留合成南山行(例如种子块 gb-20017;gen_city_deals.py 已不再生成南山,
    # 这里是兜底自清,幂等)。
    deals = [d for d in deals
             if not (d.get("city") == TARGET_CITY
                     and d.get("district") == DISTRICT)]

    pois = load_pois()
    mapped = skipped = 0
    skipped_leaves = {}
    for i, p in enumerate(pois):
        category, leaf = map_category(p.get("typecode", ""))
        if category is None:
            skipped += 1
            skipped_leaves[leaf] = skipped_leaves.get(leaf, 0) + 1
            continue
        mapped += 1
        h = stable_int(p["poi_id"])
        name = p["name"]
        persons = [2, 3, 4, 5, 6, 3, 4, 2][h % 8]
        base = CATEGORY_PRICE.get(category, 158)
        price = round(base + (h % 7) * 12 - (h % 3) * 6, 0)
        if price < 30:
            price = 30
        original = round(price * (1.5 + (h % 5) * 0.08), 0)
        sold = 120 + (h % 1880)
        real_rating = p.get("rating") or ""
        try:
            rating = round(float(real_rating), 1)
            if not (1.0 <= rating <= 5.0):
                raise ValueError
        except ValueError:
            rating = round(4.1 + (h % 9) * 0.1, 1)
            if rating > 4.9:
                rating = 4.9
        address = p.get("address") or ""
        deals.append({
            "item_id": "gb-{0}".format(ITEM_BASE + i),
            "merchant_id": "m-{0}".format(MERCHANT_BASE + i),
            "title": "{0}（{1} 人餐）".format(name, persons),
            "category": category,
            "city": TARGET_CITY,
            "district": DISTRICT,
            "price": float(price),
            "original_price": float(original),
            "sold_count": sold,
            "rating": rating,
            "min_people": max(1, persons - 1),
            "max_people": persons + 1,
            "tags": [category, leaf],
            "description": "真实商户(高德 POI {0}),地址:{1}".format(
                p["poi_id"], address or "南山区"),
        })

    root["deals"] = deals
    root["version"] = 1
    tmp = DATA_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(root, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, DATA_PATH)
    return len(deals), len(pois), mapped, skipped, skipped_leaves


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    total, n_poi, mapped, skipped, skipped_leaves = gen()
    print("POI deals generated. Total deals now: {0}".format(total))
    print("南山区 POI: {0} 个 → 映射 {1} 条 / 跳过 {2} 条".format(
        n_poi, mapped, skipped))
    if skipped_leaves:
        print("跳过明细(未映射 typecode):")
        for leaf, n in sorted(skipped_leaves.items(), key=lambda kv: -kv[1]):
            print("  {0} × {1}".format(leaf, n))
