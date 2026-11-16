#!/usr/bin/env python3
"""Interop: the firmware's ESPHome API code vs the real Home Assistant client.

t_6215e36e. Drives tests/host/esphome_api_host_server (the firmware's
esphome_api.c + ble_hci.c behind a loopback socket) with aioesphomeapi at the
version Home Assistant 2026.10.2 pins, along the path HA's ESPHome integration
takes for a Bluetooth proxy (homeassistant/components/esphome/manager.py
_on_connect + bleak_esphome connect_scanner):

  connect(login=False) with expected_name -> device_info_and_list_entities()
  -> feature flags say passive scanner, raw adverts, not connectable
  -> subscribe_home_assistant_states_and_services()
  -> subscribe_bluetooth_le_raw_advertisements()
  -> keepalive pings answered for > 4.5 x keepalive (the client's timeout)
  -> disconnect(): server sees DisconnectRequest and exits 0.

Usage: test_esphome_api_interop.py <server-binary>
"""
import asyncio
import subprocess
import sys

from aioesphomeapi import APIClient, BluetoothProxyFeature
from bluetooth_data_tools import int_to_bluetooth_address, parse_advertisement_data_bytes

KEEPALIVE_S = 1.0


async def main(server: str) -> int:
    proc = subprocess.Popen([server], stdout=subprocess.PIPE, text=True)
    line = proc.stdout.readline().split()
    assert line[0] == "PORT", line
    port = int(line[1])

    cli = APIClient(
        "127.0.0.1", port, None, client_info="interop-test",
        keepalive=KEEPALIVE_S, expected_name="xvf3800-test",
    )
    await asyncio.wait_for(cli.connect(login=False), 10)
    assert (cli.api_version.major, cli.api_version.minor) == (1, 10), cli.api_version

    info, entities, services = await asyncio.wait_for(
        cli.device_info_and_list_entities(), 10)
    assert info.name == "xvf3800-test", info.name
    assert info.mac_address == "dc:b4:d9:38:a6:cc", info.mac_address
    assert info.bluetooth_mac_address == "dc:b4:d9:38:a6:ce"
    assert info.project_name == "kyzcreig.pipecat-house-satellite"
    assert not info.uses_password and not info.api_encryption_supported
    assert entities == [] and services == []
    flags = info.bluetooth_proxy_feature_flags_compat(cli.api_version)
    assert flags == (BluetoothProxyFeature.PASSIVE_SCAN
                     | BluetoothProxyFeature.RAW_ADVERTISEMENTS), flags
    assert not flags & BluetoothProxyFeature.ACTIVE_CONNECTIONS
    print(f"ok - device info: {info.name} mac={info.mac_address} "
          f"bt={info.bluetooth_mac_address} flags=0x{int(flags):02x}")

    cli.subscribe_home_assistant_states_and_services(
        on_state=lambda s: None, on_service_call=lambda c: None,
        on_state_sub=lambda e, a: None, on_state_request=lambda e, a: None)

    # Idle (nothing streaming yet) past the client's keepalive timeout
    # (4.5 x interval): the client pings, and an unanswered ping drops us.
    await asyncio.sleep(KEEPALIVE_S * 7)
    assert cli._connection is not None and cli._connection.is_connected, "dropped"
    print("ok - idle 7 s at keepalive 1 s: still connected (pings answered)")

    got = []
    cli.subscribe_bluetooth_le_raw_advertisements(lambda resp: got.append(resp))
    await asyncio.sleep(KEEPALIVE_S * 7)
    assert cli._connection.is_connected, "dropped while streaming"
    batches = len(got)
    assert batches >= 30, f"only {batches} advertisement batches in 7 s"
    advs = got[-1].advertisements
    assert len(advs) == 2, len(advs)
    a, b = advs[0], advs[1]
    assert int_to_bluetooth_address(a.address) == "C0:11:22:33:44:55"
    assert a.rssi == -67 and a.address_type == 1, (a.rssi, a.address_type)
    parsed = parse_advertisement_data_bytes(a.data)
    # (local_name, service_uuids, service_data, manufacturer_data, tx_power)
    assert parsed[3] == {0x004C: bytes.fromhex("0215aabbcc")}, parsed
    assert int_to_bluetooth_address(b.address) == "00:00:00:00:00:01"
    assert b.rssi == -90 and b.address_type == 0 and b.data == b""
    print(f"ok - {batches} raw advertisement batches over 7 s with keepalive "
          f"{KEEPALIVE_S}s; C0:11:22:33:44:55 rssi -67 mfr 0x004C parsed")

    await cli.disconnect()
    out, _ = proc.communicate(timeout=10)
    print(out.strip())
    assert proc.returncode == 0, f"server exit {proc.returncode}"
    pings = int(out.split("pings_in=")[1].split()[0])
    assert pings >= 2, f"server answered only {pings} pings"
    print(f"ok - {pings} PingRequests answered")
    print("esphome_api interop (aioesphomeapi): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1])))
