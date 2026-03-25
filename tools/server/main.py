#!/usr/bin/env python3
"""
UWB Mesh Tracker - CoAP Server

Receives distance measurements from Thread-connected UWB anchor nodes
and stores them in a SQLite database.

Usage:
    python main.py [--host HOST] [--port PORT] [--db PATH]

The server listens on all IPv6 interfaces by default. Connect your
Thread border router so the server is reachable from the Thread mesh.

Requirements:
    pip install -r requirements.txt
"""

import argparse
import asyncio
import logging
import os
import signal
import socket
import struct

import aiocoap

import database
from coap_server import build_site

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(name)s  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger(__name__)


async def run_server(host: str, port: int) -> None:
    """Start the CoAP server and run until cancelled."""
    bind_addr = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    logger.info("Starting CoAP server on %s", bind_addr)

    site = build_site()
    context = await aiocoap.Context.create_server_context(
        site,
        bind=(host, port),
    )

    # Join ff03::1 multicast on wpan0 so we receive CoAP POSTs from Thread devices.
    # aiocoap doesn't join multicast groups by default.
    _join_multicast("ff03::1", "wpan0")

    logger.info("CoAP server ready. Waiting for distance measurements...")
    logger.info("Database: %s", os.path.abspath(database.DB_PATH))

    # Run until cancelled (Ctrl-C or SIGTERM)
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _handle_signal():
        logger.info("Shutdown signal received")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, _handle_signal)

    await stop_event.wait()

    logger.info("Shutting down CoAP server...")
    await context.shutdown()


def _join_multicast(group: str, interface: str) -> None:
    """Join an IPv6 multicast group on a specific interface at the OS socket level."""
    try:
        idx = socket.if_nametoindex(interface)
        sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        mreq = struct.pack("16sI", socket.inet_pton(socket.AF_INET6, group), idx)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)
        sock.close()
        logger.info("Joined multicast %s on %s", group, interface)
    except OSError as e:
        logger.warning("Could not join multicast %s on %s: %s", group, interface, e)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="UWB Mesh Tracker - CoAP Server"
    )
    parser.add_argument(
        "--host",
        default="::",
        help="IPv6 address to bind (default: :: = all interfaces)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=5683,
        help="CoAP UDP port (default: 5683)",
    )
    parser.add_argument(
        "--db",
        default=None,
        help="Path to SQLite database file (default: uwb_measurements.db)",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Log level (default: INFO)",
    )
    args = parser.parse_args()

    logging.getLogger().setLevel(args.log_level)

    if args.db:
        database.DB_PATH = args.db

    # Initialize database
    database.init_db()

    # Run server
    asyncio.run(run_server(args.host, args.port))


if __name__ == "__main__":
    main()
