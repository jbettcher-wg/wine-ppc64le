#!/usr/bin/env python3
"""loopback_server.py -- the HTTP server check-wininet-callbacks.sh runs itself.

There is no network access here and there must never be: the gate binds this
to 127.0.0.1 on an EPHEMERAL port, prints the port it got, and the probe is
told that port on its command line.  Nothing resolves a name, nothing leaves
the machine, and two copies of this gate running at once cannot collide over
a fixed port number.

Raw sockets rather than http.server, for three reasons that all matter to what
the probe is allowed to assert:

  * the response bytes are written out literally here, so the probe can check
    the body it received against a constant rather than against whatever a
    library decided to send.  Content-Length is computed from the body, so
    the two can never disagree;
  * http.server logs to stderr and reformats headers; this writes exactly what
    is below and nothing else;
  * a one-shot library server would close the listening socket between
    requests, and wininet's callback sequence (CONNECTING_TO_SERVER,
    CONNECTION_SENT, ...) is precisely what the gate is watching, so the
    server has to stay predictable across however many connections wininet
    decides to open.

It serves until told how many requests to expect (--requests, default 1) and
then exits, so the gate never has to kill it and a hung probe cannot leave a
listener behind.
"""

import argparse
import socket
import sys
import threading

# The body every request gets.  A fixed, compile-time-known string: the probe
# checks the bytes AND the length, and the length is what wininet reports
# through the callback's fifth argument on some of its status codes.
BODY = b"ppc64le-wininet-callback-gate-body\n"

RESPONSE = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: text/plain\r\n"
    b"Content-Length: " + str(len(BODY)).encode() + b"\r\n"
    b"Connection: close\r\n"
    b"\r\n" + BODY
)


def serve(sock, requests, quiet):
    served = 0
    while served < requests:
        try:
            conn, _peer = sock.accept()
        except OSError:
            break
        try:
            conn.settimeout(10)
            # Read the request head.  We do not parse it: this server answers
            # everything the same way, and what the gate is proving is on the
            # CLIENT side of the boundary.
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            conn.sendall(RESPONSE)
        except OSError as exc:
            if not quiet:
                print("loopback_server: connection error: %s" % exc,
                      file=sys.stderr, flush=True)
        finally:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
        served += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--requests", type=int, default=1,
                    help="serve this many connections, then exit")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="give up and exit even if fewer arrived")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Port 0: the kernel picks a free one.  Loopback only -- binding
    # 0.0.0.0 would make this reachable from off the machine, which a test
    # fixture has no business being.
    sock.bind(("127.0.0.1", 0))
    sock.listen(8)
    port = sock.getsockname()[1]

    # The gate reads this line to learn the port.  Flushed immediately,
    # because the gate blocks on it.
    print("PORT %d" % port, flush=True)
    print("BODYLEN %d" % len(BODY), flush=True)

    worker = threading.Thread(target=serve, args=(sock, args.requests, args.quiet))
    worker.daemon = True
    worker.start()
    worker.join(args.timeout)

    try:
        sock.close()
    except OSError:
        pass
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
