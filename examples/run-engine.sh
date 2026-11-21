#!/bin/sh
# Started by llm.service. Serves the model named in /opt/llm/llm-model (set by switch-model.sh),
# else the default below. Each model's GGUF lives at /opt/llm/models/<name with / and : replaced>.gguf.
M=$(cat /opt/llm/llm-model 2>/dev/null || echo "qwen3.8:27b")
G=/opt/llm/models/$(printf '%s' "$M" | tr '/:' '__').gguf
[ -s "$G" ] || { echo "no GGUF for $M at $G" >&2; exit 1; }
exec llama-server -m "$G" --alias "$M" --host 127.0.0.1 --port 8080 -ngl 999 --jinja \
  -np 16 -c $((8192 * 16)) -kvu -b 2048 -ub 2048 --backend-sampling
