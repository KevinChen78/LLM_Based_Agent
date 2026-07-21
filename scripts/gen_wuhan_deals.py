#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Append 100 deterministic Wuhan group-buying deals to data/deals.json.

Idempotent: re-running removes any previously generated Wuhan records and
regenerates a fresh, stable set (so the file is reproducible). The original
non-Wuhan records are always preserved.

Run from the project root:
    python scripts/gen_wuhan_deals.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DATA_PATH = os.path.join(PROJECT, "data", "deals.json")

NUM = 100
CITY = "武汉"

DISTRICTS = [
    "武昌", "江汉", "江岸", "汉阳", "洪山", "光谷",
    "青山", "硚口", "江夏", "蔡甸", "东西湖", "黄陂",
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


def gen():
    with open(DATA_PATH, "r", encoding="utf-8") as f:
        root = json.load(f)
    deals = root.get("deals", [])
    # Drop any existing Wuhan records so regeneration is stable.
    deals = [d for d in deals if d.get("city") != CITY]

    for i in range(NUM):
        cat, title_tpl, tags, base_price, bmin, bmax, desc = TEMPLATES[i % len(TEMPLATES)]
        # People: cycle a small set for variety in the title and serving range.
        p = [2, 3, 4, 5, 6, 3, 4, 2][i % 8]
        title = title_tpl.format(p=p) if "{p}" in title_tpl else title_tpl
        price = round(base_price + (i % 7) * 12 - (i % 3) * 6, 0)
        if price < 30:
            price = 30
        original = round(price * (1.5 + (i % 5) * 0.08), 0)
        sold = 120 + (i * 37 % 1880)
        rating = round(4.1 + (i % 9) * 0.1, 1)
        if rating > 4.9:
            rating = 4.9
        district = DISTRICTS[(i * 5) % len(DISTRICTS)]
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
        ("Wuhan deals generated. Total deals now: {0}\n".format(total)).encode("utf-8")
    )
