// oai-proxy — Ollama /api/chat in, OpenAI /v1/chat/completions out (llama-server), so the
// worker and the Lab run unchanged against either engine. One fork per request, no deps.
//   cc -O2 -o oai-proxy worker/oai-proxy.c && ./oai-proxy 11434 127.0.0.1 8080
// /api/pull switches llama-server to the named model via /opt/llm/switch-model.sh (devbox only; a box
// without it answers 404 as before), so a worker's pull-on-miss (worker/ollama.js chatRound) works as on Ollama.
// ponytail: one model at a time — Ollama could hold two, so two tiers sharing a box flip it back and forth; chat
// during a switch gets a 502 instead of Ollama's queueing. /api/generate (bar the empty-prompt load), /api/embed,
// stream:false and message.images are not translated — add when a caller needs them.
#define _GNU_SOURCE // strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static const char *UH, *UP;
// Ollama lists a model in /api/ps from a request until its keep_alive runs out (default 5m, 0 = unload
// now, negative = forever), and the devbox idle watchdog reads that as "loaded". llama-server never
// unloads, so the proxy keeps the same clock: `until` is shared across the per-request forks and pushed
// out on every streamed line, so a long answer stays listed.
static volatile time_t *until;
static time_t ka = 300;
typedef struct { char *s; size_t n, cap; } buf;

static void put(buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
  memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void str(buf *b, const char *s) { put(b, s, strlen(s)); }
static void wr(int fd, const char *s, size_t n) {
  while (n) { ssize_t k = write(fd, s, n); if (k <= 0) _exit(0); s += k; n -= k; } // client gone: exiting closes upstream, which stops generation
}

// ---- JSON: scan spans in place, never build a tree ----
static const char *ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
static const char *skip(const char *p) {
  p = ws(p);
  if (*p == '"') { for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++; return *p ? p + 1 : p; }
  if (*p == '{' || *p == '[') {
    int d = 0;
    do { if (*p == '"') { p = skip(p); continue; } if (*p == '{' || *p == '[') d++; else if (*p == '}' || *p == ']') d--; p++; } while (d && *p);
    return p;
  }
  while (*p && !strchr(",}] \n\r\t", *p)) p++;
  return p;
}
static int member(const char **q, const char **k, size_t *kn, const char **v) {
  const char *p = ws(*q);
  if (*p == '{' || *p == ',') p = ws(p + 1);
  if (*p != '"') return 0;
  const char *e = skip(p); *k = p + 1; *kn = e - p - 2;
  p = ws(e); if (*p != ':') return 0;
  *v = ws(p + 1); *q = skip(*v); return 1;
}
static int elem(const char **q, const char **v) {
  const char *p = ws(*q);
  if (*p == '[' || *p == ',') p = ws(p + 1);
  if (!*p || *p == ']') return 0;
  *v = p; *q = skip(p); return 1;
}
static int is(const char *k, size_t kn, const char *s) { return kn == strlen(s) && !memcmp(k, s, kn); }
static const char *get(const char *o, const char *key) {
  const char *k, *v; size_t kn;
  if (!o || *(o = ws(o)) != '{') return NULL;
  while (member(&o, &k, &kn, &v)) if (is(k, kn, key)) return v;
  return NULL;
}
static void key(buf *o, int *n, const char *k, size_t kn) { if ((*n)++) str(o, ","); str(o, "\""); put(o, k, kn); str(o, "\":"); }
static void span(buf *o, const char *v) { put(o, v, skip(v) - v); }
// keep_alive as Ollama takes it: seconds as a number, or a duration string ("5m", "1h30m", "0").
static void keep_alive(const char *body) {
  const char *v = get(body, "keep_alive"); char *e; double t = 0, n;
  if (!v) return;
  if (*v != '"') { n = strtod(v, &e); if (e == v) return; t = n; }
  else for (v++; *v && *v != '"'; v = e) {
    n = strtod(v, &e); if (e == v) return;
    if (*e == 'h') t += n * 3600, e++;
    else if (!strncmp(e, "ms", 2)) t += n / 1000, e += 2;
    else if (*e == 'm') t += n * 60, e++;
    else if (*e == 's') t += n, e++;
    else if (*e == '"') t += n;
    else return;
  }
  ka = t < 0 ? (time_t)1 << 40 : (time_t)t;
}

// ---- request: Ollama body → OpenAI body ----
// OpenAI pairs a tool result to its call by id; Ollama pairs them by order. Ids are assigned in order.
static int ncall, ntool;
static void calls(buf *o, const char *a) {
  const char *q = a, *c; int n = 0; char id[80];
  str(o, "[");
  while (elem(&q, &c)) {
    const char *f = get(c, "function"), *nm = get(f, "name"), *ar = get(f, "arguments");
    snprintf(id, sizeof id, "%s{\"id\":\"call_%d\",\"type\":\"function\",\"function\":{\"name\":", n++ ? "," : "", ncall++);
    str(o, id);
    if (nm) span(o, nm); else str(o, "\"\"");
    str(o, ",\"arguments\":");
    if (ar && *ar == '"') span(o, ar);
    else if (ar) { // object → JSON string
      const char *e = skip(ar);
      str(o, "\"");
      for (; ar < e; ar++) {
        if (*ar == '"' || *ar == '\\') put(o, "\\", 1);
        put(o, (unsigned char)*ar < 0x20 ? " " : ar, 1);
      }
      str(o, "\"");
    } else str(o, "\"{}\"");
    str(o, "}}");
  }
  str(o, "]");
}
static void msgs(buf *o, const char *a) {
  const char *q = a, *m; int n = 0; char id[32];
  str(o, "[");
  while (elem(&q, &m)) {
    const char *mq = m, *k, *v; size_t kn; int c = 0, tool = 0;
    str(o, n++ ? ",{" : "{");
    while (member(&mq, &k, &kn, &v)) {
      if (is(k, kn, "tool_name")) continue;
      if (is(k, kn, "role")) tool = is(v + 1, skip(v) - v - 2, "tool");
      key(o, &c, k, kn);
      if (is(k, kn, "tool_calls") && *v == '[') calls(o, v); else span(o, v);
    }
    if (tool) { snprintf(id, sizeof id, "\"call_%d\"", ntool++); key(o, &c, "tool_call_id", 12); str(o, id); }
    str(o, "}");
  }
  str(o, "]");
}
static void request(buf *o, const char *body) {
  const char *q = ws(body), *k, *v; size_t kn; int n = 0;
  str(o, "{");
  while (member(&q, &k, &kn, &v)) {
    if (is(k, kn, "options") && *v == '{') { // sampler keys go top level; num_ctx is fixed at server start
      const char *oq = v, *ok, *ov; size_t okn;
      while (member(&oq, &ok, &okn, &ov)) {
        if (is(ok, okn, "num_ctx")) continue;
        if (is(ok, okn, "num_predict")) { if (*ov != '-' && *ov != '0') { key(o, &n, "max_tokens", 10); span(o, ov); } continue; }
        key(o, &n, ok, okn); span(o, ov);
      }
    } else if (is(k, kn, "think")) { key(o, &n, "chat_template_kwargs", 20); str(o, "{\"enable_thinking\":"); span(o, v); str(o, "}"); }
    else if (is(k, kn, "messages") && *v == '[') { key(o, &n, k, kn); msgs(o, v); }
    else {
      key(o, &n, k, kn); span(o, v);
      // token counts arrive only in a final usage chunk, and only when asked for
      if (is(k, kn, "stream") && *v == 't') { key(o, &n, "stream_options", 14); str(o, "{\"include_usage\":true}"); }
    }
  }
  str(o, "}");
}

// ---- upstream HTTP: buffered reader that undoes chunked encoding ----
typedef struct { int fd, chunked, started; long left; size_t i, n; char b[65536]; } rd;
static int rbyte(rd *r) {
  if (r->i == r->n) { ssize_t k = read(r->fd, r->b, sizeof r->b); if (k <= 0) return -1; r->n = k; r->i = 0; }
  return (unsigned char)r->b[r->i++];
}
static int rline(rd *r, buf *l, int (*next)(rd *)) {
  int ch; l->n = 0; put(l, "", 0);
  while ((ch = next(r)) >= 0 && ch != '\n') { char c = ch; put(l, &c, 1); }
  if (l->n && l->s[l->n - 1] == '\r') l->s[--l->n] = 0;
  return ch < 0 && !l->n ? -1 : 0;
}
static int bbyte(rd *r) {
  if (!r->chunked) return rbyte(r);
  if (!r->left) {
    buf l = {0};
    if (r->started) rline(r, &l, rbyte); // CRLF closing the previous chunk
    rline(r, &l, rbyte); r->left = strtol(l.s, 0, 16); r->started = 1; free(l.s);
    if (!r->left) return -1;
  }
  r->left--; return rbyte(r);
}
static int upstream(rd *r, const char *method, const char *path, const buf *body) {
  struct addrinfo h = { .ai_socktype = SOCK_STREAM }, *ai; char hd[512]; buf l = {0}; int st = 0, one = 1;
  if (getaddrinfo(UH, UP, &h, &ai)) return -1;
  r->fd = socket(ai->ai_family, SOCK_STREAM, 0);
  int bad = connect(r->fd, ai->ai_addr, ai->ai_addrlen); freeaddrinfo(ai);
  if (bad) return -1;
  setsockopt(r->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int hn = snprintf(hd, sizeof hd, "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                    method, path, UH, body ? body->n : 0);
  if (write(r->fd, hd, hn) != hn || (body && write(r->fd, body->s, body->n) != (ssize_t)body->n)) return -1;
  if (rline(r, &l, rbyte) < 0) return -1;
  sscanf(l.s, "HTTP/%*s %d", &st);
  while (rline(r, &l, rbyte) == 0 && l.n) if (strcasestr(l.s, "transfer-encoding: chunked")) r->chunked = 1;
  free(l.s); return st;
}
static void reply(int c, int st, const char *type, const char *body, size_t n) {
  char hd[160]; int hn = snprintf(hd, sizeof hd, "HTTP/1.1 %d X\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", st, type);
  wr(c, hd, hn); if (n) wr(c, body, n);
}
static void slurp(rd *r, buf *b) { int ch; put(b, "", 0); while ((ch = bbyte(r)) >= 0) { char x = ch; put(b, &x, 1); } }

// ---- response: OpenAI SSE → Ollama NDJSON ----
static buf tname[16], targs[16]; static int nt; static int cut;
static long gen_n, prompt_n; static double gen_ms, prompt_ms; // Ollama's eval_count / prompt_eval_count and durations
static void emit(int c, const char *field, const char *tok) {
  buf b = {0};
  str(&b, "{\"message\":{\"role\":\"assistant\",\"content\":");
  if (strcmp(field, "content")) str(&b, "\"\",\"thinking\":");
  span(&b, tok); str(&b, "},\"done\":false}\n");
  wr(c, b.s, b.n); free(b.s);
}
static void event(int c, const char *j) {
  const char *q = get(j, "choices"), *c0, *d, *v, *t, *s;
  if ((v = get(j, "usage")) && *v == '{') {
    if ((s = get(v, "completion_tokens"))) gen_n = atol(s);
    if ((s = get(v, "prompt_tokens"))) prompt_n = atol(s);
  }
  if ((v = get(j, "timings")) && *v == '{') {
    if ((s = get(v, "predicted_ms"))) gen_ms = atof(s);
    if ((s = get(v, "prompt_ms"))) prompt_ms = atof(s);
  }
  if (!q || !elem(&q, &c0)) return;
  if ((v = get(c0, "finish_reason")) && !strncmp(v, "\"length\"", 8)) cut = 1;
  if (!(d = get(c0, "delta"))) return;
  if (((v = get(d, "reasoning_content")) || (v = get(d, "reasoning"))) && *v == '"' && v[1] != '"') emit(c, "thinking", v);
  if ((v = get(d, "content")) && *v == '"' && v[1] != '"') emit(c, "content", v);
  if ((q = get(d, "tool_calls")) && *q == '[')
    while (elem(&q, &t)) {
      int i = (v = get(t, "index")) ? atoi(v) : 0;
      const char *f = get(t, "function");
      if (i < 0 || i > 15 || !f) continue;
      if (i >= nt) nt = i + 1;
      if ((s = get(f, "name")) && *s == '"') put(&tname[i], s + 1, skip(s) - s - 2);
      if ((s = get(f, "arguments")) && *s == '"') put(&targs[i], s + 1, skip(s) - s - 2);
    }
}
static void finish(int c) {
  buf b = {0};
  str(&b, "{\"message\":{\"role\":\"assistant\",\"content\":\"\"");
  if (nt) str(&b, ",\"tool_calls\":[");
  for (int i = 0; i < nt; i++) {
    str(&b, i ? ",{\"function\":{\"name\":\"" : "{\"function\":{\"name\":\"");
    put(&b, tname[i].s ? tname[i].s : "", tname[i].n);
    str(&b, "\",\"arguments\":");
    if (!targs[i].n) str(&b, "{}");
    for (size_t k = 0; k < targs[i].n; k++) { // the arguments string, unescaped one level back into an object
      char ch = targs[i].s[k];
      if (ch == '\\' && k + 1 < targs[i].n) {
        ch = targs[i].s[++k];
        if (ch == 'u') { str(&b, "\\u"); continue; }
        if (ch == 'n' || ch == 't' || ch == 'r') ch = ' ';
      }
      put(&b, &ch, 1);
    }
    str(&b, "}}");
  }
  if (nt) str(&b, "]");
  str(&b, cut ? "},\"done\":true,\"done_reason\":\"length\"" : "},\"done\":true,\"done_reason\":\"stop\"");
  if (gen_n) {
    char n[160];
    snprintf(n, sizeof n, ",\"eval_count\":%ld,\"prompt_eval_count\":%ld,\"eval_duration\":%.0f,\"prompt_eval_duration\":%.0f",
             gen_n, prompt_n, gen_ms * 1e6, prompt_ms * 1e6);
    str(&b, n);
  }
  str(&b, "}\n");
  wr(c, b.s, b.n);
}

static void chat(int c, const char *body) {
  buf o = {0}, l = {0}; rd *r = calloc(1, sizeof *r);
  request(&o, body);
  int st = upstream(r, "POST", "/v1/chat/completions", &o);
  if (st < 0) { reply(c, 502, "application/json", "{\"error\":\"oai-proxy: upstream unreachable\"}", 43); return; }
  if (st != 200) { slurp(r, &l); reply(c, st, "application/json", l.s, l.n); return; }
  reply(c, 200, "application/x-ndjson", "", 0);
  while (rline(r, &l, bbyte) == 0 && (*until = time(0) + ka))
    if (!strncmp(l.s, "data:", 5) && strcmp(ws(l.s + 5), "[DONE]")) event(c, ws(l.s + 5));
  finish(c);
}
// GPU memory in use, in bytes. worker/gpuGate.js refuses to subscribe unless /api/ps shows the
// model in VRAM — llama-server has no such field, so it is read off the GPU itself. 0 = no GPU.
static long long vram(void) {
  long long mib = 0, v; FILE *p = popen("nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null", "r");
  if (!p) return 0;
  while (fscanf(p, "%lld", &v) == 1) mib += v;
  pclose(p); return mib << 20;
}
static void models(int c, int ps) {
  buf l = {0}, o = {0}; rd *r = calloc(1, sizeof *r); char sz[48] = "";
  int st = upstream(r, "GET", "/v1/models", NULL);
  if (st < 0) { reply(c, 502, "application/json", "{\"error\":\"oai-proxy: upstream unreachable\"}", 43); return; }
  slurp(r, &l);
  if (ps && time(0) >= *until) { reply(c, 200, "application/json", "{\"models\":[]}", 13); return; }
  if (ps) snprintf(sz, sizeof sz, ",\"size_vram\":%lld", vram());
  const char *q = get(l.s, "data"), *m, *id; int n = 0;
  str(&o, "{\"models\":[");
  while (q && elem(&q, &m)) if ((id = get(m, "id"))) {
    str(&o, n++ ? ",{\"name\":" : "{\"name\":"); span(&o, id); str(&o, ",\"model\":"); span(&o, id); str(&o, sz); str(&o, "}");
  }
  str(&o, "]}");
  reply(c, 200, "application/json", o.s, o.n);
}

// Ollama answers 404 for a model it does not hold; llama-server would silently answer with its own.
// `unsure` is the answer when there is no list to judge by (llama-server down or mid-restart).
static int listed(const char *m, int unsure) {
  buf l = {0}; rd *r = calloc(1, sizeof *r); const char *q, *e, *id;
  if (upstream(r, "GET", "/v1/models", NULL) < 0) return unsure;
  slurp(r, &l);
  if (!(q = get(l.s, "data"))) return unsure;
  for (; q && elem(&q, &e); )
    if ((id = get(e, "id")) && skip(id) - id == skip(m) - m && !memcmp(id, m, skip(id) - id)) return 1;
  return 0;
}
static int known(const char *m) { return listed(m, 1); } // unsure: let the request itself report the 502
static int serving(void) { // processes named exactly oai-proxy: the parent plus one per request in flight
  int n = 0; FILE *p = popen("pgrep -xc oai-proxy", "r");
  if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); }
  return n;
}
// The watchdog counts processes named exactly oai-proxy beyond the parent as generations in flight, as
// Ollama's own signals only see generations. A child takes the name only once it is serving one, so a
// dashboard poll or a client that never finishes sending does not keep the box alive.
static void named(const char *n) {
#ifdef __linux__
  prctl(PR_SET_NAME, n);
#else
  (void)n;
#endif
}

#define SWITCH "/opt/llm/switch-model.sh"
static void pull(int c, const char *b) {
  const char *m = get(b, "model"); char name[200]; size_t n;
  if (!m || *m != '"') m = get(b, "name");
  if (!m || *m != '"' || (n = skip(m) - m - 2) == 0 || n >= sizeof name) { reply(c, 400, "application/json", "{\"error\":\"model is required\"}", 29); return; }
  memcpy(name, m + 1, n); name[n] = 0;
  for (size_t i = 0; i < n; i++) if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-", name[i])) { reply(c, 400, "application/json", "{\"error\":\"invalid model name\"}", 30); return; }
  if (listed(m, 0)) { reply(c, 200, "application/json", "{\"status\":\"success\"}\n", 21); return; }
  if (access(SWITCH, X_OK)) { reply(c, 404, "application/json", "{\"error\":\"oai-proxy: not supported\"}", 36); return; }
  named("oai-proxy"); // busy to the idle watchdog for the whole switch
  for (int i = 0; i < 600 && serving() > 2; i++) sleep(1); // let generations in flight on the old model finish
  signal(SIGCHLD, SIG_DFL); // main ignores it, which would auto-reap the script before waitpid reads its status
  int st; pid_t p = fork();
  if (!p) { execl(SWITCH, SWITCH, name, (char *)0); _exit(127); }
  if (p < 0 || waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || (WEXITSTATUS(st) && WEXITSTATUS(st) != 2)) { reply(c, 500, "application/json", "{\"error\":\"oai-proxy: switch failed\"}", 36); return; }
  if (WEXITSTATUS(st) == 2) { reply(c, 404, "application/json", "{\"error\":\"pull model manifest: file does not exist\"}", 52); return; }
  for (int i = 0; i < 1800; i++) { // download + load; Ollama's pull also blocks until done
    *until = time(0) + ka;
    if (listed(m, 0)) { reply(c, 200, "application/json", "{\"status\":\"success\"}\n", 21); return; }
    sleep(2);
  }
  reply(c, 504, "application/json", "{\"error\":\"oai-proxy: model did not load\"}", 41);
}

static void serve(int c) {
  buf r = {0}; char tmp[65536], method[8] = "", path[256] = ""; ssize_t k; char *he; int one = 1;
  struct timeval tv = { 60, 0 };
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); // a stalled client frees its fork
  while (!(he = r.s ? strstr(r.s, "\r\n\r\n") : NULL)) { if ((k = read(c, tmp, sizeof tmp)) <= 0) return; put(&r, tmp, k); }
  size_t hl = he + 4 - r.s; *he = 0;
  char *cl = strcasestr(r.s, "content-length:");
  size_t len = cl ? strtoul(cl + 15, 0, 10) : 0;
  while (r.n < hl + len) { if ((k = read(c, tmp, sizeof tmp)) <= 0) return; put(&r, tmp, k); }
  sscanf(r.s, "%7s %255s", method, path);
  const char *b = r.s + hl, *pr, *m = get(b, "model");
  int post = !strcmp(method, "POST"), ch = post && !strcmp(path, "/api/chat");
  // Ollama's load-only call (worker/gpuGate.js): an empty prompt loads the model and returns at once.
  int load = post && !strcmp(path, "/api/generate") && (!(pr = get(b, "prompt")) || !strncmp(pr, "\"\"", 2));
  if ((ch || load) && (!m || *m != '"')) { reply(c, 400, "application/json", "{\"error\":\"model is required\"}", 29); return; }
  if ((ch || load) && !known(m)) {
    char e[320]; int n = snprintf(e, sizeof e, "{\"error\":\"model \\\"%.*s\\\" not found, try pulling it first\"}", (int)(skip(m) - m - 2), m + 1);
    reply(c, 404, "application/json", e, n < (int)sizeof e ? n : (int)sizeof e - 1); return;
  }
  if (ch || load) { named("oai-proxy"); keep_alive(b); *until = time(0) + ka; }
  if (ch) chat(c, b);
  else if (load) reply(c, 200, "application/json", "{\"response\":\"\",\"done\":true,\"done_reason\":\"load\"}", 48);
  else if (!strcmp(path, "/api/tags") || !strcmp(path, "/api/ps")) models(c, !strcmp(path, "/api/ps"));
  else if (post && !strcmp(path, "/api/pull")) pull(c, b);
  else if (!strcmp(path, "/api/version")) reply(c, 200, "application/json", "{\"version\":\"oai-proxy\"}", 23);
  else reply(c, 404, "application/json", "{\"error\":\"oai-proxy: unknown route\"}", 36);
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: oai-proxy <listen-port> <upstream-host> <upstream-port>\n"); return 2; }
  UH = argv[2]; UP = argv[3];
  until = mmap(0, sizeof *until, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  signal(SIGCHLD, SIG_IGN); signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[1])) };
  if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 128)) { perror("oai-proxy"); return 1; }
  for (;;) {
    int c = accept(s, 0, 0);
    if (c < 0) continue;
    if (!fork()) { close(s); named("oai-proxy-io"); serve(c); _exit(0); }
    close(c);
  }
}
