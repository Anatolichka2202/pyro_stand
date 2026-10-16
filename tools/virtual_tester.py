#!/usr/bin/env python3
"""Виртуальный пиротестер: пассивный источник синтетических данных, только loopback."""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
from pathlib import Path

FRAME_LENGTH = 963
CHANNELS = 96


def crc8(data):
    """CRC-8, полином 0x31, начальное значение 0; как в протоколе новой платы."""
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value << 1) ^ 0x31) & 255 if value & 128 else (value << 1) & 255
    return value


def board_frame(channels):
    """Состояние 96 каналов: triggered, ready, rise_us, fall_us."""
    data = bytearray(FRAME_LENGTH)
    data[:2] = b"\xaa\x55"
    for index, (triggered, ready, rise_us, fall_us) in channels.items():
        if not 0 <= index < CHANNELS:
            raise ValueError("Канал вне диапазона 0..95")
        struct.pack_into("<BBII", data, 2 + index * 10,
                         int(bool(triggered)), int(bool(ready)),
                         rise_us & 0xFFFFFFFF, fall_us & 0xFFFFFFFF)
    data[-1] = crc8(data[:-1])
    return bytes(data)


def modern_scenario(name):
    """Кадры с НАБОРНЫМИ отметками: они не имитируют изменение часов ПК."""
    kp = 0xFFFFF000 if name == "wrap" else 1_000_000
    frames = [board_frame({})]
    frames.append(board_frame({} if name == "missing-kp" else {
        7: (True, True, kp, 0)}))
    channels = {0: (True, True, (kp + 20_000) & 0xFFFFFFFF, 0),
                1: (True, True, (kp + 20_000) & 0xFFFFFFFF, 0)}
    if name != "missing-kp":
        channels[7] = (True, True, kp, 0)
    frames.append(board_frame(channels))
    channels[0] = (True, False, (kp + 20_000) & 0xFFFFFFFF,
                   (kp + 26_000) & 0xFFFFFFFF)
    channels[1] = (True, False, (kp + 20_000) & 0xFFFFFFFF,
                   (kp + 26_000) & 0xFFFFFFFF)
    frames.append(board_frame(channels))
    if name == "bad-crc":
        broken = bytearray(frames[2])
        broken[24] ^= 1
        frames.insert(2, bytes(broken))
    if name == "loss":
        # Потерю UDP нельзя распознать без номера последовательности внутри протокола.
        frames.pop(2)
    return frames


def old_scenario(name):
    """Один байт = 1 аппаратная миллисекунда; восьмой бит — КП старой платы."""
    samples = bytearray(340)
    if name != "missing-kp":
        for tick in range(5, len(samples)):
            samples[tick] |= 0x80
    for tick in range(25, 31):
        samples[tick] |= 0x03
    for tick in range(55, 61):
        samples[tick] |= 0x1C
    if name == "loss":
        del samples[10:12]  # Дыра незаметна получателю: в протоколе нет номера байта.
    return bytes(samples)


def load_trace(path, protocol):
    """Читает как собственный JSONL, так и .frames.jsonl отчёта пиростенда."""
    packets = []
    for number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        row = json.loads(line)
        if row.get("protocol") != protocol:
            continue
        if "frame_base64" not in row:
            continue
        packet = base64.b64decode(row["frame_base64"], validate=True)
        if protocol == "aa55-96channels" and len(packet) != FRAME_LENGTH:
            raise ValueError(f"Строка {number}: длина AA55 не равна {FRAME_LENGTH}")
        packets.append(packet)
    if not packets:
        raise ValueError("В журнале нет кадров нужного протокола")
    return packets


def save_trace(path, protocol, packets):
    with Path(path).open("w", encoding="utf-8") as output:
        for index, packet in enumerate(packets):
            output.write(json.dumps({
                "protocol": protocol,
                "seq_in_emulator": index,
                "frame_base64": base64.b64encode(packet).decode("ascii")
            }, ensure_ascii=False) + "\n")


def send_udp(packets, address, port, rate):
    if address not in ("127.0.0.1", "::1", "localhost"):
        raise ValueError("Виртуальный пиротестер отправляет только в loopback")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        for packet in packets:
            udp.sendto(packet, ("127.0.0.1", port))
            if rate > 0:
                time.sleep(1 / rate)


def send_pty(data, rate):
    if os.name != "posix":
        raise RuntimeError("PTY доступен на Linux/macOS; на Windows используйте файл масок")
    import pty
    master, slave = pty.openpty()
    try:
        print("Виртуальный COM (укажите --legacy-serial):", os.ttyname(slave), flush=True)
        print("Запустите Pyro-stand, затем нажмите Enter для подачи байтов.", flush=True)
        input()
        # Не объединяем отдельные миллисекундные отсчёты при live-воспроизведении.
        for byte in data:
            os.write(master, bytes((byte,)))
            if rate > 0:
                time.sleep(1 / rate)
    finally:
        os.close(master)
        os.close(slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("udp", "legacy-file", "legacy-pty"),
                        default="udp")
    parser.add_argument("--scenario", choices=("normal", "missing-kp", "bad-crc",
                                               "loss", "wrap"), default="normal")
    parser.add_argument("--address", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--rate", type=float, default=None,
                        help="Скорость подачи пакетов/масок; 0 — без задержек")
    parser.add_argument("--record", help="Записать сгенерированные данные в JSONL")
    parser.add_argument("--replay", help="Воспроизвести кадры из *.frames.jsonl")
    parser.add_argument("--output", default="legacy-simulated.raw",
                        help="Имя файла сырых байтов в режиме legacy-file")
    args = parser.parse_args()
    if args.rate is None:
        args.rate = 1000 if args.mode.startswith("legacy") else 100
    if args.port < 1 or args.port > 65535 or args.rate < 0:
        parser.error("Неверный порт или частота")
    protocol = "legacy-mask-1ms" if args.mode.startswith("legacy") else "aa55-96channels"
    packets = load_trace(args.replay, protocol) if args.replay else (
        [old_scenario(args.scenario)] if args.mode.startswith("legacy")
        else modern_scenario(args.scenario))
    if args.record:
        save_trace(args.record, protocol, packets)
    if args.mode == "legacy-file":
        result = b"".join(packets)
        Path(args.output).write_bytes(result)
        print(f"Записано {len(result)} аппаратных отсчётов в {args.output}")
    elif args.mode == "legacy-pty":
        send_pty(b"".join(packets), args.rate)
    else:
        send_udp(packets, args.address, args.port, args.rate)
        print(f"Отправлено {len(packets)} UDP-кадров в 127.0.0.1:{args.port}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
