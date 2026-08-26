#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Shared feature assembly for learning-to-rank (Phase 2.1).

Single source of truth imported by BOTH scripts/train_ranker.py (training)
and ranking_service/main.py (inference) so the two can never drift. The
feature order is fixed by FEATURE_NAMES and recorded into meta.json at
training time; the service validates it before scoring.

Feature groups:
  static    — from the candidate payload itself
  context   — candidate x request-context crosses
  stats     — item_features aggregates (impressions/likes/dislikes per item,
              built offline by scripts/build_features.py; missing => zeros)
  user      — user_profiles aggregates (missing => neutral defaults)

The rule_score/rule_ranks helpers replicate the C++ DealRanker formula
(src/tools/deal_tools.cpp) exactly — rank_in_rules lets the model learn a
*correction* on top of the rule baseline instead of relearning it.
"""

import math

FEATURE_NAMES = [
    # static
    "rating_norm",              # rating / 5
    "log1p_sold",               # log1p(sold_count)
    "discount",                 # (original_price - price) / original_price
    "price",
    # context crosses
    "price_fit",                # same formula as the C++ rule ranker
    "category_match",           # context.category == item.category
    "city_match",               # context.city == item.city
    "rank_in_rules",            # rule-score rank / (n-1); 0 = best
    # stats (item_features; cold start => neutral)
    "like_rate",                # (likes + 1) / (impressions + 2)
    "dislike_rate",             # dislikes / (impressions + 2)
    "log1p_impressions",
    "category_hot",             # share of likes in this item's category
    # user crosses (user_profiles; missing => neutral)
    "price_dev_from_avg",       # (price - avg_budget) / max(avg_budget, 1)
    "category_affinity",        # item.category in preferred_categories
    "price_sensitivity_price",  # price_sensitivity * price / 100
]


def discount_of(item):
    price = float(item.get("price") or 0)
    original = float(item.get("original_price") or 0)
    if original <= 0:
        return 0.0
    return max(0.0, (original - price) / original)


def price_fit(price, budget):
    # Mirrors the C++ rule ranker (deal_tools.cpp): no budget => neutral 1.
    if budget <= 0:
        return 1.0
    if price <= budget:
        return 1.0
    return max(0.0, 1.0 - (price - budget) / budget)


def rule_score(item, budget, max_sold):
    """The exact C++ DealRanker formula (0.35/0.25/0.25/0.15 weights)."""
    rating_norm = float(item.get("rating") or 0) / 5.0
    popularity = (float(item.get("sold_count") or 0) / max_sold) if max_sold > 0 else 0.0
    price = float(item.get("price") or 0)
    return (0.35 * rating_norm + 0.25 * popularity +
            0.25 * price_fit(price, budget) + 0.15 * discount_of(item))


def rule_ranks(candidates, budget):
    """rank_in_rules per candidate index: rule-score rank / max(n-1, 1).

    Ties share the better (lower) rank via stable sort on (-score, index).
    Returns a list parallel to `candidates`."""
    max_sold = max((float(c.get("sold_count") or 0) for c in candidates), default=0.0)
    scored = [(rule_score(c, budget, max_sold), i) for i, c in enumerate(candidates)]
    scored.sort(key=lambda t: (-t[0], t[1]))
    n = len(candidates)
    ranks = [0.0] * n
    for rank, (_, idx) in enumerate(scored):
        ranks[idx] = rank / max(n - 1, 1)
    return ranks


def build_feature_vector(item, stats, user, context):
    """One feature row, ordered by FEATURE_NAMES.

    item:    candidate dict (item_id/title/price/original_price/rating/
             sold_count/category/city/district/tags)
    stats:   item_features row dict or {} (impressions/likes/dislikes/
             category_hot)
    user:    user_profiles dict or {} (avg_budget/preferred_categories/
             price_sensitivity)
    context: {budget, city, category, rank_in_rules}
    """
    price = float(item.get("price") or 0)
    budget = float(context.get("budget") or 0)

    impressions = float(stats.get("impressions") or 0)
    likes = float(stats.get("likes") or 0)
    dislikes = float(stats.get("dislikes") or 0)

    avg_budget = float(user.get("avg_budget") or 0)
    preferred = user.get("preferred_categories") or []
    sensitivity = float(user.get("price_sensitivity") or 0.5)

    row = [
        float(item.get("rating") or 0) / 5.0,
        math.log1p(float(item.get("sold_count") or 0)),
        discount_of(item),
        price,
        price_fit(price, budget),
        1.0 if context.get("category") and context["category"] == item.get("category") else 0.0,
        1.0 if context.get("city") and context["city"] == item.get("city") else 0.0,
        float(context.get("rank_in_rules") or 0.0),
        (likes + 1.0) / (impressions + 2.0),
        dislikes / (impressions + 2.0),
        math.log1p(impressions),
        float(stats.get("category_hot") or 0.0),
        ((price - avg_budget) / max(avg_budget, 1.0)) if avg_budget > 0 else 0.0,
        1.0 if item.get("category") in preferred else 0.0,
        sensitivity * price / 100.0,
    ]
    assert len(row) == len(FEATURE_NAMES), "feature row drifted from FEATURE_NAMES"
    return row
