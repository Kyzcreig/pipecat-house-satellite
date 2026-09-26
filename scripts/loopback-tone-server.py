#!/usr/bin/env python3
"""Minimal SmallWebRTC tone endpoint for ESP32 bench validation."""

import argparse
import asyncio
import math
from fractions import Fraction

from aiohttp import web
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import AudioStreamTrack
from av import AudioFrame


class ToneTrack(AudioStreamTrack):
    kind = "audio"

    def __init__(self, sample_rate: int = 16000, frequency: int = 660) -> None:
        super().__init__()
        self.sample_rate = sample_rate
        self.frequency = frequency
        self.samples = sample_rate // 50
        self.pts = 0
        self.phase = 0

    async def recv(self) -> AudioFrame:
        await asyncio.sleep(self.samples / self.sample_rate)

        pcm = bytearray()
        for _ in range(self.samples):
            value = int(9000 * math.sin(2 * math.pi * self.phase))
            pcm.extend(value.to_bytes(2, "little", signed=True))
            self.phase = (self.phase + self.frequency / self.sample_rate) % 1.0

        frame = AudioFrame(format="s16", layout="mono", samples=self.samples)
        frame.planes[0].update(bytes(pcm))
        frame.sample_rate = self.sample_rate
        frame.pts = self.pts
        frame.time_base = Fraction(1, self.sample_rate)
        self.pts += self.samples
        return frame


async def offer(request: web.Request) -> web.Response:
    params = await request.json()
    pc = RTCPeerConnection()
    request.app["pcs"].add(pc)
    peer_id = f"peer-{len(request.app['pcs'])}"
    print(f"{peer_id}: offer received")

    @pc.on("connectionstatechange")
    async def on_connectionstatechange() -> None:
        print(f"{peer_id}: state={pc.connectionState}")
        if pc.connectionState in {"failed", "closed"}:
            await pc.close()
            request.app["pcs"].discard(pc)

    @pc.on("track")
    def on_track(track) -> None:
        print(f"{peer_id}: inbound track kind={track.kind}")

        async def consume_audio() -> None:
            frames = 0
            while True:
                try:
                    await track.recv()
                except Exception as exc:
                    print(f"{peer_id}: inbound ended after {frames} frames: {exc}")
                    return
                frames += 1
                if frames == 1:
                    print(f"{peer_id}: inbound audio frame received")

        if track.kind == "audio":
            asyncio.create_task(consume_audio())

    pc.addTrack(ToneTrack())
    await pc.setRemoteDescription(
        RTCSessionDescription(sdp=params["sdp"], type=params["type"])
    )
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    for line in pc.localDescription.sdp.splitlines():
        if line.startswith("a=fingerprint:"):
            print(f"{peer_id}: {line}")
    print(f"{peer_id}: answer sent")
    return web.json_response(
        {"sdp": pc.localDescription.sdp, "type": pc.localDescription.type}
    )


async def on_shutdown(app: web.Application) -> None:
    await asyncio.gather(*(pc.close() for pc in app["pcs"]), return_exceptions=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=7860)
    args = parser.parse_args()

    app = web.Application()
    app["pcs"] = set()
    app.router.add_post("/api/offer", offer)
    app.on_shutdown.append(on_shutdown)
    print(f"listening on http://{args.host}:{args.port}/api/offer")
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
