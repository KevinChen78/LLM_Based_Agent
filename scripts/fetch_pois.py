#!/usr/bin/env python3
"""Fetch real merchant POIs from the AMap (高德) Web service API — Phase 7-A.

Pure stdlib. Deterministic and quota-friendly:

- Endpoint: https://restapi.amap.com/v3/place/text (place text search),
  types=050000 (餐饮服务) by default, per city, offset=25 paged.
- Cities: 武汉/深圳/北京/上海 (same four cities as the existing catalog — no
  new cities introduced).
- Cache: every successful raw response is written to
  data/pois_cache/<city>_<type>_p<N>.json; reruns hit the cache and make ZERO
  network requests. Failed responses are NOT cached (they would poison reruns).
- Rate limit & quota: 1s between requests; a persisted daily counter
  (data/pois_cache/_quota.json) stops the run at AMAP_DAILY_LIMIT (default
  4000) — cached pages are kept, continue tomorrow.
- Output: data/pois_raw.json (deduped by POI id, sorted by id), written
  atomically (temp file + os.replace). deals.json is never touched.

Key management: AMAP_KEY comes from the real environment or the project-root
.env / .env.local (gitignored). No key -> clear instructions, exit 2, nothing
is written.

Compliance: AMap open-platform Web service API, personal learning/demo use
only; the dataset is not redistributed (data/ is gitignored).

Usage:
    python scripts/fetch_pois.py              # full pull (all 4 cities)
    python scripts/fetch_pois.py --limit 50   # small trial run
    # 南山区周边搜索(2026-08-27 用户指令:配额有限,聚焦深圳南山):
    python scripts/fetch_pois.py --around 113.95,22.53 --radius 10000 \
        --city 深圳 --district 南山区
"""

import argparse
import json
import math
import os
import sys
import time
import urllib.parse
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE_DIR = os.path.join(ROOT, "data", "pois_cache")
OUT_PATH = os.path.join(ROOT, "data", "pois_raw.json")

API = "https://restapi.amap.com/v3/place/text"
API_AROUND = "https://restapi.amap.com/v3/place/around"
CITIES = ["武汉", "深圳", "北京", "上海"]
TYPES = ["050000"]  # 餐饮服务(一级类目;细分码可在映射阶段再精细化)
OFFSET = 25
PAGE_SLEEP_S = 1.0
REQ_TIMEOUT_S = 15


def load_env_files():
    """Same convention as llm_gateway: real env > .env.local > .env; a key
    already set is never overridden (setdefault semantics, .env wins ties)."""
    for name in (".env", ".env.local"):
        path = os.path.join(ROOT, name)
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    key, _, val = line.partition("=")
                    os.environ.setdefault(key.strip(), val.strip())
        except FileNotFoundError:
            pass


def load_quota(today):
    path = os.path.join(CACHE_DIR, "_quota.json")
    try:
        with open(path, encoding="utf-8") as f:
            q = json.load(f)
        if q.get("date") == today:
            return int(q.get("count", 0))
    except (FileNotFoundError, ValueError):
        pass
    return 0


def save_quota(today, count):
    os.makedirs(CACHE_DIR, exist_ok=True)
    tmp = os.path.join(CACHE_DIR, "_quota.json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"date": today, "count": count}, f)
    os.replace(tmp, os.path.join(CACHE_DIR, "_quota.json"))


def cache_path(city, typecode, page):
    return os.path.join(CACHE_DIR, f"{city}_{typecode}_p{page}.json")


def fetch_page(key, city, typecode, page, around=None, radius=10000):
    """Return (raw_dict, from_cache). Raises on transport/HTTP failure.

    around="lng,lat" switches to /v3/place/around (circle search); the cache
    key namespaced `around_<city>_...` never collides with text-search pages.
    """
    tag = ("around_" + around.replace(",", "_").replace(".", "d")
           if around else city)
    path = cache_path(tag, typecode, page)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f), True
    params = {
        "key": key, "types": typecode,
        "offset": OFFSET, "page": page,
        # extensions=all 才返回 biz_ext(真实评分 rating / 人均 cost)。
        "extensions": "all",
    }
    if around:
        params["location"] = around
        params["radius"] = radius
        url = API_AROUND
    else:
        params["city"] = city
        params["citylimit"] = "true"
        url = API
    qs = urllib.parse.urlencode(params)
    req = urllib.request.Request(f"{url}?{qs}",
                                 headers={"User-Agent": "groupbuy-agent-demo/1.0"})
    with urllib.request.urlopen(req, timeout=REQ_TIMEOUT_S) as resp:
        raw = json.loads(resp.read().decode("utf-8"))
    # Only successful responses are cached — a cached failure page would
    # poison every future rerun.
    if raw.get("status") == "1":
        os.makedirs(CACHE_DIR, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(raw, f, ensure_ascii=False)
        os.replace(tmp, path)
    return raw, False


def norm_poi(p, city):
    """Normalize one POI; returns None when unusable (no id/name)."""
    poi_id = p.get("id")
    name = (p.get("name") or "").strip()
    if not poi_id or not name:
        return None
    biz = p.get("biz_ext") or {}
    rating = biz.get("rating") if isinstance(biz, dict) else None
    # AMap returns "" or [] when no rating exists.
    if not isinstance(rating, str) or not rating:
        rating = ""
    return {
        "poi_id": poi_id,
        "name": name,
        "typecode": p.get("type", ""),
        "city": city,
        "adname": p.get("adname", ""),       # 真实区县名
        "address": p.get("address", "") if isinstance(p.get("address"), str) else "",
        "location": p.get("location", ""),   # "lng,lat"
        "rating": rating,                    # 真实评分(字符串,可能为空)
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N collected POIs (0 = no limit)")
    ap.add_argument("--around", default="", metavar="LNG,LAT",
                    help="circle search around this center (place/around) "
                         "instead of the default 4-city text search")
    ap.add_argument("--radius", type=int, default=10000,
                    help="around-search radius in meters (default 10000)")
    ap.add_argument("--city", default="",
                    help="city label recorded on each POI (around mode; "
                         "defaults to the response's cityname)")
    ap.add_argument("--district", default="",
                    help="keep only POIs whose adname equals this district "
                         "(e.g. 南山区); around mode only")
    ap.add_argument("--grid", default="", metavar="MINLNG,MINLAT,MAXLNG,MAXLAT",
                    help="grid of around-search centers covering this bbox "
                         "(use with --step/--radius; overrides --around)")
    ap.add_argument("--step", type=float, default=2000.0,
                    help="grid center spacing in meters (default 2000; "
                         "choose <= sqrt(2)*radius for overlap-free-ish cover)")
    ap.add_argument("--out", default="",
                    help="output path (default data/pois_raw.json; around "
                         "mode defaults to data/pois_<district>.json)")
    args = ap.parse_args()

    load_env_files()
    key = os.environ.get("AMAP_KEY", "").strip()
    if not key:
        print("error: AMAP_KEY 未设置。\n"
              "  1. 注册高德开放平台 https://lbs.amap.com/ → 创建应用 →\n"
              "     创建「Web 服务」类型 key;\n"
              f"  2. 写入项目根 .env.local(已 gitignore):AMAP_KEY=你的key\n"
              "  3. 重跑本脚本。deals.json 未被触碰。", file=sys.stderr)
        return 2

    daily_limit = int(os.environ.get("AMAP_DAILY_LIMIT", "4000"))
    today = time.strftime("%Y-%m-%d")
    used = load_quota(today)
    n_requests = 0
    pois = {}
    stop = False

    # Task list: (city_label, around_center_or_None)
    if args.grid:
        min_lng, min_lat, max_lng, max_lat = (
            float(x) for x in args.grid.split(","))
        lat0 = (min_lat + max_lat) / 2.0
        dlat = args.step / 111000.0
        dlng = args.step / (111000.0 * math.cos(math.radians(lat0)))
        centers = []
        lat = min_lat
        while lat <= max_lat + 1e-9:
            lng = min_lng
            while lng <= max_lng + 1e-9:
                centers.append("{0:.5f},{1:.5f}".format(lng, lat))
                lng += dlng
            lat += dlat
        print("grid: {0} 个中心点 (step={1}m, radius={2}m)".format(
            len(centers), args.step, args.radius))
        tasks = [(args.city, c) for c in centers]
    elif args.around:
        tasks = [(args.city, args.around)]
    else:
        tasks = [(c, None) for c in CITIES]

    for city, around in tasks:
        if stop:
            break
        for typecode in TYPES:
            if stop:
                break
            page = 1
            while True:
                if args.limit and len(pois) >= args.limit:
                    stop = True
                    break
                if used >= daily_limit:
                    print(f"配额熔断:今日已用 {used}/{daily_limit} 次,"
                          f"已缓存部分保留,次日续跑。")
                    stop = True
                    break
                try:
                    raw, cached = fetch_page(key, city, typecode, page,
                                             around=around, radius=args.radius)
                except Exception as e:
                    print(f"warn: {city} {typecode} p{page} 请求失败({e!r}),"
                          f"跳过该页(未缓存,下次重试)。")
                    break
                if not cached:
                    used += 1
                    n_requests += 1
                    save_quota(today, used)
                    time.sleep(PAGE_SLEEP_S)
                if raw.get("status") != "1":
                    print(f"warn: {city} {typecode} p{page} API 拒绝:"
                          f" infocode={raw.get('infocode')} info={raw.get('info')}"
                          f"(未缓存,检查 key/配额)。")
                    break
                page_pois = raw.get("pois") or []
                for p in page_pois:
                    n = norm_poi(p, city or p.get("cityname", ""))
                    if n is None:
                        continue
                    if args.district and n["adname"] != args.district:
                        continue
                    pois[n["poi_id"]] = n
                total = int(raw.get("count") or 0)
                print(f"  {city or 'around'}/{typecode} p{page}: +{len(page_pois)}"
                      f" (累计去重 {len(pois)}, 服务端总数 {total}"
                      f"{', 缓存' if cached else ''})")
                if not page_pois or page * OFFSET >= total:
                    break
                page += 1

    out_path = args.out or (
        os.path.join(ROOT, "data", f"pois_{args.district}.json")
        if (args.around or args.grid) and args.district else OUT_PATH)
    out = {
        "source": ("amap /v3/place/around" if (args.around or args.grid)
                   else "amap /v3/place/text") + " (personal demo use only)",
        "cities": [t[0] for t in tasks],
        "around": args.around or None,
        "grid": args.grid or None,
        "district_filter": args.district or None,
        "types": TYPES,
        "count": len(pois),
        "pois": [pois[k] for k in sorted(pois)],
    }
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
        f.write("\n")
    os.replace(tmp, out_path)

    print(f"\n完成: {len(pois)} 个去重 POI → {out_path}")
    print(f"本次网络请求 {n_requests} 次,今日累计 {used}/{daily_limit};"
          f"缓存目录 {CACHE_DIR}(重跑零请求)。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
