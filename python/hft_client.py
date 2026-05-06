#!/usr/bin/env python3
"""Minimal Python client for the C++ HFT Unix-domain command server."""

from __future__ import annotations

import argparse
import json
import socket
from dataclasses import dataclass
from typing import Any


@dataclass
class HftClient:
    socket_path: str = "/tmp/crypto_hft.sock"
    timeout_seconds: float = 2.0

    def _request(self, payload: dict[str, Any]) -> dict[str, Any]:
        message = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(self.timeout_seconds)
            sock.connect(self.socket_path)
            sock.sendall(message)
            chunks: list[bytes] = []
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break

        line = b"".join(chunks).split(b"\n", 1)[0]
        response = json.loads(line.decode("utf-8"))
        if not response.get("ok", False):
            raise RuntimeError(response.get("error", "unknown server error"))
        return response.get("result", {})

    def ping(self) -> dict[str, Any]:
        return self._request({"cmd": "ping"})

    def subscribe(self, *tickers: str) -> dict[str, Any]:
        if not tickers:
            raise ValueError("at least one ticker is required")
        return self._request({"cmd": "subscribe", "tickers": list(tickers)})

    def snapshot(self, ticker: str) -> dict[str, Any]:
        return self._request({"cmd": "snapshot", "ticker": ticker})


def main() -> int:
    parser = argparse.ArgumentParser(description="Crypto HFT local client")
    parser.add_argument("--socket", default="/tmp/crypto_hft.sock", help="Unix socket path")
    subcommands = parser.add_subparsers(dest="cmd", required=True)

    subcommands.add_parser("ping")

    subscribe = subcommands.add_parser("subscribe")
    subscribe.add_argument("tickers", nargs="+")

    snapshot = subcommands.add_parser("snapshot")
    snapshot.add_argument("ticker")

    args = parser.parse_args()
    client = HftClient(args.socket)
    if args.cmd == "ping":
        result = client.ping()
    elif args.cmd == "subscribe":
        result = client.subscribe(*args.tickers)
    else:
        result = client.snapshot(args.ticker)

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
