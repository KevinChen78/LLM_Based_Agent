#!/usr/bin/env python3
"""Phase 5-D: gRPC front-end for the retrieval service (pilot).

Runs in the SAME process as the HTTP server, on a separate port
(GRPC_PORT, default off). Both protocols call the same handler functions
(retrieve_deals / retrieve_kb / health payload builder from main.py), so
behaviour cannot drift between them — the parity matrix in
scripts/test_pg_retrieval.py checks http vs grpc field-by-field.

Contract: proto/retrieval.proto mirrors the HTTP JSON shapes exactly,
including presence semantics (optional max_price/min_price/people/top_k) and
the Phase 3-C additive audit fields (relaxed_level only set when > 0).

Any gRPC failure on the C++ side falls back to the HTTP client; this module
itself degrades to "gRPC off" if grpcio is not installed.
"""

import os
import sys
from concurrent import futures

_GEN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen")
if _GEN_DIR not in sys.path:
    sys.path.insert(0, _GEN_DIR)

try:
    import grpc
    import retrieval_pb2
    import retrieval_pb2_grpc
except ImportError:  # grpcio not installed -> gRPC front-end unavailable
    grpc = None


def _deals_request_to_body(req):
    """proto RetrieveDealsRequest -> the exact dict the HTTP handler accepts."""
    body = {
        "city": req.city,
        "district": req.district,
        "category": req.category,
        "query": req.query,
    }
    # Presence matters: absent = no filter; max_price=0 is a real filter.
    if req.HasField("max_price"):
        body["max_price"] = req.max_price
    if req.HasField("min_price"):
        body["min_price"] = req.min_price
    if req.HasField("people"):
        body["people"] = req.people
    if req.HasField("top_k"):
        body["top_k"] = req.top_k
    return body


def _deal_item_to_proto(d):
    item = retrieval_pb2.DealItem(
        item_id=d.get("item_id", ""),
        title=d.get("title", ""),
        category=d.get("category", ""),
        city=d.get("city", ""),
        district=d.get("district", ""),
        price=float(d.get("price", 0) or 0),
        original_price=float(d.get("original_price", 0) or 0),
        rating=float(d.get("rating", 0) or 0),
        sold_count=int(d.get("sold_count", 0) or 0),
        score=float(d.get("score", 0) or 0),
        description=d.get("description", "") or "",
        merchant_id=d.get("merchant_id", "") or "",
        min_people=int(d.get("min_people", 0) or 0),
        max_people=int(d.get("max_people", 0) or 0),
    )
    tags = d.get("tags") or []
    item.tags.extend(str(t) for t in tags)
    return item


class _RetrievalServicer(retrieval_pb2_grpc.RetrievalServiceServicer):
    """Thin adapter: proto <-> the shared handler functions."""

    def __init__(self, deals_fn, kb_fn, health_fn):
        self._deals = deals_fn
        self._kb = kb_fn
        self._health = health_fn

    def Health(self, request, context):
        h = self._health()
        resp = retrieval_pb2.HealthResponse(
            status=h.get("status", ""),
            deal_count=int(h.get("deal_count", 0)),
            kb_count=int(h.get("kb_count", 0)),
            backend=h.get("backend", ""),
            vector=h.get("vector", ""),
            tokenizer=h.get("tokenizer", ""),
            vector_model=h.get("vector_model", ""),
        )
        resp.corpora.extend(h.get("corpora", []))
        return resp

    def RetrieveDeals(self, request, context):
        r = self._deals(_deals_request_to_body(request))
        resp = retrieval_pb2.RetrieveDealsResponse(total=int(r.get("total", 0)))
        resp.items.extend(_deal_item_to_proto(d) for d in r.get("items", []))
        # Phase 3-C: audit fields only when the relaxation chain fired.
        if r.get("relaxed_level"):
            resp.relaxed_level = int(r["relaxed_level"])
            resp.effective_category = r.get("effective_category", "")
        return resp

    def RetrieveKb(self, request, context):
        body = {"query": request.query}
        if request.HasField("top_k"):
            body["top_k"] = request.top_k
        r = self._kb(body)
        resp = retrieval_pb2.RetrieveKbResponse()
        for p in r.get("passages", []):
            passage = retrieval_pb2.KbPassage(
                id=p.get("id", ""),
                category=p.get("category", ""),
                title=p.get("title", ""),
                content=p.get("content", ""),
                source=p.get("source", ""),
                score=float(p.get("score", 0) or 0),
            )
            passage.tags.extend(str(t) for t in (p.get("tags") or []))
            resp.passages.append(passage)
        return resp


def maybe_start_grpc(deals_fn, kb_fn, health_fn):
    """Start the gRPC server on GRPC_PORT if configured.

    Returns (server, port) or None. Never raises: any failure (no grpcio,
    port busy) degrades to HTTP-only with a warning, consistent with the
    project's degrade chain.
    """
    port_str = os.environ.get("GRPC_PORT", "")
    if not port_str:
        return None
    if grpc is None:
        print("[Retrieval] WARNING: GRPC_PORT set but grpcio not installed; "
              "gRPC front-end disabled (HTTP still available)")
        return None
    try:
        port = int(port_str)
    except ValueError:
        print(f"[Retrieval] WARNING: invalid GRPC_PORT={port_str!r}; gRPC disabled")
        return None
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    retrieval_pb2_grpc.add_RetrievalServiceServicer_to_server(
        _RetrievalServicer(deals_fn, kb_fn, health_fn), server)
    bound = server.add_insecure_port(f"0.0.0.0:{port}")
    if bound == 0:
        print(f"[Retrieval] WARNING: gRPC port {port} busy; gRPC disabled")
        return None
    server.start()
    print(f"[Retrieval] gRPC front-end on 0.0.0.0:{port} "
          "(same handlers as HTTP)")
    return server, port
