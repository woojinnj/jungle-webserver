#!/usr/bin/env bash
# Starts backend A/B, the proxy, and the demo controller. Open http://localhost:8080/
cd "$(dirname "$0")/.." || exit 1
[ -x build/webserver ] || make || exit 1
exec python3 demo/control.py
