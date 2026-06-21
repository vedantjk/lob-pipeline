#!/bin/bash
set -euo pipefail

# NASDAQ ITCH 5.0 sample (smallest file, ~3.52 GB compressed / ~10 GB raw).
# Spec: docs/specs/2026-06-20-rung0-oracle.md §3
# Idempotent: re-running skips work that's already done.
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
FILE="12302019.NASDAQ_ITCH50.gz"
RAW="${FILE%.gz}"

# Always operate on the repo's data/ dir, regardless of the caller's cwd.
DATA_DIR="$(cd "$(dirname "$0")/.." && pwd)/data"
mkdir -p "${DATA_DIR}"
cd "${DATA_DIR}"

remote_size=$(curl -sI "${BASE}/${FILE}" \
    | awk 'tolower($1)=="content-length:"{gsub("\r","",$2); print $2}')

# 1. Download only if missing or incomplete. wget -c resumes a partial file.
if [[ -f "${FILE}" && -n "${remote_size}" && "$(stat -c '%s' "${FILE}")" == "${remote_size}" ]]; then
    echo "${FILE} already present and complete — skipping download."
else
    wget -c "${BASE}/${FILE}"
fi

# 2. Verify integrity.
# NOTE: the directory listing advertises "${FILE}.md5sum" but the server 404s it
# (verified 2026-06-20) — no official checksum is published. We fall back to two
# checks that together are stronger than the .gz md5 would have been:
#   (a) byte size matches the server's Content-Length, and
#   (b) gzip -t validates gzip's internal CRC32 + length of the *uncompressed*
#       stream, proving a bit-correct decompression.
if wget -q -O "${FILE}.md5sum" "${BASE}/${FILE}.md5sum"; then
    echo "md5sum file present — verifying."
    md5sum -c "${FILE}.md5sum"
else
    rm -f "${FILE}.md5sum"
    echo "No published .md5sum (server 404) — falling back to size + gzip CRC."

    local_size=$(stat -c '%s' "${FILE}")
    if [[ -n "${remote_size}" && "${remote_size}" != "${local_size}" ]]; then
        echo "SIZE MISMATCH: local=${local_size} remote=${remote_size}" >&2
        exit 1
    fi
    echo "size OK (${local_size} bytes)"

    gzip -t "${FILE}"
    echo "gzip CRC OK"
fi

# 3. Decompress only if missing, keeping the .gz. Result: ~10 GB raw file.
if [[ -f "${RAW}" ]]; then
    echo "${RAW} already present — skipping decompression."
else
    gunzip -k "${FILE}"
fi

# 4. Eyeball: first framed message should be a System Event ('S').
echo "First bytes (expect: 2-byte len prefix then 0x53 'S'):"
xxd -l 160 "${RAW}"
