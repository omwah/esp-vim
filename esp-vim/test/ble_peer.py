#!/usr/bin/env python3
"""
The Bluetooth side of the BLE gate (esp-vim/test/ble.py), run with pixi's Python
(Bumble is a pixi PyPI dependency).

esp-emu intercepts the firmware's Bluetooth controller (the VHCI calls) and
forwards HCI to a TCP server: --ble-hci tcp:127.0.0.1:PORT. This script is that
server. It gives the device a virtual controller on a virtual radio link, and on
the same link a second controller running a peripheral that advertises NAME from
the public address ADDR. A scan on the device should find it.

    ble_peer.py PORT NAME ADDR        # prints BLE-PEER-READY, runs until killed

BUMBLE_LOGLEVEL=DEBUG logs the HCI traffic to stderr.
"""

import asyncio
import logging
import os
import sys

from bumble.controller import Controller
from bumble.core import AdvertisingData
from bumble.device import Device
from bumble.hci import Address, LeFeatureMask, OwnAddressType
from bumble.host import Host
from bumble.link import LocalLink
from bumble.transport import open_transport
from bumble.transport.common import AsyncPipeSink


async def main(port, name, addr):
    link = LocalLink()
    # The device's controller, reached over TCP by the emulator.
    transport = await open_transport(f"tcp-server:127.0.0.1:{port}")
    device = Controller("device", host_source=transport.source, host_sink=transport.sink,
                        link=link)
    # Bumble reports advertisements in the extended format whenever the
    # controller *supports* extended advertising, even to a host that scanned
    # with the legacy commands -- which a real controller doesn't do, and which
    # the firmware's NimBLE (built without extended advertising) ignores.
    device.le_features &= ~LeFeatureMask.LE_EXTENDED_ADVERTISING

    # The peripheral: a controller on the same link, with a Bumble host.
    peer_ctrl = Controller("peer", link=link, public_address=addr)
    peer = Device(name=name, address=Address(addr),
                  host=Host(peer_ctrl, AsyncPipeSink(peer_ctrl)))
    await peer.power_on()
    await peer.start_advertising(
        own_address_type=OwnAddressType.PUBLIC,
        advertising_data=bytes(AdvertisingData([
            (AdvertisingData.FLAGS, bytes([0x06])),
            (AdvertisingData.COMPLETE_LOCAL_NAME, name.encode()),
        ])),
        auto_restart=True)
    print("BLE-PEER-READY", flush=True)
    await asyncio.get_running_loop().create_future()


if __name__ == "__main__":
    logging.basicConfig(level=os.environ.get("BUMBLE_LOGLEVEL", "WARNING").upper())
    asyncio.run(main(int(sys.argv[1]), sys.argv[2], sys.argv[3]))
