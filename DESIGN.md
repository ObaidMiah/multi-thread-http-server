# Design

This document explains how the server works and why it's built the way it is.
It assumes you know what HTTP and threads are, but not much about low-level
network programming. The goal is that you could read this, then read
`server.cpp`, and understand the whole thing.

## What this project is

A web server, written from scratch in C++. Normally you'd reach for a framework
(Express, Flask, Spring, etc.) that hides all the networking. This project does
the opposite — it talks to the operating system's networking directly so you can
see what a framework actually does for you under the hood.

It does three things:

1. Serves files from the `www/` folder (HTML, CSS, JS) to a browser.
2. Answers a few JSON API endpoints (`/api/health`, `/api/time`, `/api/echo`).
3. Handles many users at the same time without falling over.

## The core problem: handling many users at once

A server has to deal with many clients connecting at the same time. If it
handled them one at a time, everyone would wait in a single-file line. So we
need to do work in parallel — that means threads.

The naive approach is "spin up a new thread for every connection." That works
until a few thousand people connect at once and you've created a few thousand
threads, which eats all your memory and grinds the machine to a halt.

This project uses a **thread pool** instead: create a small, fixed number of
worker threads up front (8 of them) and reuse them. Think of it like a help
desk. Instead of hiring a new employee for every customer who walks in, you have
8 employees and a waiting line. When an employee finishes with one customer,
they grab the next person in line. No matter how busy it gets, you never have
more than 8 employees, so the building never gets overwhelmed.

## How the pieces fit together

```
                   new connection
   browser ───────────────────────► main thread (the "host")
                                          │
                                          │ adds connection to the line
                                          ▼
                              ┌───────────────────────┐
                              │   waiting line (queue)│
                              └───────────────────────┘
                                          │ each worker grabs the next one
                 ┌────────────┬───────────┼───────────┬────────────┐
                 ▼            ▼            ▼           ▼            ▼
              worker 0     worker 1     worker 2    ...         worker 7
                 │
                 ▼
        handle the request:  read it ─► figure out what's wanted ─► reply
```

### The "host" — the main thread

One thread does nothing but greet new connections. When someone connects, it
doesn't try to serve them itself — it just writes their connection down on the
waiting line and immediately goes back to greeting the next person. This is
important: because the host never gets stuck doing slow work, it can accept new
visitors as fast as they arrive.

### The waiting line (the queue)

This is just a list of connections waiting to be handled. The tricky part is
that multiple threads touch it at the same time — the host is adding to it while
workers are removing from it. If two threads grab the same item at once, you get
corruption or crashes.

Two tools solve this:

- A **mutex** (short for "mutual exclusion") — basically a key. Only the thread
  holding the key can touch the line. Everyone else waits their turn. This
  guarantees one-at-a-time access so nothing gets corrupted.
- A **condition variable** — a way for workers to sleep until there's actually
  work. Without it, idle workers would spin in a loop constantly asking "is
  there anything yet? anything yet?" and burn CPU for nothing. Instead they go to
  sleep, and the host taps one on the shoulder when a new connection arrives.

### The workers

Each of the 8 workers runs the same loop forever: sleep until there's work, grab
one connection from the line, fully handle it, then go back for the next. One
worker owns one connection from start to finish.

## Handling one connection

Once a worker picks up a connection, here's what it does (this all lives in
`handle_client`):

**1. Set a timeout.** Modern HTTP lets a browser keep a connection open to send
several requests over it (this is "keep-alive" — it's faster than reconnecting
every time). But that means a connection could sit there doing nothing,
occupying one of our 8 workers indefinitely. So we set a 5-second timeout: if the
client goes quiet for too long, we give up and free the worker.

**2. Read the request.** Here's a subtlety that trips up almost everyone the
first time: when you read from a network connection, you are *not* guaranteed to
get the whole message in one read. A request might arrive in pieces. So the code
reads in a loop, sticking the pieces together, until it sees the marker that
means "headers are done" (a blank line, `\r\n\r\n`). Then, if the request has a
body (like a POST), it keeps reading until it has received the number of bytes
the client said it would send (the `Content-Length` header).

**3. Understand the request.** An HTTP request is just text. The first line says
the method, path, and version (e.g. `GET /api/health HTTP/1.1`). The lines after
that are headers (`key: value`). The code splits this apart into something it can
work with, strips any query string off the path (so `/api/health?x=1` is treated
as `/api/health`), and does basic validation — rejecting malformed requests,
blocking paths containing `..` (so nobody can escape the `www/` folder and read
your system files), refusing bodies larger than 1 MB, and only allowing `GET`
and `POST`.

**4. Decide whether to keep the connection open.** Based on the HTTP version and
the `Connection` header, the worker decides whether to reuse this connection for
another request or close it when done.

**5. Reply, and maybe loop.** It figures out the response (see below), sends it,
and if the connection should stay open, loops back to step 2 for the next
request. Otherwise it closes the connection and the worker goes back to the
waiting line.

## Figuring out the response: routing

When a request comes in, how does the server know what to do with it? It keeps a
lookup table that maps a path to a piece of code that handles it. For example,
`/api/health` maps to a small function that returns `{"status":"ok"}`.

- If the requested path is in the table, run its handler.
- If it's *not* in the table, treat it as a request for a file and try to serve
  the matching file from the `www/` folder (or return a 404 if there's no such
  file).

This is a clean way to organize things: API endpoints are entries in the table,
and everything else is "just serve the file." Adding a new endpoint is as simple
as adding one entry to the table.

There are a few small helper functions for sending replies — a general one, one
specifically for JSON responses, and one for error responses (like 404 Not Found
and 400 Bad Request) — so the code that builds the raw HTTP response text isn't
duplicated everywhere. They all send through one routine that loops until every
byte is written, because a single network "send" isn't guaranteed to push out
the whole response at once. Any text that goes into a JSON reply is run through
an escaping step first, so characters like quotes or newlines can't produce
broken JSON.

## Shutting down cleanly

Pressing Ctrl-C doesn't just kill the process. The server catches that signal and
shuts down in an orderly way:

1. A shared "are we still running?" flag is flipped to off, and the front-door
   socket is closed — which unfreezes the main thread so it stops accepting new
   connections.
2. All worker threads are woken up. Each one finishes any connections still
   waiting in the line, and once the line is empty, exits.
3. The main thread waits for every worker to finish before the program exits.

The result is that in-flight work isn't cut off mid-response. (The one exception:
a worker parked waiting on a slow client can take up to the 5-second receive
timeout to notice and exit.)

## Quick reference

| What it handles        | How                                          |
| ---------------------- | -------------------------------------------- |
| Greeting new clients   | one dedicated thread that never does slow work |
| Sharing work safely    | a queue protected by a mutex + condition variable |
| Doing the work         | a fixed pool of 8 reusable worker threads    |
| Hung/idle connections  | a 5-second receive timeout per connection    |
| Ctrl-C                 | flag + socket close, then drain and join workers |

## What's intentionally left undone

This is a learning project, so some things are deliberately left as future work.
If you wanted to take it toward production, these are the gaps that remain:

- **Logging is just `printf`.** There's no proper request log (timestamp,
  method, path, status code) and no log levels.
- **Query strings are stripped but not parsed.** `/api/echo?msg=hi` ignores the
  `msg=hi` part — there's no helper to pull out individual query parameters yet.
- **No config file.** The port is configurable on the command line, but there's
  nothing for the worker count, body-size limit, or the `www/` root.
- **No HTTPS / TLS.** Traffic is plain HTTP only.
- **Shutdown isn't instant.** A worker waiting on a slow client can take up to
  the 5-second receive timeout to exit, so a clean shutdown isn't immediate.

## Why these choices

- **One file, no libraries.** Keeps the focus on learning how the protocol and
  the networking actually work, and makes the whole thing readable top to bottom.
- **Blocking I/O + a thread pool**, rather than the more advanced "event loop"
  approach (what nginx and Node use). The event-loop style scales to far more
  connections but is much harder to reason about. A thread pool is simple,
  correct, and more than enough for this project. Rewriting it as an event loop
  would be a natural next challenge.
- **A routing table** so the server is easy to extend — new behavior is one new
  entry, and anything unrecognized falls back to serving a file.
