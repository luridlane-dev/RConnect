"""Refreshes the embedded country data.

    python tools/update_geodata.py                 # download this month's DB-IP list and the flags
    python tools/update_geodata.py --csv FILE.csv.gz --flags-zip w40.zip

Writes src/assets/geoip.bin and src/assets/flags/*.png.

geoip.bin (little endian):
    "RCGEO2\0\0", uint32 n4, uint32 n6,
    n4 x uint32 IPv4 range start, n6 x uint32 IPv6 range start (upper 32 bits),
    n4 x 2-byte country code, n6 x 2-byte country code.
A range runs up to the next start. Code "--" means unknown. IPv4 is kept at /24 and IPv6 at
/32 granularity, which keeps the file small; countries rarely change inside such a block.

IP data: DB-IP Lite (https://db-ip.com), CC BY 4.0. Flags: flagcdn.com (public domain).
"""
import argparse
import datetime
import gzip
import io
import ipaddress
import pathlib
import shutil
import struct
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "src" / "assets"


def download(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def ranges(csv_bytes):
    v4, v6 = [], []
    for line in gzip.open(io.BytesIO(csv_bytes), "rt", encoding="ascii"):
        first, last, cc = line.strip().split(",")
        cc = cc.upper() if len(cc) == 2 and cc != "ZZ" else "--"
        a, b = ipaddress.ip_address(first), ipaddress.ip_address(last)
        if a.version == 4:
            v4.append((int(a) >> 8 << 8, int(b), cc))
        else:
            v6.append((int(a) >> 96, int(b) >> 96, cc))
    return v4, v6


def compact(rows):
    """Merges neighbours with the same code and marks gaps as unknown."""
    out = []
    end = -1
    for start, last, cc in sorted(rows):
        if start > end + 1:
            if not out or out[-1][1] != "--":
                out.append((end + 1, "--"))
        if start <= end:  # overlaps after truncating to /24 or /32
            start = end + 1
            if start > last:
                continue
        if not out or out[-1][1] != cc:
            out.append((start, cc))
        end = last
    return out


def write_bin(v4, v6, path):
    with open(path, "wb") as f:
        f.write(b"RCGEO2\0\0")
        f.write(struct.pack("<II", len(v4), len(v6)))
        f.write(b"".join(struct.pack("<I", s) for s, _ in v4))
        f.write(b"".join(struct.pack("<I", s) for s, _ in v6))
        f.write(b"".join(cc.encode() for _, cc in v4))
        f.write(b"".join(cc.encode() for _, cc in v6))


def write_flags(zip_bytes, folder):
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        for name in z.namelist():
            base = pathlib.PurePosixPath(name).name
            if base.endswith(".png") and len(base) == 6:  # two-letter codes only
                (folder / base).write_bytes(z.read(name))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", help="local dbip-country-lite-*.csv.gz")
    p.add_argument("--flags-zip", help="local flagcdn w40.zip")
    args = p.parse_args()

    if args.csv:
        csv_bytes = pathlib.Path(args.csv).read_bytes()
    else:
        month = datetime.date.today().strftime("%Y-%m")
        csv_bytes = download(f"https://download.db-ip.com/free/dbip-country-lite-{month}.csv.gz")
    v4, v6 = ranges(csv_bytes)
    v4, v6 = compact(v4), compact(v6)
    write_bin(v4, v6, ASSETS / "geoip.bin")
    print(f"geoip.bin: {len(v4)} IPv4 and {len(v6)} IPv6 ranges, {(ASSETS / 'geoip.bin').stat().st_size // 1024} KB")

    zip_bytes = pathlib.Path(args.flags_zip).read_bytes() if args.flags_zip else download("https://flagcdn.com/w40.zip")
    write_flags(zip_bytes, ASSETS / "flags")
    print(f"flags: {len(list((ASSETS / 'flags').glob('*.png')))} files")


if __name__ == "__main__":
    main()
