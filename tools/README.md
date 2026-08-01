# 랩 배선 도구

실습 환경을 꾸리기 위한 **중립적인 배선 도구**만 들어 있습니다.
공격 페이로드는 여기 없습니다 — 그건 `HACKING_SCENARIOS.md` 와
`CHEAT_ENGINE_LAB.md` 의 시나리오를 보고 실습자가 직접 만듭니다.

## vcan_bridge.py

리눅스 SocketCAN(`vcan0`)과 워크스테이션 게이트웨이의 **udp 전송**을 이어 줍니다.
USB-CAN 어댑터가 없어도 `candump` / `cansend` / SavvyCAN / `python-can` / `scapy`
같은 평소 도구를 워크스테이션 앞에 그대로 붙일 수 있습니다.

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0

./vcan_bridge.py --iface vcan0 --peer <워크스테이션IP>:47101 --listen 47102 --verbose
```

워크스테이션 쪽에서는 **설정 → 전송 방식 → 가상 CAN (UDP)** 를 고르고,
`runtime/config/cannula.ini` 의 `udp_peer` 를 리눅스 쪽 `IP:47102` 로 지정합니다.

와이어 포맷(14바이트):

| 오프셋 | 크기 | 내용 |
|---|---|---|
| 0 | 4 | CAN ID (little-endian) |
| 4 | 1 | DLC |
| 5 | 1 | 예약 |
| 6 | 8 | 데이터 |

> DLC 는 게이트웨이가 그대로 신뢰합니다. 8보다 큰 값도 통과합니다 —
> 그게 어떤 의미인지는 취약점 문서를 참고하세요.

## 하드웨어를 쓰는 경우

CANable / candleLight 계열 어댑터를 **slcan 펌웨어**로 올리면 워크스테이션이
직접 잡습니다. **설정 → USB-CAN 어댑터 (slcan)** 를 고르고 COM 포트를 적습니다.
게이트웨이가 `C\r` → `S6\r`(500 kbps) → `O\r` 순으로 초기화합니다.

버스 양 끝에 120Ω 종단이 필요하고, 펌프(STM32) 쪽에는 SN65HVD230 트랜시버가
필요합니다. 자세한 배선은 `ptl-weak-cannular-fw/README.md` 를 보세요.
