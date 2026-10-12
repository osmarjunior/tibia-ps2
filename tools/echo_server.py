#!/usr/bin/env python3
"""Echo server TCP para o teste da Fase 1 do TASKS2.md.

Uso:
    python tools/echo_server.py [porta] [bind]

Default: porta 7777, bind 0.0.0.0 (aceita conexoes do PCSX2 via NAT
ou de um PS2 real na rede local). Devolve exatamente os bytes recebidos.

Log por conexao: timestamp, peer, bytes ecoados.
"""
import socket
import sys
import threading
import time


def handle(conn, addr):
    print(f"[{time.strftime('%H:%M:%S')}] conn {addr}", flush=True)
    total = 0
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            conn.sendall(data)
            total += len(data)
            print(f"[{time.strftime('%H:%M:%S')}] {addr}: echoed {len(data)} bytes "
                  f"(total {total}): {data[:32]!r}{'...' if len(data) > 32 else ''}",
                  flush=True)
    except ConnectionError:
        pass
    finally:
        conn.close()
        print(f"[{time.strftime('%H:%M:%S')}] {addr}: closed (total {total} bytes)",
              flush=True)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
    bind = sys.argv[2] if len(sys.argv) > 2 else "0.0.0.0"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, port))
    srv.listen(8)
    print(f"echo server listening on {bind}:{port}", flush=True)

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    main()
