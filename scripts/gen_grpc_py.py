#!/usr/bin/env python3
"""Regenerate the Python gRPC stubs for the retrieval pilot.

    python scripts/gen_grpc_py.py

Reads proto/retrieval.proto, writes retrieval_service/gen/retrieval_pb2.py and
retrieval_pb2_grpc.py. Deterministic for a fixed grpcio-tools version; the
generated files are committed so the runtime only needs grpcio.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "retrieval_service", "gen")


def main():
    os.makedirs(OUT, exist_ok=True)
    rc = subprocess.call(
        [sys.executable, "-m", "grpc_tools.protoc",
         "-I", os.path.join(ROOT, "proto"),
         f"--python_out={OUT}",
         f"--grpc_python_out={OUT}",
         "retrieval.proto"],
        cwd=ROOT)
    if rc == 0:
        print(f"stubs regenerated in {OUT}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
