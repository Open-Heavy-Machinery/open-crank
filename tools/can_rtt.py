#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Round-trip time of a CAN echo, to measure the latency of a USB-CAN adapter.

Usage:
  tools/can_rtt.py IFACE [--count N] [--rate HZ] [--timeout-ms MS] [--csv FILE]
  tools/can_rtt.py IFACE --echo

Measure mode (the default) sends N request frames on IFACE, one every 1/HZ
seconds (defaults: --count 5000, --rate 100, --timeout-ms 100). Each request
has the extended identifier 0x18FF2EFC and 8 bytes: bytes 0-3 are a sequence
number (uint32, little-endian, starting at 0), bytes 4-7 are 0xFF. The script
waits for echoes with the extended identifier 0x18FF2F27 and 8 bytes, and
matches each one to its request by the sequence number.

  - RTT = time.monotonic_ns() when the echo is received minus when its
    request was sent.
  - An echo that does not arrive within --timeout-ms of its request is lost.
  - A duplicate echo, an echo with an unknown sequence number (including one
    that arrives after its request was counted as lost) or an echo that is
    not 8 bytes long is counted as ignored and does not enter the statistics.

At the end it prints how many frames were sent, received, lost and ignored,
and the RTT in milliseconds with 3 decimals: min, p50, p99 and max.
Percentiles use the nearest-rank method over the received RTTs: the p-th
percentile of n sorted values is the value at rank ceil(p / 100 * n).

--csv FILE writes "seq,rtt_us" with one row per request sent, RTT in whole
microseconds; for a lost frame the rtt_us field is empty (open-tick's
convention: empty means no value).

The figure is the round trip measured in user space on the PC. It includes
the kernel, slcand and USB in both directions, and it is not divided by two.

Echo mode (--echo) listens for 0x18FF2EFC and answers each frame with
0x18FF2F27 and the same bytes, until Ctrl+C. It lets the script be checked on
a vcan interface without a board.

Only the python3 standard library is used (raw AF_CAN socket). Each mode sets
a CAN_RAW_FILTER so that it receives only the identifier it expects.

Exit status: 0 if at least one echo was received (always 0 in echo mode after
Ctrl+C), 1 if none was, 2 if the arguments are wrong.
"""

import argparse
import math
import select
import socket
import struct
import sys
import time

REQUEST_ID = 0x18FF2EFC
ECHO_ID = 0x18FF2F27

CAN_FRAME = struct.Struct("=IB3x8s")
CAN_FILTER = struct.Struct("=II")
SEQ_FILLER = struct.Struct("<I4s")
FILLER = b"\xff" * 4


def open_socket(iface, receive_id):
    """Raw CAN socket on iface that receives only the extended receive_id."""
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    mask = socket.CAN_EFF_FLAG | socket.CAN_RTR_FLAG | socket.CAN_EFF_MASK
    sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FILTER,
                    CAN_FILTER.pack(receive_id | socket.CAN_EFF_FLAG, mask))
    sock.bind((iface,))
    return sock


def send_frame(sock, can_id, data):
    sock.send(CAN_FRAME.pack(can_id | socket.CAN_EFF_FLAG, len(data),
                             data.ljust(8, b"\x00")))


def nearest_rank(sorted_values, p):
    rank = max(1, math.ceil(p / 100.0 * len(sorted_values)))
    return sorted_values[rank - 1]


def ms(ns):
    return "%.3f" % (ns / 1e6)


def run_echo(iface):
    sock = open_socket(iface, REQUEST_ID)
    try:
        while True:
            _, length, data = CAN_FRAME.unpack(sock.recv(CAN_FRAME.size))
            send_frame(sock, ECHO_ID, data[:length])
    except KeyboardInterrupt:
        return 0
    finally:
        sock.close()


def run_measure(iface, count, rate, timeout_ms, csv_path):
    sock = open_socket(iface, ECHO_ID)
    sock.setblocking(False)
    period_ns = round(1e9 / rate)
    timeout_ns = timeout_ms * 1_000_000

    sent_ns = {}        # seq -> send time, for requests awaiting their echo
    rtt_ns = {}         # seq -> RTT, for requests whose echo arrived
    lost = 0
    ignored = 0
    sent = 0
    start_ns = time.monotonic_ns()

    try:
        while sent < count or sent_ns:
            now = time.monotonic_ns()

            # Requests whose echo is overdue; sent_ns is in send order.
            for seq in list(sent_ns):
                if now - sent_ns[seq] < timeout_ns:
                    break
                del sent_ns[seq]
                lost += 1

            if sent < count and now >= start_ns + sent * period_ns:
                send_frame(sock, REQUEST_ID, SEQ_FILLER.pack(sent, FILLER))
                sent_ns[sent] = time.monotonic_ns()
                sent += 1
                continue
            if sent >= count and not sent_ns:
                break

            deadlines = []
            if sent < count:
                deadlines.append(start_ns + sent * period_ns)
            if sent_ns:
                deadlines.append(next(iter(sent_ns.values())) + timeout_ns)
            wait_s = max(0, min(deadlines) - now) / 1e9

            readable, _, _ = select.select([sock], [], [], wait_s)
            if not readable:
                continue
            while True:
                try:
                    raw = sock.recv(CAN_FRAME.size)
                except BlockingIOError:
                    break
                received_ns = time.monotonic_ns()
                _, length, data = CAN_FRAME.unpack(raw)
                seq = SEQ_FILLER.unpack(data)[0]
                if length != 8 or seq not in sent_ns:
                    ignored += 1
                    continue
                rtt_ns[seq] = received_ns - sent_ns.pop(seq)
    finally:
        sock.close()

    if csv_path is not None:
        with open(csv_path, "w", encoding="ascii", newline="") as out:
            out.write("seq,rtt_us\n")
            for seq in range(sent):
                rtt = rtt_ns.get(seq)
                out.write("%d,%s\n" % (seq, "" if rtt is None
                                       else (rtt + 500) // 1000))

    print("sent      %d" % sent)
    print("received  %d" % len(rtt_ns))
    print("lost      %d" % lost)
    print("ignored   %d" % ignored)
    if not rtt_ns:
        print("rtt_ms    no echo received")
        return 1
    values = sorted(rtt_ns.values())
    print("rtt_ms    min %s  p50 %s  p99 %s  max %s" % (
        ms(values[0]), ms(nearest_rank(values, 50)),
        ms(nearest_rank(values, 99)), ms(values[-1])))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Round-trip time of a CAN echo (request 0x18FF2EFC, "
                    "echo 0x18FF2F27).")
    parser.add_argument("iface", metavar="IFACE",
                        help="SocketCAN interface, for example can0 or vcan0")
    parser.add_argument("--count", type=int, default=5000, metavar="N",
                        help="requests to send (default 5000)")
    parser.add_argument("--rate", type=float, default=100.0, metavar="HZ",
                        help="requests per second (default 100)")
    parser.add_argument("--timeout-ms", type=int, default=100, metavar="MS",
                        help="time after which an echo is lost (default 100)")
    parser.add_argument("--csv", metavar="FILE",
                        help='write "seq,rtt_us", one row per request')
    parser.add_argument("--echo", action="store_true",
                        help="answer requests instead of measuring")
    args = parser.parse_args(argv)

    if not 1 <= args.count <= 2 ** 32:
        parser.error("--count must be between 1 and 2^32")
    if not (args.rate > 0 and math.isfinite(args.rate)):
        parser.error("--rate must be a positive number")
    if args.timeout_ms <= 0:
        parser.error("--timeout-ms must be positive")
    if args.iface not in [name for _, name in socket.if_nameindex()]:
        parser.error("no such interface: %s" % args.iface)

    if args.echo:
        return run_echo(args.iface)
    return run_measure(args.iface, args.count, args.rate, args.timeout_ms,
                       args.csv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
