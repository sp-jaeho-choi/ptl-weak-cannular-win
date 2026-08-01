#!/usr/bin/env python3
"""
vcan_bridge.py — 리눅스 SocketCAN(vcan) ↔ CANnulaBridge UDP 전송 연결

랩 배선 도구다. 워크스테이션(Windows)의 게이트웨이를 udp 전송 모드로 두고,
리눅스 쪽 vcan0 과 양방향으로 프레임을 흘려 준다. 이렇게 하면 candump /
cansend / SavvyCAN / python-can / scapy 같은 평소 쓰는 CAN 도구를 그대로
워크스테이션 앞에 붙일 수 있다.

    # 리눅스에서 가상 CAN 인터페이스 준비
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link set up vcan0

    # 워크스테이션(192.168.56.10)의 게이트웨이와 연결
    ./vcan_bridge.py --iface vcan0 --peer 192.168.56.10:47101 --listen 47102

    # 이제 평소 도구를 그대로 쓴다
    candump vcan0
    cansend vcan0 100#1400F401010046 00

와이어 포맷(14바이트): u32 id (LE) | u8 dlc | u8 예약 | u8 data[8]

⚠ 인가된 침투 테스트 실습용. 실제 차량·의료기기 버스에 붙이지 말 것.
"""
import argparse
import socket
import struct
import sys
import threading

WIRE = struct.Struct("<IBB8s")


def wire_pack(can_id, dlc, data):
    return WIRE.pack(can_id, dlc, 0, bytes(data).ljust(8, b"\x00"))


def wire_unpack(pkt):
    if len(pkt) < WIRE.size:
        pkt = pkt.ljust(WIRE.size, b"\x00")
    can_id, dlc, _rsv, data = WIRE.unpack(pkt[: WIRE.size])
    return can_id, min(dlc, 8), data[: min(dlc, 8)]


def open_can(iface):
    s = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.bind((iface,))
    return s


def can_to_udp(can_sock, udp_sock, peer, verbose):
    while True:
        frame = can_sock.recv(16)
        can_id, dlc, data = struct.unpack("<IB3x8s", frame)
        can_id &= 0x1FFFFFFF
        dlc = min(dlc, 8)
        udp_sock.sendto(wire_pack(can_id, dlc, data[:dlc]), peer)
        if verbose:
            print(f"vcan→udp  {can_id:03X} [{dlc}] {data[:dlc].hex().upper()}", flush=True)


def udp_to_can(udp_sock, can_sock, verbose, learn_peer):
    while True:
        pkt, src = udp_sock.recvfrom(64)
        can_id, dlc, data = wire_unpack(pkt)
        learn_peer(src)
        frame = struct.pack("<IB3x8s", can_id, dlc, bytes(data).ljust(8, b"\x00"))
        try:
            can_sock.send(frame)
        except OSError as e:
            print(f"vcan 송신 실패: {e}", file=sys.stderr, flush=True)
        if verbose:
            print(f"udp→vcan  {can_id:03X} [{dlc}] {bytes(data).hex().upper()}", flush=True)


def main():
    ap = argparse.ArgumentParser(description="vcan ↔ CANnulaBridge UDP 전송 연결")
    ap.add_argument("--iface", default="vcan0", help="SocketCAN 인터페이스 (기본 vcan0)")
    ap.add_argument("--peer", required=True, help="게이트웨이 UDP 주소 host:port (예: 127.0.0.1:47101)")
    ap.add_argument("--listen", type=int, default=47102, help="이쪽 UDP 수신 포트 (기본 47102)")
    ap.add_argument("--verbose", action="store_true", help="프레임을 찍는다")
    args = ap.parse_args()

    host, _, port = args.peer.rpartition(":")
    peer = [(host, int(port))]

    try:
        can_sock = open_can(args.iface)
    except OSError as e:
        print(f"{args.iface} 를 열 수 없다: {e}", file=sys.stderr)
        print("vcan 인터페이스를 먼저 만들어야 한다 (docstring 참고).", file=sys.stderr)
        return 1

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_sock.bind(("0.0.0.0", args.listen))

    def learn_peer(src):
        # 게이트웨이가 다른 포트에서 보내오면 그쪽으로 회신 대상을 옮긴다.
        if src != peer[0]:
            peer[0] = src

    print(f"vcan {args.iface} ↔ udp {args.peer} (수신 {args.listen})", flush=True)

    t1 = threading.Thread(target=can_to_udp, args=(can_sock, udp_sock, peer[0], args.verbose), daemon=True)
    t2 = threading.Thread(target=udp_to_can, args=(udp_sock, can_sock, args.verbose, learn_peer), daemon=True)
    t1.start()
    t2.start()
    try:
        while True:
            t1.join(1)
            t2.join(1)
            if not (t1.is_alive() and t2.is_alive()):
                break
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
