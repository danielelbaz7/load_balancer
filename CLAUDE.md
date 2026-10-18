# load_balancer

Event-driven Layer 4 (TCP) load balancer in C++20, built on Linux `epoll` with non-blocking sockets. Single-threaded event loop; each client connection is paired with its own backend connection and bytes are relayed both ways.

## Build and run

epoll is Linux-only, so development happens in the Alpine container from `docker-compose.yml` (project mounted at `/src`, build output in the `/build` volume).

```bash
docker compose run --rm --service-ports dev
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build /build
/build/load_balancer 8080 127.0.0.1:9001 127.0.0.1:9002   # <listen_port> <host:port>...
```

Quick test backends inside the container: `apk add busybox-extras` then `busybox-extras httpd -f -p 9001 -h <dir>`. Load test: `wrk -t2 -c100 -d10s http://localhost:8080/`.

## Architecture

```
main ─ parses argv ─► LoadBalancer(port, backends) ─► run()   (epoll loop, never returns)

run(): epoll_wait, then for each ready fd:
   listener fd   → Listener::on_event → accept4 loop → on_new_client
   timer fd      → on_health_tick
   probe fd      → on_probe_done
   anything else → on_connection_event (client or backend socket of a Connection)
```

| File | Role |
|---|---|
| `listener.h/.cpp` | Owns the listening socket (`SO_REUSEADDR`, `SO_REUSEPORT`, non-blocking). `on_event` accepts until `EAGAIN` and hands each client fd to a callback. |
| `load_balancer.h/.cpp` | Owns epoll, the pool, the connection map, the health timer. Runs the loop and all proxy logic. |
| `backend_pool.h/.cpp` | `Backend` (address, active connection count, health state) and `BackendPool` (least-connections choice among healthy backends, health bookkeeping). |
| `connection.h` | `Connection`: client fd + backend fd + `Backend*`, connect/done flags, pending-byte buffers per direction, current epoll masks. |
| `main.cpp` | Parses `<port> <host:port>...` with `getaddrinfo` (numeric IPs or hostnames). |

### Connection lifecycle

1. **Accept** → `on_new_client`: pick a backend, non-blocking `connect` (expect `EINPROGRESS`), create the `Connection`, map both fds to it, watch only the backend fd for `EPOLLOUT`. The client fd isn't registered yet; the kernel buffers anything it sends.
2. **Handshake done** → `on_backend_connected`: `getsockopt(SO_ERROR)` tells success from failure (both show up as writable). On success watch both fds for `EPOLLIN | EPOLLRDHUP`.
3. **Data** → `forward`: read until `EAGAIN`, `send_some` to the other side; leftovers go in `to_backend` / `to_client` and are sent later by `flush` on `EPOLLOUT`.
4. **Backpressure** → `update_interest`: stop reading a side while bytes are waiting to go to the other side; ask for `EPOLLOUT` only while bytes are waiting. At most one 16 KB chunk is buffered per direction.
5. **Close** → a side that hits EOF is marked done; the connection closes once everything it sent has been delivered (`close_connection` erases both map entries, closes both fds, decrements the backend count).

### Health checks

- Active: a `timerfd` fires every 2 s; `on_health_tick` starts a non-blocking connect probe per backend. A probe still pending at the next tick counts as a timeout.
- Passive: a real client's failed backend connect also counts as a failure.
- 3 failures in a row → DOWN (skipped by `choose_backend`); 2 successes in a row → UP.

## Invariants to keep

- The backend list is fixed after construction. The vector never grows, so `Backend*` held by connections and probes never dangles.
- `pool_` is declared before `conns_` / `probes_` in `LoadBalancer`, so it's destroyed after everything that points into it.
- Every socket is non-blocking. One blocking call stalls every connection.
- Only watch events you will handle: `EPOLLOUT` on an idle connected socket, or `EPOLLIN` without reading, makes level-triggered epoll report the fd forever (100% CPU).

## Things I ran into

- **epoll doesn't exist on macOS** (it has kqueue). Docker Desktop runs a Linux VM; containers share its kernel, so the Alpine image only provides userland (compiler, headers, tools).
- **Ubuntu's apt CMake is 3.28**, so `cmake_minimum_required` is 3.20, not CLion's default 4.0.
- **Accepted sockets don't inherit `O_NONBLOCK` on Linux.** Use `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`.
- **Accept/read loops must run until `EAGAIN`.** One epoll wakeup can mean many waiting clients or many chunks.
- **Checking `events` instead of `errno`**, plus `&&`/`||` precedence, turned the accept loop into an infinite loop calling the callback with `-1`. Only call the callback for fds `>= 0`; break on any unrecoverable error.
- **A constructor that throws never runs its destructor.** Close fds manually before throwing, and save `errno` first since `close()` can overwrite it.
- **Members are constructed in declaration order**, not initializer-list order; members without a default constructor (like `Listener`) must be built in the initializer list.
- **`for (Backend b : backends_)` copies each element**; returning `&b` returned a pointer to a destroyed copy. The compiler caught it with `-Wreturn-local-addr`. Use `Backend&`.
- **`std::vector` reallocation moves elements** and invalidates pointers into it, which is why the pool is fixed after startup.
- **Non-blocking `connect` returns `EINPROGRESS`**; a failed connect also reports writable, so always check `SO_ERROR`.
- **Writing to a socket the peer closed raises `SIGPIPE`** and kills the process. Use `send(..., MSG_NOSIGNAL)`.
- **`write`/`send` can accept fewer bytes than given.** Leftovers must be buffered and sent on `EPOLLOUT`.
- **Closing as soon as a peer hung up dropped data**: a backend that sends a response and closes, with a slow client, lost the tail of the response. Fixed by marking the side done and closing only once its bytes were delivered.
- **netcat-openbsd doesn't close on stdin EOF** unless given `-N`, which looked like a leak in tests.
- **Alpine's busybox has no `httpd` applet**; it's in `busybox-extras`.

## Known limitations / next steps

- Half-close isn't supported: when a client shuts down its write side, the connection closes once its bytes are delivered, before the backend responds.
- If a connection closes and a new one reuses the same fd within one `epoll_wait` batch, a stale event could reach the new connection (fix: `data.ptr` handlers plus deferred deletion).
- Health probes only check TCP connect, not an HTTP response.
- No connect timeout for client connections, no max-connection limit, no idle timeout.
- Single-threaded; next step is one event loop per core with `SO_REUSEPORT`.
- Per-connection logging to stdout slows benchmarks.
