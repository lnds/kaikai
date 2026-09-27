# net

Byte-level networking — TCP, UDP, DNS, and Unix-domain sockets.

## Description

Each protocol family is its own effect, so a signature says exactly
which door a function can open: a resolver carries `/ NetDns` and
cannot connect; a local daemon carries `/ NetUnix` and cannot reach
the IP stack.

| module | effect | handles |
|--------|--------|---------|
| `net.tcp` | `NetTcp` | `connect(host, port)`, `listen(host, port)`, `accept`, `send`, `recv`, `recv_timeout`, `close` |
| `net.udp` | `NetUdp` | `bind`, `send`, `recv`, `close` |
| `net.dns` | `NetDns` | `resolve`, `resolve_first`, `with_dns` |
| `net.unix` | `NetUnix` | `listen(path, mode)`, `connect(path)`, then the TCP ops, plus `peer_uid` and `close_listener` |

Bytes are `[Int]` (each in `0..255`), results are Ok-first, and the
blocking stream ops park the fiber, not the OS thread — several
fibers can `accept` on one listener. `recv` returning `Ok([])` means
the peer closed; `recv_timeout` returns `None` when the deadline
passes first.

## Unix-domain sockets

```kaikai
import net.unix

fn serve(l: Listener) : Int / NetUnix =
  match NetUnix.accept(l) {
    Err(_) -> 1
    Ok(c)  -> {
      let who = NetUnix.peer_uid(c)        # the kernel's record, not the peer's claim
      let _ = NetUnix.send(c, [111, 107])
      NetUnix.close(c)
      match who { Ok(_) -> 0  Err(_) -> 1 }
    }
  }

fn main() : Int / NetUnix = match listen("/tmp/demo.sock", owner_only) {
  Err(_) -> 1
  Ok(l)  -> {
    let rc = serve(l)
    NetUnix.close_listener(l)                 # closes and unlinks the file
    rc
  }
}
```

- `mode` is the socket file's permission bits; `owner_only` is
  `0600` (kaikai has no octal literal). The file carries it from the
  moment it is reachable — no `chmod` afterwards.
- `listen` replaces a socket nobody listens on (a crash leftover) and
  refuses a live one or a non-socket file.
- `path` must fit `sun_path` (104 bytes on macOS, 108 on Linux), and
  so must `path`'s directory plus 12 bytes, where the socket is bound
  before it is moved into place.

## See also

`kai info effects` (handlers, rows), `kai info fibers` (nursery,
spawn — serving many clients at once).
