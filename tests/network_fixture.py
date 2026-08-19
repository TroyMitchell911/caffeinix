#!/usr/bin/env python3

import socket
import threading
import time


def receive_exact(connection, length):
    chunks = []
    received = 0
    while received < length:
        chunk = connection.recv(length - received)
        if not chunk:
            raise ConnectionError("connection closed")
        chunks.append(chunk)
        received += len(chunk)
    return b"".join(chunks)


def receive_until(connection, marker, limit):
    data = bytearray()
    while marker not in data:
        if len(data) == limit:
            raise ValueError("request exceeds limit")
        chunk = connection.recv(min(4096, limit - len(data)))
        if not chunk:
            raise ConnectionError("connection closed")
        data.extend(chunk)
    return bytes(data)


def guest_server_client(udp_socket, peer):
    deadline = time.monotonic() + 5
    while True:
        try:
            connection = socket.create_connection(("127.0.0.1", 18082),
                                                  timeout=1)
            break
        except OSError:
            if time.monotonic() >= deadline:
                return
            time.sleep(0.05)
    with connection:
        connection.sendall(b"host-request")
        if receive_exact(connection, 11) != b"guest-reply":
            return
    udp_socket.sendto(b"server-ok", peer)


def udp_server(server):
    while True:
        payload, peer = server.recvfrom(4096)
        if payload == b"udp-request":
            server.sendto(b"udp-reply", peer)
        elif payload == b"udp-readv-large":
            server.sendto(bytes(index & 0xff for index in range(8192)),
                          peer)
        elif payload == b"tcp-listen-ready":
            threading.Thread(target=guest_server_client,
                             args=(server, peer), daemon=True).start()


def tcp_server(server):
    while True:
        connection, _ = server.accept()
        with connection:
            request = receive_exact(connection, len(b"tcp-request"))
            if request == b"tcp-request":
                connection.sendall(b"tcp-reply")


def tcp_bulk_connection(connection):
    with connection:
        length = int.from_bytes(receive_exact(connection, 4), "big")
        payload = receive_exact(connection, length)
        connection.sendall(payload)


def tcp_bulk_server(server):
    while True:
        connection, _ = server.accept()
        threading.Thread(target=tcp_bulk_connection,
                         args=(connection,), daemon=True).start()


def tcp_linger_connection(connection):
    with connection:
        time.sleep(10)


def tcp_linger_server(server):
    while True:
        connection, _ = server.accept()
        threading.Thread(target=tcp_linger_connection,
                         args=(connection,), daemon=True).start()


def http_server(server):
    response = (
        b"HTTP/1.0 200 OK\r\n"
        b"Content-Length: 15\r\n"
        b"Content-Type: text/plain\r\n"
        b"Connection: close\r\n\r\n"
        b"caffeinix-http\n"
    )
    while True:
        connection, _ = server.accept()
        with connection:
            request = receive_until(connection, b"\r\n", 4096)
            if request.startswith(b"GET /fixture "):
                connection.sendall(response)


udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_socket.bind(("127.0.0.1", 18080))

tcp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
tcp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
tcp_socket.bind(("127.0.0.1", 18081))
tcp_socket.listen()

bulk_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
bulk_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
bulk_socket.bind(("127.0.0.1", 18083))
bulk_socket.listen()

linger_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
linger_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
linger_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
linger_socket.bind(("127.0.0.1", 18086))
linger_socket.listen()

http_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
http_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
http_socket.bind(("127.0.0.1", 18084))
http_socket.listen()

threading.Thread(target=udp_server, args=(udp_socket,), daemon=True).start()
threading.Thread(target=tcp_bulk_server, args=(bulk_socket,),
                 daemon=True).start()
threading.Thread(target=tcp_linger_server, args=(linger_socket,),
                 daemon=True).start()
threading.Thread(target=http_server, args=(http_socket,), daemon=True).start()
print("NETWORK_FIXTURE_READY", flush=True)
tcp_server(tcp_socket)
