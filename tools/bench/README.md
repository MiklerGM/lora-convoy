# The bench on one computer

Phones, boards, Bluetooth and the air between them, all simulated, so that the boards themselves
are left to test only what is theirs: their radios, GNSS receivers, Bluetooth stacks and power
([BENCH](../../docs/BENCH.md)).

```mermaid
flowchart LR
  subgraph emulators["Android emulators (read-only copies of one AVD)"]
    A["phone 0: LoRa Convoy + OsmAnd"]
    B["phone 1: LoRa Convoy + OsmAnd"]
  end
  subgraph netsim["netsim: the emulators' virtual Bluetooth"]
    BA["LoRaConvoy-C0FFEE00"]
    BB["LoRaConvoy-C0FFEE01"]
    BC["LoRaConvoy-C0FFEE02"]
  end
  subgraph sim["sim serve: the firmware's node core on simulated air"]
    N0["board 0"] --- AIR(("slots, collisions, loss"))
    N1["board 1"] --- AIR
    N2["board 2, a beacon"] --- AIR
  end
  A -- "BLE: pairing, encrypted writes" --> BA
  B --> BB
  BA -- "phone API over TCP 4403" --> N0
  BB -- "4404" --> N1
  BC -- "4405" --> N2
```

- **`firmware/test/sim.c serve`** runs the boards: the firmware's own node core, with its
  encryption, slots, help and phone API, on simulated air.
- **`ble_board.py`** makes each board a Bluetooth peripheral in netsim, as `ble_nus.c` serves one:
  the Nordic UART Service, writes only over an encrypted link after Just Works pairing, one owner,
  FORGET, and bonds kept across restarts as a board keeps them in flash. It runs on Google's Bumble.
- **`bench.py`** starts it all and rides it: `up [PHONES]`, `ride`, `down`. The phones are
  read-only copies of one AVD, which would share its Bluetooth keys and so look like one phone to
  a board; `up` has each make keys of its own, as a real phone has. A failed ride leaves each
  phone's screen at the failure beside the logs.

## What a ride checks

1. Each phone finds its board over Bluetooth, pairs (Android's consent dialog accepted as a rider
   would), and talks to it encrypted.
2. Phone 0 starts a group with the riders' names; each other phone joins with its own
   invitation; the last board rides alone, keyed by hand, as a beacon.
3. Every phone's board hears all the others.
4. Phone 1 calls for help, and every other board answers it.
5. OsmAnd is stopped and started again, as a rider or an update does, and phone 0's map shows
   every other rider again: their colours are looked for in ten screenshots. The last frame is
   kept, for a look at the names. `bench.py engine` and `map` measure the OpenGL engine's
   blinking the same way, on the bench or on a real phone over adb.
6. Board 0 restarts and keeps its bond, as a board keeps it in flash: phone 0 comes back to it
   without pairing again.
7. Board 0 is reset by its button while phone 0 is out of reach, so phone 0's bond is stale: the
   phone pairs again once the rider agrees (Android 15 does it by itself; on phones that do not,
   the app drops the stale bond after two refused sessions, BondHealth).
8. Phone 1 forgets its board: the board drops the bond, says FORGOTTEN, and Android drops its own.
9. Phone 1, free now, tries phone 0's board, which turns it away: one owner per board.

## Running it

Once: `android/emulator.sh up`, `install` and `map`, and in OsmAnd, Plugins, switch LoRa Convoy on
(M1 in `emulator.sh`). Then:

```
python3 -I tools/bench/bench.py up 2      # about 2 minutes once built
python3 -I tools/bench/bench.py ride      # about 5 minutes; once per up, which starts the boards bond-free
python3 -I tools/bench/bench.py down
```

Bumble goes into `~/.cache/lora-convoy/bench-venv` from `requirements.txt` on the first `up`; logs,
bonds and screenshots go to `~/.cache/lora-convoy/bench`. Each phone takes about 2 GB of memory.

## What it does not check

The boards' own Bluetooth stacks (NimBLE on the Heltec, Zephyr's on the L1), their radios, GNSS
and power; a phone asleep in a pocket. Those are the bench with boards.
