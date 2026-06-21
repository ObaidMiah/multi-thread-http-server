# MultiThreadHttpServer

A multithreaded HTTP/1.1 server written from scratch in C++ on top of raw
POSIX sockets — no web framework, no external dependencies. It serves static
files, exposes a small JSON REST API, and handles many concurrent connections
through a fixed thread pool.

This is a learning project focused on the internals of how an HTTP server
actually works: the socket lifecycle, request parsing, connection management,
and concurrency.

## Features

- **Thread pool** — a fixed pool of worker threads pulls accepted connections
  off a shared, mutex-protected queue (no thread-per-request blowup).
- **HTTP/1.1 keep-alive** — multiple requests are served over one connection,
  with a socket receive timeout and `Connection: close`/`keep-alive` honored.
- **Static file serving** from the `www/` directory, with content-type
  detection by file extension.
- **JSON REST API** with method-aware route handlers.
- **Request parsing** — request line, query-string stripping, headers, and
  `Content-Length` bodies, including handling of partial reads.
- **Graceful shutdown** — `SIGINT` (Ctrl-C) stops accepting new connections,
  drains the queue, joins all workers, and exits cleanly.
- **Basic hardening** — path-traversal rejection (`..`), method allow-list,
  a 1 MB request-body cap, JSON output escaping, full-write `send` handling,
  and structured error responses (400 / 404 / 405 / 413 / 500).

## Build

Requires a C++20 compiler (clang or g++) and pthreads.

```sh
make          # build ./server
make run      # build and run
make clean    # remove the binary
```

Or directly:

```sh
c++ -std=c++20 -O2 -pthread server.cpp -o server
```

## Run

```sh
./server          # listens on port 8080 (default)
./server 9000     # listens on a port of your choice
```

Open <http://localhost:8080> in a browser to see the demo page served from
`www/`. Press **Ctrl-C** to shut the server down cleanly.

## Endpoints

### Static files

Any path that isn't a registered API route is served from `www/`.

| Path        | Serves              |
| ----------- | ------------------- |
| `/`         | `www/index.html`    |
| `/style.css`| `www/style.css`     |
| `/script.js`| `www/script.js`     |

### JSON API

| Method | Path          | Description                          |
| ------ | ------------- | ------------------------------------ |
| `GET`  | `/api/health` | Health check — `{"status":"ok"}`     |
| `GET`  | `/api/time`   | Current server time as epoch seconds |
| `POST` | `/api/echo`   | Echoes the request body back as JSON |

Examples:

```sh
curl http://localhost:8080/api/health
curl http://localhost:8080/api/time
curl -X POST http://localhost:8080/api/echo -d 'hello'
```

## Testing

A smoke-test script builds the server, starts it on a test port, exercises
every endpoint and the error paths (404, 405, 413, path traversal, query-string
routing, JSON escaping), then shuts it down via `SIGINT`:

```sh
./test.sh
```

It exits non-zero if any check fails. Requires `curl` and `python3` (used to
validate JSON responses).

## Project layout

```
server.cpp     # the entire server
www/           # static assets served by the server
Makefile       # build / run / clean
test.sh        # endpoint smoke tests
DESIGN.md      # architecture and design notes
```

## Architecture

See [DESIGN.md](DESIGN.md) for how the accept loop, thread pool, connection
lifecycle, and request pipeline fit together.
