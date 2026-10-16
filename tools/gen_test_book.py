#!/usr/bin/env python3
"""生成第 2 步用的大 TXT。

GB18030 汉字大约 2 字节，750 万字接近 15MB。
UTF-8 汉字 3 字节，写到文件达到 100MB 为止。
正文里不出现「第」，避免把填充行认成章节。
"""

from __future__ import annotations

import sys
from pathlib import Path

FILLER = "山风穿过竹林，带来远处的潮声。\n"
HEADING_EVERY = 8000


def write_book(path: Path, encoding: str, target_chars: int | None, target_bytes: int | None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    chars = 0
    chapter = 1
    since_heading = HEADING_EVERY
    with path.open("wb") as output:
        while True:
            if since_heading >= HEADING_EVERY:
                piece = f"第{chapter}章 试炼\n"
                chapter += 1
                since_heading = 0
            else:
                piece = FILLER
            encoded = piece.encode(encoding)
            output.write(encoded)
            chars += len(piece)
            since_heading += len(piece)
            if target_chars is not None and chars >= target_chars:
                break
            if target_bytes is not None and output.tell() >= target_bytes:
                break
    print(f"{path}  {path.stat().st_size} bytes  {chars} chars  {encoding}")


def write_gbk_with_bad_byte(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    filler = FILLER.encode("gb18030")
    target = 1024 * 1024
    blob = bytearray()
    while len(blob) < target:
        blob += filler
    middle = len(blob) // 2
    blob[middle : middle + 1] = b"\xff"
    path.write_bytes(blob)
    print(f"{path}  {path.stat().st_size} bytes  gbk-bad-byte")


def write_cr_only(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # 老 Mac 文本只有 \r。体积控制在大约 1MB，用来看换行归一和分章。
    parts: list[str] = []
    chapter = 1
    size = 0
    target = 1024 * 1024
    while size < target:
        if len(parts) % 40 == 0:
            piece = f"第{chapter}章 试炼\r"
            chapter += 1
        else:
            piece = "山风穿过竹林，带来远处的潮声。\r"
        parts.append(piece)
        size += len(piece.encode("utf-8"))
    path.write_bytes("".join(parts).encode("utf-8"))
    print(f"{path}  {path.stat().st_size} bytes  cr-only")


def write_long_chapter(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "第一章 长章\n" + ("文" * 120_000)
    path.write_text(text, encoding="utf-8")
    print(f"{path}  {path.stat().st_size} bytes  {len(text)} chars  long-chapter")


def write_ascii_then_gbk(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    line = b"https://example.com/novel/chapter\n"
    prefix = bytearray()
    while len(prefix) < 70 * 1024:
        prefix += line
    body = bytearray()
    filler = FILLER.encode("gb18030")
    while len(body) < 1024 * 1024:
        body += filler
    body += "你好\n".encode("gb18030")
    path.write_bytes(prefix + body)
    print(f"{path}  {path.stat().st_size} bytes  ascii-prefix-gbk")


def write_cold_start(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "第1章 开篇\n山风穿过竹林，带来远处的潮声。\n第2章 再看\n潮声还在。\n"
    path.write_text(text, encoding="utf-8")
    print(f"{path}  {path.stat().st_size} bytes  cold-start")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "bench" / "data"
    # 5e6 个汉字只有约 10MB。要接近 15MB，按 2 字节一个汉字写 750 万字。
    write_book(out / "gb18030_15mb.txt", "gb18030", 7_500_000, None)
    write_book(out / "utf8_100mb.txt", "utf-8", None, 100 * 1024 * 1024)
    write_gbk_with_bad_byte(out / "gbk_bad_byte.txt")
    write_cr_only(out / "cr_only.txt")
    write_long_chapter(out / "long_chapter.txt")
    write_ascii_then_gbk(out / "ascii_prefix_gbk.txt")
    write_cold_start(out / "cold_start.txt")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
