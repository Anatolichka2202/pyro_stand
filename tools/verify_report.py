#!/usr/bin/env python3
"""Проверяет целостность выгруженного отчёта и исходных кадров без Pyro-stand/Qt."""
import argparse
import base64
import hashlib
import json
from pathlib import Path


class ReportError(ValueError):
    pass


def check(condition, description):
    if not condition:
        raise ReportError(description)


def crc8(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value << 1) ^ 0x31) & 0xFF if value & 0x80 else (value << 1) & 0xFF
    return value


def validate(path):
    """Возвращает сводку. Наличие корректной подписи не подтверждает подлинность."""
    report_file = Path(path).resolve()
    report = json.loads(report_file.read_text(encoding="utf-8-sig"))
    check(report.get("format") == "pyro_stand.report.v1",
          "Неизвестный формат отчёта")
    relative = report.get("frame_evidence_file")
    check(isinstance(relative, str) and relative and Path(relative).name == relative,
          "Недопустимый путь архива кадров")
    evidence_file = report_file.parent / relative
    evidence = evidence_file.read_bytes()
    sha = hashlib.sha256(evidence).hexdigest()
    check(sha.lower() == str(report.get("frame_evidence_sha256", "")).lower(),
          "Контрольная сумма архива кадров не совпадает")
    protocol = report.get("tester_protocol")
    check(protocol in ("aa55-96channels", "legacy-mask-1ms"),
          "Неизвестный протокол прибора")
    count = 0
    checked = 0
    rejected = 0
    for line_number, line in enumerate(evidence.splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
            data = base64.b64decode(row["frame_base64"], validate=True)
        except (ValueError, KeyError) as exc:
            raise ReportError(f"Строка {line_number}: повреждён JSONL/base64") from exc
        check(row.get("protocol") == protocol,
              f"Строка {line_number}: протокол не совпадает")
        if protocol == "aa55-96channels":
            valid = (len(data) == 963 and data[:2] == b"\xaa\x55" and
                     crc8(data[:-1]) == data[-1])
            check(valid == bool(row.get("valid")),
                  f"Строка {line_number}: флаг валидности не соответствует CRC/длине")
            checked += int(valid)
            rejected += int(not valid)
        else:
            # Старый протокол не содержит CRC или номера кадра.
            checked += len(data)
        count += 1
    check(int(report["accepted_frames"]) == checked,
          "Число принятых кадров/миллисекунд не совпадает с архивом")
    check(int(report["rejected_frames"]) == rejected,
          "Число отвергнутых кадров не совпадает с архивом")
    source = report.get("cyclogram_base64")
    digest = report.get("cyclogram_sha256")
    if source and digest:
        cyclogram = base64.b64decode(source, validate=True)
        check(hashlib.sha256(cyclogram).hexdigest().lower() == digest.lower(),
              "SHA-256 циклограммы не совпадает с вложенным файлом")
    return {"report": str(report_file), "protocol": protocol,
            "records": count, "accepted": checked, "rejected": rejected,
            "archive_sha256": sha,
            "collection_finished": bool(report.get("collection_finished"))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="JSON-отчёт из Pyro-stand")
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.report), ensure_ascii=False, indent=2))
    except (OSError, ReportError, ValueError, KeyError) as exc:
        parser.exit(2, f"Отчёт не прошёл проверку: {exc}\n")


if __name__ == "__main__":
    main()
