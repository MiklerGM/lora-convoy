#!/usr/bin/env python3
"""Simulated boards over virtual Bluetooth, for Android emulators.

Each board that `sim serve` runs on TCP becomes a Bluetooth LE peripheral in the emulators' virtual
radio (netsim), served as the firmware's ble_nus.c serves one: the Nordic UART Service, commands
written only over an encrypted link (Just Works pairing), one owner at a time, and FORGET dropping
the bond. Lines go both ways between the phone and the board's phone API.

    ble_board.py I [PORT]    board I as LoRaConvoy-C0FFEE0I, bridged to 127.0.0.1:PORT+I (4403)

One board a process: a second netsim chip in the same process never comes up.

Needs Bumble (tools/bench/requirements.txt) and an emulator running an Android 13 or later image,
whose Bluetooth is netsim's.
"""
import asyncio
import logging
import pathlib
import sys

from bumble.core import UUID, AdvertisingData
from bumble.device import Device
from bumble.gatt import Characteristic, CharacteristicValue, Service
from bumble.hci import Address
from bumble.keys import JsonKeyStore
from bumble.pairing import PairingConfig, PairingDelegate
from bumble.transport import open_transport

NUS = UUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
RX = UUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")  # commands from the phone
TX = UUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")  # lines to the phone, notified

log = logging.getLogger("ble_board")

# Bonds kept across restarts, as a board keeps them in flash; deleting the file is the board's
# button held down.
BONDS = pathlib.Path.home() / ".cache/lora-convoy/bench"


class Board:
    def __init__(self, index: int, port: int):
        self.index = index
        self.port = port
        # a random static address (its top two bits set); the firmware's name is the prefix and
        # four bytes of its address
        self.address = Address(f"F0:C0:FF:EE:00:{index:02X}")
        self.name = f"LoRaConvoy-C0FFEE{index:02X}"
        self.device: Device | None = None
        self.connection = None
        self.writer: asyncio.StreamWriter | None = None
        self.pending = b""
        self.tx = Characteristic(TX, Characteristic.Properties.NOTIFY, Characteristic.READABLE, b"")
        rx = Characteristic(
            RX,
            Characteristic.Properties.WRITE | Characteristic.Properties.WRITE_WITHOUT_RESPONSE,
            Characteristic.WRITEABLE | Characteristic.WRITE_REQUIRES_ENCRYPTION,
            CharacteristicValue(write=self.on_write),
        )
        self.service = Service(NUS, [rx, self.tx])

    def say(self, text: str):
        print(f"board {self.index}: {text}", flush=True)

    async def run(self):
        async with await open_transport(f"android-netsim:name=lora-convoy-board-{self.index}") as hci:
            device = Device.with_hci(self.name, self.address, hci.source, hci.sink)
            self.device = device
            BONDS.mkdir(parents=True, exist_ok=True)
            device.keystore = JsonKeyStore(self.name, str(BONDS / f"board-{self.index}.json"))
            # Its identity is its static address, which does not change, as a board's does not;
            # netsim's own address for the chip changes with every start.
            device.pairing_config_factory = lambda connection: PairingConfig(
                sc=True, mitm=False, bonding=True,
                delegate=PairingDelegate(io_capability=PairingDelegate.NO_OUTPUT_NO_INPUT),
                identity_address_type=PairingConfig.AddressType.RANDOM)
            device.add_service(self.service)
            device.on("connection", self.on_connection)
            await device.power_on()
            await self.advertise()
            self.say(f"advertising as {self.name}, phone API at 127.0.0.1:{self.port}")
            await asyncio.get_running_loop().create_future()  # until stopped

    async def advertise(self):
        # The service's UUID in the advertisement, the name in the scan response: together they
        # pass the 31-byte limit (ble_nus.c, advertise()).
        adv = AdvertisingData([
            (AdvertisingData.FLAGS, bytes([0x06])),
            (AdvertisingData.COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS, bytes(NUS)),
        ])
        rsp = AdvertisingData([(AdvertisingData.COMPLETE_LOCAL_NAME, self.name.encode())])
        await self.device.start_advertising(auto_restart=True, advertising_data=bytes(adv),
                                            scan_response_data=bytes(rsp))

    def on_connection(self, connection):
        asyncio.get_running_loop().create_task(self.connected(connection))

    async def connected(self, connection):
        bonded = await self.device.keystore.get_all()
        peer = str(connection.peer_address)
        # One owner: a board that has a bond refuses every other phone (ble_nus.c, is_stranger).
        if bonded and all(name.split("/")[0] != peer.split("/")[0] for name, _ in bonded):
            self.say(f"refused {peer}: it has an owner")
            await connection.disconnect()
            return
        self.connection = connection
        self.pending = b""
        self.say(f"phone {peer} connected")
        connection.on("disconnection", lambda reason: self.disconnected(connection, reason))
        connection.on("pairing", lambda keys: self.say("paired"))
        connection.on("pairing_failure", lambda reason: self.say(f"pairing failed ({reason})"))
        try:
            reader, self.writer = await asyncio.open_connection("127.0.0.1", self.port)
        except OSError as e:
            self.say(f"no board at 127.0.0.1:{self.port} ({e})")
            await connection.disconnect()
            return
        asyncio.get_running_loop().create_task(self.lines_to_phone(connection, reader))

    def disconnected(self, connection, reason):
        self.say(f"phone gone ({reason})")
        if self.connection is connection:
            self.connection = None
            if self.writer:
                self.writer.close()
                self.writer = None

    async def lines_to_phone(self, connection, reader: asyncio.StreamReader):
        while self.connection is connection:
            line = await reader.readline()
            if not line:
                break
            await self.notify(connection, line)

    async def notify(self, connection, data: bytes):
        chunk = max(20, connection.att_mtu - 3)
        for i in range(0, len(data), chunk):
            try:
                await self.device.notify_subscriber(connection, self.tx, data[i:i + chunk])
            except Exception as e:  # the phone went meanwhile
                log.debug("notify: %s", e)
                return

    def on_write(self, connection, value: bytes):
        self.pending += bytes(value)
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            line = line.strip(b"\r")
            encrypted = connection.is_encrypted
            self.say(f"<- {line.decode(errors='replace')}" + ("" if encrypted else " (NOT ENCRYPTED)"))
            if line.strip().upper() == b"FORGET":
                asyncio.get_running_loop().create_task(self.forget(connection))
            elif self.writer:
                self.writer.write(line + b"\n")

    async def forget(self, connection):
        # As the firmware: bonds cleared first, then FORGOTTEN tells the phone it may drop its own.
        for name, _ in await self.device.keystore.get_all():
            await self.device.keystore.delete(name)
        self.say("forgot its phone")
        await self.notify(connection, b"FORGOTTEN\n")
        await asyncio.sleep(1)
        await connection.disconnect()


if __name__ == "__main__":
    import os
    logging.basicConfig(level=os.environ.get("BENCH_LOG", "WARNING").upper())
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    index = int(sys.argv[1])
    asyncio.run(Board(index, (int(sys.argv[2]) if len(sys.argv) > 2 else 4403) + index).run())
