#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Append deterministic Shenzhen / Beijing / Shanghai group-buying deals to
data/deals.json — same generation pattern as gen_wuhan_deals.py.

Idempotent: re-running removes only the records this script generated
(matched by generated item_id ranges) and regenerates a fresh, stable set.
The original seed deals (gb-20001..20120, incl. Beijing/Shanghai ones) and
the Wuhan records are always preserved.

Run from the project root:
    python scripts/gen_city_deals.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DATA_PATH = os.path.join(PROJECT, "data", "deals.json")

NUM_PER_CITY = 100

# (category, title template, tags, base_price, base_min_people, base_max_people, description)
CITY_CONFIGS = {
    "深圳": {
        "item_base": 26021,      # gb-26021..26120 (Wuhan block is gb-20021..25020)
        "merchant_base": 36021,
        "districts": ["南山", "福田", "罗湖", "宝安", "龙岗", "龙华", "盐田", "坪山", "光明", "大鹏"],
        "templates": [
            ("火锅", "潮汕牛肉火锅（{p} 人餐）", ["潮汕", "牛肉火锅"], 268, 3, 5, "现切鲜牛肉，吊龙匙柄三花趾，沙茶酱绝配"),
            ("火锅", "海南椰子鸡火锅（{p} 人餐）", ["椰子鸡", "文昌鸡"], 238, 2, 4, "新鲜椰青水锅底，文昌鸡肉质滑嫩"),
            ("粤菜", "粤式早茶点心（{p} 人餐）", ["早茶", "虾饺", "点心"], 168, 2, 4, "虾饺烧卖凤爪，一盅两件老广味道"),
            ("海鲜", "大鹏海鲜加工餐（{p} 人餐）", ["海鲜", "大鹏", "加工"], 328, 3, 5, "较场尾自采海鲜加工，鲜到掉眉毛"),
            ("海鲜", "盐田海鲜街套餐（{p} 人餐）", ["海鲜", "盐田"], 298, 3, 4, "盐田渔港直供，椒盐濑尿虾必点"),
            ("粤菜", "港式茶餐厅（{p} 人餐）", ["茶餐厅", "菠萝油", "奶茶"], 128, 2, 3, "菠萝油配丝袜奶茶，港味十足"),
            ("烧烤", "海边烧烤大拼盘（{p} 人餐）", ["烧烤", "夜宵"], 178, 2, 4, "炭火烧烤拼盘，生蚝扇贝撸串自由"),
            ("日料", "日式放题（{p} 人餐）", ["日料", "放题", "刺身"], 358, 2, 3, "刺身寿司放题，三文鱼厚切任吃"),
            ("川菜", "川味毛血旺（{p} 人餐）", ["川菜", "毛血旺", "辣"], 168, 2, 4, "麻辣鲜香毛血旺，下饭神器"),
            ("火锅", "重庆老火锅（{p} 人餐）", ["火锅", "牛油", "麻辣"], 198, 2, 4, "牛油红汤锅底，毛肚七上八下"),
            ("粤菜", "顺德鱼生（{p} 人餐）", ["顺德菜", "鱼生"], 288, 3, 4, "顺德捞起鱼生，薄如蝉翼"),
            ("西餐", "战斧牛排（{p} 人餐）", ["牛排", "西餐"], 398, 2, 3, "战斧牛排果木烤制，肉汁丰盈"),
            ("小吃", "光明乳鸽（{p} 人餐）", ["乳鸽", "深圳特产"], 108, 2, 3, "光明红烧乳鸽，皮脆肉嫩多汁"),
            ("甜品", "广式糖水铺（{p} 人餐）", ["糖水", "双皮奶"], 68, 2, 3, "双皮奶姜撞奶，饭后甜品首选"),
            ("烤肉", "韩式烤肉（{p} 人餐）", ["烤肉", "韩式"], 228, 2, 4, "厚切五花肉配生菜，蘸料地道"),
            ("串串", "冷锅串串（{p} 人餐）", ["串串", "冷锅"], 138, 2, 3, "冷锅串串麻辣鲜香，夜宵好去处"),
            ("湘菜", "湘菜剁椒鱼头（{p} 人餐）", ["湘菜", "剁椒鱼头"], 188, 3, 5, "剁椒鱼头配面条，鲜辣过瘾"),
            ("自助餐", "海鲜自助餐（{p} 人餐）", ["自助", "海鲜"], 298, 1, 2, "波士顿龙虾畅吃，海鲜自助天花板"),
            ("素食", "创意素食（{p} 人餐）", ["素食", "创意菜"], 158, 2, 3, "分子料理素食，健康新选择"),
            ("甜品", "网红奶茶双人餐", ["奶茶", "甜品"], 56, 1, 2, "人气奶茶配舒芙蕾，下午茶标配"),
            # Phase 3-B:西式快餐补齐(只增不改)
            ("西餐", "现烤牛肉汉堡（{p} 人餐）", ["汉堡", "西式快餐"], 78, 1, 2, "现烤牛肉汉堡配粗薯，快餐经典"),
            ("西餐", "披萨意面拼盘（{p} 人餐）", ["披萨", "意面", "西式快餐"], 108, 2, 3, "薄底披萨配肉酱意面，分享装"),
        ],
    },
    "北京": {
        "item_base": 27021,      # gb-27021..27120
        "merchant_base": 37021,
        "districts": ["海淀", "朝阳", "东城", "西城", "丰台", "石景山", "通州", "昌平", "大兴", "顺义"],
        "templates": [
            ("北京菜", "北京烤鸭（{p} 人餐）", ["烤鸭", "北京菜"], 268, 2, 4, "挂炉烤鸭现片，鸭皮酥脆蘸白糖"),
            ("火锅", "老北京铜锅涮肉（{p} 人餐）", ["涮肉", "铜锅", "羊肉"], 198, 2, 4, "炭火铜锅手切鲜羊肉，麻酱小料地道"),
            ("火锅", "羊蝎子火锅（{p} 人餐）", ["羊蝎子", "火锅"], 188, 3, 5, "老北京羊蝎子，骨髓吸着吃才香"),
            ("小吃", "卤煮火烧双人餐", ["卤煮", "北京小吃"], 76, 1, 2, "百年卤煮老店，火烧入味肺头软烂"),
            ("小吃", "老北京炸酱面（{p} 人餐）", ["炸酱面", "北京小吃"], 58, 1, 2, "小碗干炸酱，八样菜码拌着吃"),
            ("烤肉", "炙子烤肉（{p} 人餐）", ["炙子烤肉", "牛羊肉"], 168, 2, 3, "武吃炙子烤肉，烟火气十足"),
            ("北京菜", "宫廷菜官府菜（{p} 人餐）", ["宫廷菜", "官府菜"], 398, 3, 5, "宫廷御膳传承，仪式感拉满"),
            ("火锅", "四川火锅（{p} 人餐）", ["火锅", "麻辣"], 198, 2, 4, "地道川味火锅，在京解乡愁"),
            ("烧烤", "望京小腰烧烤（{p} 人餐）", ["烧烤", "小腰", "夜宵"], 158, 2, 4, "望京小腰配啤酒，深夜食堂"),
            ("日料", "日式居酒屋（{p} 人餐）", ["日料", "居酒屋"], 258, 2, 3, "烧鸟拼盘配清酒，下班小酌"),
            ("海鲜", "蒸汽海鲜（{p} 人餐）", ["海鲜", "蒸汽"], 298, 3, 4, "鲜活海鲜现蒸，原汁原味"),
            ("西餐", "惠灵顿牛排双人餐", ["牛排", "西餐"], 368, 2, 2, "惠灵顿牛排酥皮香脆，约会首选"),
            ("粤菜", "粤式打边炉（{p} 人餐）", ["粤菜", "打边炉"], 268, 3, 4, "花胶鸡汤锅底，滋补鲜美"),
            ("小吃", "护国寺小吃拼盘（{p} 人餐）", ["豌豆黄", "驴打滚", "小吃"], 68, 2, 3, "豌豆黄驴打滚艾窝窝，京味点心拼盘"),
            ("川菜", "宜宾燃面+川菜（{p} 人餐）", ["川菜", "燃面"], 128, 2, 3, "燃面配回锅肉，川味十足"),
            ("湘菜", "长沙口味虾（{p} 人餐）", ["湘菜", "口味虾"], 198, 2, 4, "紫苏口味虾，辣得过瘾"),
            ("自助餐", "五星酒店自助餐（{p} 人餐）", ["自助", "酒店"], 328, 1, 2, "五星酒店自助，海鲜甜品畅吃"),
            ("东北菜", "东北铁锅炖（{p} 人餐）", ["东北菜", "铁锅炖"], 188, 3, 5, "铁锅炖大鹅，贴饼子吸满汤汁"),
            ("素食", "京味素食（{p} 人餐）", ["素食"], 138, 2, 3, "功德林风味素食，素鸭素火腿"),
            ("甜品", "宫廷奶酪双人餐", ["甜品", "奶酪"], 52, 1, 2, "宫廷奶酪配杏仁豆腐，老北京甜品"),
            # Phase 3-B:西式快餐补齐(只增不改)
            ("西餐", "美式汉堡拼盘（{p} 人餐）", ["汉堡", "美式", "西式快餐"], 88, 2, 3, "美式大汉堡配洋葱圈，分量扎实"),
            ("西餐", "炸鸡全家桶（{p} 人餐）", ["炸鸡", "西式快餐"], 118, 3, 5, "脆皮炸鸡全家桶，聚会分享装"),
        ],
    },
    "上海": {
        "item_base": 28021,      # gb-28021..28120
        "merchant_base": 38021,
        "districts": ["黄浦", "静安", "徐汇", "浦东", "长宁", "虹口", "杨浦", "普陀", "闵行", "宝山"],
        "templates": [
            ("本帮菜", "本帮菜（{p} 人餐）", ["本帮菜", "红烧肉"], 228, 3, 4, "红烧肉油爆虾，浓油赤酱老上海味道"),
            ("小吃", "蟹粉小笼双人餐", ["小笼", "蟹粉"], 98, 1, 2, "现拆蟹粉小笼，皮薄汁多"),
            ("小吃", "生煎+牛肉汤双人餐", ["生煎", "上海小吃"], 58, 1, 2, "大壶春生煎，底脆汤鲜"),
            ("海鲜", "舟山海鲜（{p} 人餐）", ["海鲜", "舟山"], 328, 3, 5, "舟山直供带鱼黄鱼，家烧入味"),
            ("日料", "Omakase 主厨料理（{p} 人餐）", ["日料", "Omakase"], 498, 1, 2, "主厨发办，当日渔获"),
            ("西餐", "法餐双人餐", ["法餐", "西餐"], 468, 2, 2, "法式鹅肝配红酒，浪漫之选"),
            ("火锅", "潮汕牛肉火锅（{p} 人餐）", ["火锅", "牛肉"], 258, 2, 4, "鲜切吊龙，三起三落"),
            ("粤菜", "粤式点心自助（{p} 人餐）", ["粤菜", "点心", "自助"], 198, 2, 3, "虾饺皇流沙包任点，早茶自由"),
            ("川菜", "自贡盐帮菜（{p} 人餐）", ["川菜", "盐帮菜"], 168, 2, 4, "鲜锅兔冷吃牛肉，辣得地道"),
            ("烧烤", "延边烧烤（{p} 人餐）", ["烧烤", "延边"], 168, 2, 4, "延边黄牛肉串，蘸料一绝"),
            ("小吃", "老上海面馆（{p} 人餐）", ["面条", "葱油拌面"], 48, 1, 2, "葱油拌面配辣肉浇头"),
            ("海鲜", "大闸蟹套餐（{p} 人餐）", ["大闸蟹", "蟹宴"], 398, 2, 3, "阳澄湖大闸蟹，公母对蟹"),
            ("自助餐", "江景自助餐（{p} 人餐）", ["自助", "江景"], 358, 1, 2, "外滩江景自助，环境菜品双绝"),
            ("湘菜", "湘西土菜（{p} 人餐）", ["湘菜", "土菜"], 158, 3, 4, "湘西外婆菜，腊味合蒸"),
            ("云南菜", "云南过桥米线（{p} 人餐）", ["云南菜", "米线"], 88, 1, 2, "过桥米线配汽锅鸡"),
            ("台湾菜", "台湾三杯鸡（{p} 人餐）", ["台湾菜", "三杯鸡"], 148, 2, 3, "三杯鸡卤肉饭，台式家常"),
            ("素食", "本帮素食（{p} 人餐）", ["素食", "本帮"], 128, 2, 3, "烤麸素鸡，本帮素食老字号"),
            ("西餐", "精品咖啡brunch双人餐", ["咖啡", "brunch"], 128, 1, 2, "手冲咖啡配班尼迪克蛋"),
            ("甜品", "法式甜品双人下午茶", ["甜品", "下午茶"], 158, 1, 2, "马卡龙歌剧院蛋糕，精致下午茶"),
            ("串串", "乐山钵钵鸡（{p} 人餐）", ["钵钵鸡", "串串"], 118, 2, 3, "藤椒钵钵鸡，冷吃更入味"),
            # Phase 3-B:西式快餐补齐(只增不改)
            ("西餐", "手作汉堡brunch（{p} 人餐）", ["汉堡", "brunch", "西式快餐"], 108, 1, 2, "手作牛肉汉堡配薯角，brunch 新宠"),
            ("西餐", "意式披萨双人餐", ["披萨", "西式快餐"], 138, 2, 2, "窑烤玛格丽特披萨，饼底薄脆"),
        ],
    },
}


def gen():
    with open(DATA_PATH, "r", encoding="utf-8") as f:
        root = json.load(f)
    deals = root.get("deals", [])

    # Drop only the records this script generated (by generated id ranges) so
    # the original seed deals — including Beijing/Shanghai ones — survive.
    ranges = [(c["item_base"], c["item_base"] + NUM_PER_CITY)
              for c in CITY_CONFIGS.values()]
    # Legacy id blocks from before the Wuhan range grew to gb-20021..25020.
    # They now overlap the Wuhan block, so match legacy ids only when the
    # record's city is the legacy city (Wuhan records keep their ids).
    legacy = [((20221, 20321), "深圳"), ((20321, 20421), "北京"),
              ((20421, 20521), "上海")]

    def is_generated(d):
        iid = d.get("item_id", "")
        if not iid.startswith("gb-"):
            return False
        try:
            n = int(iid[3:])
        except ValueError:
            return False
        if any(lo <= n < hi for lo, hi in ranges):
            return True
        return any(lo <= n < hi and d.get("city") == city
                   for (lo, hi), city in legacy)

    deals = [d for d in deals if not is_generated(d)]

    for city, cfg in CITY_CONFIGS.items():
        templates = cfg["templates"]
        districts = cfg["districts"]
        for i in range(NUM_PER_CITY):
            cat, title_tpl, tags, base_price, bmin, bmax, desc = \
                templates[i % len(templates)]
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
            district = districts[(i * 5) % len(districts)]
            min_p = max(1, bmin + (p - 3))
            max_p = bmax + (p - 3)
            if max_p < min_p:
                max_p = min_p
            deals.append({
                "item_id": "gb-{0}".format(cfg["item_base"] + i),
                "merchant_id": "m-{0}".format(cfg["merchant_base"] + i),
                "title": title,
                "category": cat,
                "city": city,
                "district": district,
                "price": float(price),
                "original_price": float(original),
                "sold_count": sold,
                "rating": rating,
                "min_people": min_p,
                "max_people": max_p,
                "tags": tags,
                "description": desc,
            })

    root["deals"] = deals
    root["version"] = 1
    with open(DATA_PATH, "w", encoding="utf-8") as f:
        json.dump(root, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return len(deals)


if __name__ == "__main__":
    total = gen()
    sys.stdout.buffer.write(
        ("Shenzhen/Beijing/Shanghai deals generated. Total deals now: {0}\n"
         .format(total)).encode("utf-8")
    )
