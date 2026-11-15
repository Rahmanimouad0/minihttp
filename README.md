# minihttp

[![CI](https://github.com/Rahmanimouad0/minihttp/actions/workflows/ci.yml/badge.svg)](https://github.com/Rahmanimouad0/minihttp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A tiny HTTP/1.1 static file server, written from scratch in C with Linux
sockets and POSIX threads. It uses only libc: no frameworks and no event
libraries. It's about 1,300 lines of C, including comments.

```
Browser / curl
      │
      ▼
  TCP socket        socket → bind → listen → poll → accept4        src/server.c
      │             one thread per connection
      ▼
  HTTP parser       request line + headers, strict RFC 9112          src/http.c
      │
      ▼
  Request handler   decode → normalize → realpath → inside root?     src/handler.c
      │
      ▼
  HTTP response     status line + headers, body via sendfile()       src/http.c
```

## Features

- **TCP server:** `socket()`, `bind()`, `listen()` and `accept4()`, with `SO_REUSEADDR` for instant restarts.
- **HTTP/1.1 parser:** request line, headers and the `Host` rule. It rejects malformed input with `400`.
- **Methods:** `GET` and `HEAD`. Any other method gets `405` with an `Allow` header.
- **Static files:** Content-Type chosen by file extension, `index.html` for directories, and a `301` redirect for `/dir` → `/dir/`.
- **Status codes:** 200, 301, 400, 403, 404, 405, 408, 414, 431, 500, 503 and 505.
- **Response headers:** `Date`, `Server`, `Content-Type`, `Content-Length`, `Last-Modified` and `Connection`.
- **Keep-alive and pipelining:** a client can send several requests over one connection, even back to back.
- **Concurrency:** one thread per client, capped by `--max-clients`. Past the cap, clients get an immediate `503`.
- **Graceful shutdown:** on `SIGINT`/`SIGTERM` the server stops accepting, lets in-flight responses finish, then exits.
- **Zero-copy:** `sendfile()` sends file data from the page cache straight to the socket.
- **Hardening:** blocks path traversal (including percent-encoded), symlinks that lead outside the root, NUL and control bytes, oversized headers, and slow clients.

## Build and run

You need Linux (WSL works), `gcc` or `clang`, and `make`. The tests also need `curl`.

```bash
make
./minihttp --port 8080 --root ./public
```

```
[2026-09-25 03:23:27] listening on http://0.0.0.0:8080, serving /home/you/minihttp/public (max 256 clients)
[2026-09-25 03:23:27] press Ctrl+C to stop
```

Then open <http://localhost:8080/> in a browser, or:

```bash
curl -i http://localhost:8080/index.html
```

```
HTTP/1.1 200 OK
Server: minihttp/1.0
Date: Thu, 24 Sep 2026 19:23:27 GMT
Content-Type: text/html; charset=utf-8
Content-Length: 1309
Connection: keep-alive
Last-Modified: Thu, 24 Sep 2026 19:21:22 GMT

<!DOCTYPE html>
...
```

| Option | Default | |
|---|---|---|
| `-p`, `--port PORT` | `8080` | TCP port to listen on |
| `-r`, `--root DIR` | `./public` | document root |
| `-c`, `--max-clients N` | `256` | concurrent connection limit |
| `-h`, `--help` | | show usage |

### Try the error paths

```bash
curl -i http://localhost:8080/nope.html                         # 404 Not Found
curl -i http://localhost:8080/docs                              # 301 → /docs/
curl -i -X POST http://localhost:8080/                          # 405 Method Not Allowed
curl -i --path-as-is http://localhost:8080/../../etc/passwd     # 400 Bad Request
printf 'HELLO\r\n\r\n' | nc localhost 8080                      # 400 Bad Request
printf 'GET / HTTP/2.0\r\nHost: x\r\n\r\n' | nc localhost 8080  # 505
```

The server logs every response:

```
[2026-09-25 03:23:27] 127.0.0.1 "GET /index.html HTTP/1.1" 200 1309
[2026-09-25 03:23:27] 127.0.0.1 "GET /nope.html HTTP/1.1" 404 130
[2026-09-25 03:23:27] 127.0.0.1 "GET /docs HTTP/1.1" 301 146
[2026-09-25 03:23:27] 127.0.0.1 "-" 400 134
[2026-09-25 03:23:27] 127.0.0.1 "GET /../../etc/passwd HTTP/1.1" 400 134
[2026-09-25 03:23:27] received SIGINT, shutting down
[2026-09-25 03:23:27] bye
```

## Tests

```bash
make test        # unit tests + end-to-end tests
make sanitize    # the same, built with AddressSanitizer + UBSan
make tsan        # the same, built with ThreadSanitizer
```

After `make sanitize` or `make tsan`, run `make clean` to get a normal build back.

GitHub Actions runs all three on every push ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)),
and the normal build treats any compiler warning as an error.

- **Unit tests** (`tests/test_http.c`, 95 checks) cover the parser on valid and invalid
  requests, percent-decoding, path normalization, keep-alive rules and MIME types.
- **End-to-end tests** (`tests/integration.sh`, 45 checks) start the real binary against a
  temporary document root and use `curl` and raw TCP (bash `/dev/tcp`). They cover every
  status code, the headers, redirects, traversal and symlink escapes, keep-alive,
  pipelining, 200 parallel requests, 8 parallel 32 MB downloads checked by hash, the
  connection limit, and both shutdown signals.

## How it works

### Request lifecycle

1. The main thread `poll()`s two descriptors: the listening socket and a
   `signalfd`. A new connection is accepted with `accept4()`. The client
   gets receive and send timeouts plus `TCP_NODELAY`, and is handed to a new
   detached thread with a 256 KiB stack.
2. The thread `recv()`s into an 8 KiB buffer until it sees `\r\n\r\n`. A head
   that doesn't fit gets `431`. A client that starts a request and stalls gets
   `408` after 5 s.
3. `http_parse_request()` splits the head in place, with no allocations. It
   checks every token against the RFC grammar.
4. The handler percent-decodes the path, then resolves `.` and `..`. A path
   that climbs above `/` is rejected. It joins the result onto the root and
   calls `realpath()`, which resolves symlinks, then checks that the result is
   still inside the root.
5. The headers are written with `MSG_MORE` so they share a TCP segment with
   the start of the body. The body goes out with `sendfile()`.
6. With keep-alive, any bytes left over from a pipelined request move to the
   front of the buffer, and the loop continues.

### Concurrency

Each connection gets its own thread, which keeps the code sequential and easy
to follow. A fixed-size registry of client slots, guarded by a mutex, enforces
`--max-clients`. Past the cap, the main thread answers `503` right away and
never spawns a thread. Socket timeouts limit how long a slow or silent client
can hold a slot.

### Graceful shutdown

- `SIGINT` and `SIGTERM` are blocked in every thread and read from a
  `signalfd`, so there are no async-signal-safety pitfalls and no `EINTR`
  surprises in the workers. `SIGPIPE` is ignored.
- On a signal, the listener is closed so new connections are refused. Next, a
  `stopping` flag is set, so responses in progress are sent with
  `Connection: close`.
- Every registered client socket gets `shutdown(fd, SHUT_RD)`. That wakes
  threads blocked on an idle keep-alive connection right away, and responses
  still being written are unaffected.
- The main thread waits on a condition variable until the active count
  reaches zero, for up to 10 s, then frees everything and exits `0`.
- Threads leave the registry before calling `close()`, so the shutdown code
  can't touch a descriptor number that has since been reused.

Before closing, a worker half-closes the socket (`SHUT_WR`) and briefly drains
any remaining input. Closing a socket with unread data makes Linux send a TCP
RST, which can destroy an error response before the client reads it.

## Project layout

```
minihttp/
├── .github/workflows/ci.yml   build + tests on every push
├── LICENSE
├── Makefile
├── src/
│   ├── main.c        CLI parsing, document root checks
│   ├── server.c/.h   sockets, accept loop, threads, signals, shutdown
│   ├── http.c/.h     request parser, path decoding and normalization, response writers
│   ├── handler.c/.h  maps a request to a file under the root and serves it
│   ├── mime.c/.h     extension → Content-Type
│   └── log.c/.h      thread-safe timestamped logging
├── public/           demo site served by default
└── tests/
    ├── test_http.c   unit tests
    └── integration.sh end-to-end tests
```

## Limitations and ideas

These are deliberately left out to keep the server small:

- IPv4 only, and origin-form request targets only (`/path`, not `http://host/path`).
- No request bodies: a request that carries one is answered and its connection closed.
- No directory listings, range requests, conditional requests or compression.

Natural next steps: `If-Modified-Since` → `304`, `Range` → `206`, an
`epoll`-based event loop or a thread pool instead of one thread per client,
IPv6 dual-stack, and TLS through OpenSSL.

## License

[MIT](LICENSE)
