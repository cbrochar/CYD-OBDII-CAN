# PC tools

Scripts that run on a PC, not on the CYD.

## obd_logger.py

A Python script that connects to a **USB (COM-port) ELM327-style adapter**, surveys what the
truck supports, and writes everything to one text file. Use the log to work out
the PIDs, formulas and timing to use in the CYD firmware. This is the tool that confirmed
the truck uses CAN 11-bit 500k, with the engine module at 7E0/7E8.

It sends **read-only** requests (AT commands and Mode 01 PIDs, plus any custom reads you
add).

### What it logs

1. Adapter info (`ATI`, `AT@1`, `ATRV`) and the result of each init command
2. The protocol it detected (it tries each protocol in turn if auto-detect fails)
3. Every module that answered, and each module's supported Mode 01 PIDs
4. Every supported PID read once, with raw bytes, decoded value and response time
5. The core PIDs (RPM, speed, coolant, load, MAP, intake air temp, MAF) sampled several
   times, with min/max/average and response times
6. The same PIDs with headers and spaces off, exactly as the CYD firmware receives them
7. A test of whether the adapter can listen to only one module (`ATCRA`)
8. Optional custom PIDs (see below)
9. Warnings, and a full raw transcript of every command and reply

### Requirements

- Python 3.6 or newer
- `pip install pyserial`
- A USB ELM327 adapter that shows up as a COM port (Windows) or `/dev/ttyUSB*` (Linux).
  The script doesn't work over Bluetooth unless the adapter is paired as a serial port.

### Usage

Close FORScan, PuTTY and any serial monitor first, because only one program can use the port.
Turn the key on, ideally with the engine running and warm.

```bash
python obd_logger.py --note "engine idling, warm"
```

| Option | Default | Meaning |
|---|---|---|
| `--port COM5` | asks | Serial port; skips the port picker |
| `--baud 38400` | auto | Baud rate; otherwise it tries 38400, 115200, 9600, 57600, 230400, 500000 |
| `--samples 10` | 10 | How many times to sample the core PIDs |
| `--note "..."` | none | Free-text note saved in the log header (engine state, etc.) |

The log is saved as `obd_log_<date>_<time>.txt` in the folder you run the command from.

### Reading Ford-specific PIDs

To log Mode 22 PIDs (for example ones found with FORScan), add them to `CUSTOM_PIDS` near
the top of the script:

```python
CUSTOM_PIDS = [
    {"name": "FICM main power", "header": "7E0", "request": "2209D0"},
]
```

`header` is the module's request address (the `ATSH` value), or `""` to keep the default.
Their replies are logged raw, with headers on.

> **Only add read requests** (service `22`). Requests with services `2E`, `2F`, `31` or `10`
> can command modules or change settings.

### Note

The log header has the vehicle hard-coded as "2005 Ford Excursion 6.0L diesel". Edit that
line in `main()` if you use the script on another vehicle.

## can_compare.py

Finds which CAN IDs and bytes carry a value, using recordings from the CYD's SD card (the
`CANnnnn.LOG` and `CANnnnn.TXT` files the sniffer page's **REC** and **MARK** buttons write).
It only reads files and needs nothing but Python 3.6 or newer.

### Recording for it

Each test is easiest as one recording with a **MARK** at the moment something happens:

1. Flash the `sniffer` build, plug into the truck, key on.
2. On the sniffer page tap **REC** and wait about 10 seconds.
3. Do one thing (e.g. shift 2H → 4H), and tap **MARK** right after it.
4. Wait about 10 seconds, then do the next thing and **MARK** again, and so on.
5. Tap **STOP** and copy the files from the card to the PC.

Write down what each mark was (mark 1 = 2H → 4H, mark 2 = 4H → 4L, ...). For things that
take longer, like a drive, make separate recordings instead (one parked, one driving).

### Usage

Copy the `.LOG` and `.TXT` files to one folder and run:

```bash
python can_compare.py CAN0001.LOG --mark 1           # what changed at MARK 1
python can_compare.py CAN0001.LOG CAN0002.LOG        # what differs between two recordings
python can_compare.py CAN0001.LOG                    # summary: each ID and what its bytes do
python can_compare.py CAN0001.LOG --id 3B3           # every change of one ID, with the marks
```

| Option | Default | Meaning |
|---|---|---|
| `--mark N` | | Compare the seconds before MARK N with the seconds after it |
| `--window 5` | 5 | Seconds each side of the mark |
| `--gap 0.5` | 0.5 | Seconds right at the mark to skip (while things are still changing) |
| `--from 10` / `--to 60` | | Only use frames in this time range (seconds, as printed in the log) |
| `--id 3B3` | | Print each change of that ID's data, with `^^` under the changed bytes |
| `--top 40` | 40 | How many differences to show |
| `--all` | | Also list the IDs that didn't change |

### Reading the results

The comparison lists differences, strongest first. Rolling counters (a byte that counts up in
every frame) are ignored.

| Label | Meaning |
|---|---|
| **STRONG** | A byte, or a single bit, that holds one value before and a different value after. These are the best candidates for a switch or state: transfer case, brake, gear, door. |
| **likely** | An ID that starts or stops being sent; a byte that starts or stops changing (e.g. a wheel speed leaving 0); or a one-step change, which may just be a slow value such as a temperature |
| **maybe** | A value that is moving in both and shifted (e.g. a speed that went up), or a change in frame rate or length |

Example, from a test recording with a state byte and a flag bit planted at MARK 1:

```
  STRONG  3B3  byte 2: 02 -> 04 (steady in both)
  STRONG  211  byte 1 bit 5 (mask 20): 0 -> 1 (steady in both, rest of the byte changes)
  likely  5B5  only in B (40 frames, 8.0/s): starts being sent
```

Then check a candidate with `--id`, for example `--id 3B3`. If the byte changes at every mark
where you did the same thing, and at no other time, that's the value. Repeat the action a few
times in one recording (2H → 4H → 2H → 4H) to be sure.

For moving values (wheel speeds), compare a parked recording with a driving one, then use `--id`
on the candidates and look for bytes that rise and fall with speed. Pairs of bytes (high, low)
are common, e.g. `0B B8` = 3000.
