# oai-proxy

Make llama-server look like Ollama. Ollama's `/api/chat` in, OpenAI `/v1/chat/completions` out, streamed back as
Ollama NDJSON. One C file, no dependencies, so code written for Ollama runs unchanged on llama-server.

```bash
make            # or: cc -O2 -o oai-proxy oai-proxy.c
./oai-proxy 11434 127.0.0.1 8080   # listen on 11434, forward to llama-server on 127.0.0.1:8080
```

Usage: `oai-proxy <listen-port> <upstream-host> <upstream-port>`. It listens on all interfaces; put it behind a
firewall.

## What it translates

| Ollama | llama-server |
|---|---|
| `POST /api/chat` (streaming) | `POST /v1/chat/completions` with `stream_options.include_usage` |
| `think: true/false` | `chat_template_kwargs.enable_thinking` |
| `options.*` sampler keys | top-level keys; `num_predict` → `max_tokens`; `num_ctx` dropped (fixed at server start) |
| tool results paired by order | `tool_call_id` assigned in order |
| reply: `message.content`, `message.thinking`, `tool_calls`, `done_reason`, `eval_count`, `prompt_eval_count`, durations | from SSE deltas, `reasoning_content`, `usage` and `timings` |
| `GET /api/tags`, `GET /api/ps` | `GET /v1/models`; `/api/ps` follows Ollama's `keep_alive` clock and reports `size_vram` from `nvidia-smi` |
| `POST /api/generate` with an empty prompt (load only) | answered at once |
| `POST /api/pull` | runs `/opt/llm/switch-model.sh <model>` if it exists (exit 2 = unknown model), else 404 |
| `GET /api/version` | `{"version":"oai-proxy"}` |

A model llama-server doesn't list gets Ollama's 404 instead of an answer from whatever model is loaded.

## How it runs

- One fork per request. If the client disconnects, the child exits and closes the upstream connection, which stops
  generation.
- A child serving a generation renames itself `oai-proxy`, so `pgrep -xc oai-proxy` minus one counts generations in
  flight (useful for an idle watchdog).

## Switching models with `/api/pull`

llama-server serves one model, so `/api/pull` restarts it on another. The proxy runs
`/opt/llm/switch-model.sh <model>`, waits for generations in flight to finish, then waits until llama-server lists
the new model. Exit code 2 means "unknown model" and becomes Ollama's 404. Without the script, `/api/pull` answers
404.

`examples/` has a working setup for a systemd box:

| file | goes to | does |
|---|---|---|
| `switch-model.sh` | `/opt/llm/switch-model.sh` | checks the model is in `engine-models`, writes it to `/opt/llm/llm-model`, restarts `llm` |
| `engine-models` | `/opt/llm/engine-models` | the models this box may serve, one per line |
| `run-engine.sh` | `/opt/llm/run-engine.sh` | starts llama-server on the model in `/opt/llm/llm-model`, from `/opt/llm/models/<name>.gguf` |
| `llm.service` | `/etc/systemd/system/` | runs `run-engine.sh` |
| `llm-proxy.service` | `/etc/systemd/system/` | runs `oai-proxy 11434 127.0.0.1 8080` |

```bash
sudo mkdir -p /opt/llm/models
sudo install -m 755 examples/switch-model.sh examples/run-engine.sh /opt/llm/
sudo install -m 644 examples/engine-models /opt/llm/
sudo install -m 755 oai-proxy /usr/local/bin/
sudo install -m 644 examples/*.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now llm llm-proxy
```

Edit `run-engine.sh` for your default model and flags. GGUF file names replace `/` and `:` with `_`, so
`qwen3.8:27b` is `/opt/llm/models/qwen3.8_27b.gguf`.

## Not translated

`/api/generate` with a prompt, `/api/embed`, `stream: false`, and `message.images`. One model at a time.

## License

Apache-2.0
