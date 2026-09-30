#!/usr/bin/env python3
"""Generate a REAL downlink Opus packet stream for the gap-fill FEC/PLC tests.

Encodes a real far-field capture through the LIVE downlink encoder config —
aiortc's OpusEncoder plus the import-time patch in pipecat-house-voice
`server/webrtc_server.py` (application=voip, fec=1, packet_loss=10, 48 kHz
stereo, bit_rate 96000) — exactly what the satellite receives on the wire.

Writes `downlink_stream.bin`: a sequence of `<u16 little-endian len><payload>`
records, in transmission order, so a host test can walk the stream with the
vendored esp-libopus decoder in the firmware's exact configuration
(opus_decoder_create(16000, 1), 320-sample frames) and inject gaps anywhere.

Also writes `downlink_stream.lbrr`: one byte per packet, the ORACLE verdict of
libopus >= 1.5 `opus_packet_has_lbrr()` (1 = carries LBRR, 0 = does not). The
firmware's vendored esp-libopus (260b16c, 2020) predates that accessor, so the
firmware-side parser is cross-checked against this oracle packet-by-packet.

Run on ACE-AI in the hub venv (~/.venvs/pipecat-hub-13: PyAV 16.1.0 bundles /
links libopus 1.6.1):
    python3 gen_fixtures.py /home/ace/clanker-flightrec/_cmd_tap/<capture>.wav
"""
from __future__ import annotations

import ctypes
import ctypes.util
import fractions
import hashlib
import struct
import sys
import wave
from pathlib import Path

import av
from av.audio.resampler import AudioResampler

MAX_PACKETS = 400  # ~80 KB; enough to hold both LBRR classes many times over


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    src_path = Path(sys.argv[1])
    out_dir = Path(__file__).resolve().parent

    lib = ctypes.CDLL(ctypes.util.find_library("opus"))
    lib.opus_packet_has_lbrr.restype = ctypes.c_int
    lib.opus_packet_has_lbrr.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int32]
    lib.opus_get_version_string.restype = ctypes.c_char_p

    def has_lbrr(payload: bytes) -> int:
        buf = (ctypes.c_ubyte * len(payload)).from_buffer_copy(payload)
        return lib.opus_packet_has_lbrr(ctypes.cast(buf, ctypes.POINTER(ctypes.c_ubyte)), len(payload))

    codec = av.CodecContext.create("libopus", "w")
    codec.bit_rate = 96000
    codec.format = "s16"
    codec.layout = "stereo"
    codec.options = {"application": "voip", "fec": "1", "packet_loss": "10"}
    codec.sample_rate = 48000
    codec.time_base = fractions.Fraction(1, 48000)
    resampler = AudioResampler(format="s16", layout="stereo", rate=48000, frame_size=960)

    with wave.open(str(src_path), "rb") as handle:
        assert handle.getnchannels() == 1 and handle.getframerate() == 16000 and handle.getsampwidth() == 2
        pcm = handle.readframes(handle.getnframes())
    src = av.AudioFrame(format="s16", layout="mono", samples=len(pcm) // 2)
    src.planes[0].update(pcm)
    src.sample_rate = 16000
    src.time_base = fractions.Fraction(1, 16000)
    src.pts = 0

    payloads: list[bytes] = []
    for frame in resampler.resample(src):
        payloads.extend(bytes(p) for p in codec.encode(frame))
    payloads.extend(bytes(p) for p in codec.encode(None))
    payloads = [p for p in payloads if p][:MAX_PACKETS]

    verdicts = bytes(1 if has_lbrr(p) == 1 else 0 for p in payloads)
    stream = b"".join(struct.pack("<H", len(p)) + p for p in payloads)
    (out_dir / "downlink_stream.bin").write_bytes(stream)
    (out_dir / "downlink_stream.lbrr").write_bytes(verdicts)

    n_with = sum(verdicts)
    configs = sorted({p[0] >> 3 for p in payloads})
    prov = "\n".join(
        [
            "downlink fixture provenance",
            f"source wav: {src_path.name} (sha256 {hashlib.sha256(pcm).hexdigest()[:16]}, "
            f"{len(pcm) // 2} samples @16k mono)",
            "encoder: PyAV libopus, application=voip fec=1 packet_loss=10, 48k stereo, 96000 bps",
            "          (= aiortc OpusEncoder + webrtc_server.py import-time patch, the LIVE downlink)",
            f"oracle: libopus {lib.opus_get_version_string().decode()} opus_packet_has_lbrr()",
            f"packets: {len(payloads)} (capped at {MAX_PACKETS}); toc configs {configs}",
            f"with_LBRR={n_with} ({100 * n_with / len(payloads):.1f}%)  "
            f"without_LBRR={len(payloads) - n_with} ({100 * (len(payloads) - n_with) / len(payloads):.1f}%)",
            f"stream sha256: {hashlib.sha256(stream).hexdigest()}",
        ]
    )
    (out_dir / "PROVENANCE.txt").write_text(prov + "\n")
    print(prov)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
