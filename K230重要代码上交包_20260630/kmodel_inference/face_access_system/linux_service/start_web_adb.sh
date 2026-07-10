#!/bin/sh

export FACE_ACCESS_ADB=1
export FACE_ACCESS_ROOT="${FACE_ACCESS_ROOT:-/sharefs/face_access}"
export FACE_ACCESS_PORT="${FACE_ACCESS_PORT:-8080}"
exec python3 "$(dirname "$0")/server.py"
