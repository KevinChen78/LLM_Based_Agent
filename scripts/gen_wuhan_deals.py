#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Append 5000 deterministic Wuhan group-buying deals to data/deals.json.

Idempotent: re-running removes any previously generated Wuhan records and
regenerates a fresh, stable set (so the file is reproducible). The original
non-Wuhan records are always preserved.

Diversity: titles are composed as "<landmark>·<dish>（<p> 人餐）" over
20 dish templates x 24 landmarks x 8 party sizes = 3840 distinct combos
(the remaining 1160 records repeat a combo once — realistic for chain
stores). Each landmark maps to a consistent district. All variation is a
pure function of the record index, so the output is byte-stable.

Run from the project root:
    python scripts/gen_wuhan_deals.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DATA_PATH = os.path.join(PROJECT, "data", "deals.json")

NUM = 5000
CITY = "武汉"

# (landmark, district) — landmark goes into the title, district is consistent.
LANDMARKS = [
    ("黄鹤楼", "武昌"), ("司门口", "武昌"),
    ("江汉路", "江汉"), ("武广", "江汉"),
    ("武汉天地", "江岸"), ("后湖", "江岸"),
    ("王家湾", "汉阳"), ("钟家村", "汉阳"),
    ("街道口", "洪山"), ("南湖", "洪山"),
    ("光谷步行街", "光谷"), ("关山大道", "光谷"),
    ("红钢城", "青山"), ("建二", "青山"),
    ("汉正街", "硚口"), ("古田", "硚口"),
    ("纸坊", "江夏"), ("藏龙岛", "江夏"),
    ("后官湖", "蔡甸"), ("蔡甸广场", "蔡甸"),
    ("吴家山", "东西湖"), ("金银湖", "东西湖"),
    ("盘龙城", "黄陂"), ("前川", "黄陂"),
]

# (category, title template, tags, base_price, base_min_people, base_max_people, description)
TEMPLATES = [
    ("小龙虾", "蒜蓉小龙虾（{p} 人餐）", ["小龙虾", "蒜蓉"], 268, 2, 4, "武汉夜宵头牌，现炒蒜蓉小龙虾，麻辣鲜香"),
    ("小龙虾", "油焖大虾（{p} 人餐）", ["小龙虾", "油焖", "麻辣"], 298, 3, 5, "潜江油焖大虾，汤汁浓郁，配凉面绝绝子"),
    ("小龙虾", "麻辣小龙虾拼盘（{p} 人餐）", ["小龙虾", "麻辣", "夜宵"], 228, 2, 3, "深夜麻辣小龙虾拼盘，朋友撸串首选"),
    ("湖北菜", "武昌鱼全鱼宴（{p} 人餐）", ["武昌鱼", "湖北菜"], 258, 3, 4, "正宗武昌鱼，清蒸红烧双吃，地道鄂菜"),
    ("湖北菜", "排骨藕汤湖北菜（{p} 人餐）", ["排骨藕汤", "湖北菜"], 218, 3, 5, "洪湖粉藕炖排骨，武汉家常滋味"),
    ("湖北菜", "洪山菜薹炒腊肉（{p} 人餐）", ["菜薹", "腊肉", "湖北菜"], 188, 2, 4, "洪山菜薹配腊肉，时令鄂菜代表"),
    ("湖北菜", "沔阳三蒸套餐（{p} 人餐）", ["沔阳三蒸", "湖北菜"], 238, 3, 4, "蒸肉蒸鱼蒸菜，沔阳三蒸一笼满足"),
    ("小吃", "热干面+三鲜豆皮过早套餐", ["热干面", "豆皮", "过早"], 39, 1, 2, "武汉过早标配，热干面配三鲜豆皮"),
    ("小吃", "武汉过早拼盘（{p} 人餐）", ["热干面", "面窝", "过早"], 68, 2, 3, "面窝豆皮热干面，地道武汉过早拼盘"),
    ("火锅", "重庆牛油火锅（{p} 人餐）", ["牛油", "火锅", "微辣"], 198, 2, 4, "正宗牛油锅底，毛肚鸭肠新鲜，可调微辣"),
    ("火锅", "火锅自助（{p} 人餐）", ["自助", "火锅"], 258, 3, 5, "荤素畅吃火锅自助，性价比之选"),
    ("烧烤", "深夜烧烤大拼盘（{p} 人餐）", ["羊肉串", "烤翅", "烧烤", "夜宵"], 178, 2, 4, "深夜烧烤拼盘，60 串起撸，烟火气十足"),
    ("烧烤", "烤鱼+烤串（{p} 人餐）", ["烤鱼", "烧烤"], 168, 2, 3, "万州烤鱼配烤串，朋友聚会推荐"),
    ("串串", "成都串串香（{p} 人餐）", ["串串", "辣"], 158, 2, 4, "成都风味串串香，百串任选，热辣过瘾"),
    ("烤肉", "炭火烤肉（{p} 人餐）", ["烤肉", "炭火"], 248, 3, 4, "炭火现烤和牛拼盘，肉质细嫩"),
    ("川菜", "川菜水煮鱼（{p} 人餐）", ["水煮鱼", "川菜", "辣"], 188, 2, 4, "麻辣水煮鱼，嗜辣者必点"),
    ("日料", "日式料理（{p} 人餐）", ["刺身", "寿司", "日料"], 328, 2, 3, "刺身拼盘精致，主厨料理双人/三人餐"),
    ("海鲜", "海鲜大咖（{p} 人餐）", ["大虾", "生蚝", "海鲜"], 318, 3, 5, "海鲜大咖拼盘，分量足，聚餐首选"),
    ("西餐", "牛排双人餐", ["牛排", "西餐", "红酒"], 268, 2, 2, "M 级和牛牛排配红酒，纪念日之选"),
    ("粤菜", "粤式茶餐厅（{p} 人餐）", ["虾饺", "粤菜"], 198, 2, 4, "经典粤式点心拼盘，老少皆宜"),
]

PARTY_SIZES = [2, 3, 4, 5, 6, 8, 10, 12]


def gen():
    with open(DATA_PATH, "r", encoding="utf-8") as f:
        root = json.load(f)
    deals = root.get("deals", [])
    # Drop any existing Wuhan records so regeneration is stable.
    deals = [d for d in deals if d.get("city") != CITY]

    n_tpl = len(TEMPLATES)
    n_lm = len(LANDMARKS)
    for i in range(NUM):
        # Independent cycle lengths keep the three dimensions orthogonal:
        # template changes every record, landmark every 20, party size every 480.
        cat, title_tpl, tags, base_price, bmin, bmax, desc = TEMPLATES[i % n_tpl]
        landmark, district = LANDMARKS[(i // n_tpl) % n_lm]
        p = PARTY_SIZES[(i // (n_tpl * n_lm)) % len(PARTY_SIZES)]
        title = title_tpl.format(p=p) if "{p}" in title_tpl else title_tpl
        title = "{0}·{1}".format(landmark, title)
        price = round(base_price + (i % 7) * 12 - (i % 3) * 6
                      + ((i // n_tpl) % n_lm) % 5 * 10, 0)
        if price < 30:
            price = 30
        original = round(price * (1.5 + (i % 5) * 0.08), 0)
        sold = 120 + (i * 37 % 1880)
        rating = round(4.1 + (i % 9) * 0.1, 1)
        if rating > 4.9:
            rating = 4.9
        # Scale the serving range around the title's people count.
        min_p = max(1, bmin + (p - 3))
        max_p = bmax + (p - 3)
        if max_p < min_p:
            max_p = min_p
        deal = {
            "item_id": "gb-{0}".format(20021 + i),
            "merchant_id": "m-{0}".format(30021 + i),
            "title": title,
            "category": cat,
            "city": CITY,
            "district": district,
            "price": float(price),
            "original_price": float(original),
            "sold_count": sold,
            "rating": rating,
            "min_people": min_p,
            "max_people": max_p,
            "tags": tags,
            "description": desc,
        }
        deals.append(deal)

    root["deals"] = deals
    root["version"] = 1
    with open(DATA_PATH, "w", encoding="utf-8") as f:
        json.dump(root, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return len(deals)


if __name__ == "__main__":
    total = gen()
    sys.stdout.buffer.write(
        ("Wuhan deals generated ({0}). Total deals now: {1}\n"
         .format(NUM, total)).encode("utf-8")
    )
