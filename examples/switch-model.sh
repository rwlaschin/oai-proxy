#!/bin/sh
# Run by oai-proxy (as root) for POST /api/pull. Exit 2 = unknown model (the proxy answers Ollama's 404).
# Lists the models this box can serve, one per line, in /opt/llm/engine-models.
grep -qxF -- "$1" /opt/llm/engine-models || exit 2
printf '%s\n' "$1" > /opt/llm/llm-model && systemctl restart --no-block llm
