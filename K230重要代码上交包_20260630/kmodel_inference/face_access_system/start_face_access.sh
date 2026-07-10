#!/bin/sh

set -e

BASE_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$BASE_DIR"
mkdir -p db

GPIO_ARGS=""
if [ "$1" = "--gpio" ]; then
    GPIO_ARGS="--gpio --relay-pin ${RELAY_PIN:-27} --buzzer-pin ${BUZZER_PIN:-46}"
fi

./face_ai.elf \
    face_detect_640.kmodel 0.6 0.2 \
    face_recognize.kmodel 100 75 db 0 &
AI_PID=$!

./access_control.elf --log "$BASE_DIR/access.csv" $GPIO_ARGS &
CONTROL_PID=$!

sleep 1
./capture_display.elf 0

kill "$AI_PID" "$CONTROL_PID" 2>/dev/null || true
wait "$AI_PID" "$CONTROL_PID" 2>/dev/null || true
