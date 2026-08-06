# SEU scrubbing of critical RAM (W2-5)

Hardening card **W2-5**. Depends on **W2-3** (SRAM2 parity NMI). Standards:
**NASA-STD-8739.8** (fault tolerance, no silent data corruption),
**ECSS-E-ST-40C §5.4** (data integrity / fault tolerance),
**ECSS-Q-ST-80C** (fault detection).

## Why

W2-3 detects single-bit upsets in the parity-protected SRAM2 block and contains
them with a recorded reboot. Parity, however:

- catches only **one flipped bit per byte** — a double-bit (or multi-bit) upset
  in SRAM2 slips past the check;
- does **not** cover the 256 KB SRAM1 where most OBSW state actually lives;
- only *detects* — it does not repair the RAM it protected (the NMI reboots).

W2-5 is the **correction half** for the whole OBSW.

## Design

Every critical struct is mirrored as a CRC-32-protected "golden" record in
FRAM (radiation-tolerant FeRAM, hardening.md §5.3). The CRC guards the *backup*
itself: a golden copy with a flipped bit would otherwise silently restore wrong
data, which is worse than no backup.

| Layer | File | Role |
|-------|------|------|
| Golden copy | `App/obsw/scrub.c` / `scrub.h` | CRC-32 records in FRAM, periodic refresh + repair |
| Detection | `Core/Src/sram2_parity.c` (W2-3) | parity NMI → recorded reboot |
| Transport | `App/memory/memory.c` `fram_read`/`fram_write` (I2C FM24VN10) | real FRAM driver |
| Boot repair | `state_machine_init()` → `scrub_init()` | restore last-good struct after reboot |
| Write-through | `try_transition()` → `scrub_sync()` | golden copy never lags the live truth |

### FRAM record layout (fixed slot per region id)

```
offset 0  : magic[4]      = "SCUR"      (lets ground find records in a FRAM dump)
offset 4  : region_id     (1 byte, == slot index)
offset 5  : reserved[3]
offset 8  : crc32         (4) over payload[0 .. payload_len-1]
offset 12 : payload_len   (4)
offset 16 : payload[payload_len]   (padded to SCRUB_MAX_REGION_SIZE)
```

### FRAM allocation (coexistence with cyclic_buffer)

FRAM is 64 KB total (`memory.c`, 4× FM24VN10). The `cyclic_buffer` (payload
science data) starts at offset 0 and advances a head pointer over the whole
device. W2-5 deliberately places its pool at the **top** of FRAM and grows
downward by slot:

```
SCRUB_FRAM_BASE = 0x10000 - (SCRUB_MAX_REGIONS * SCRUB_RECORD_SIZE)
```

so the two pools never overlap in the normal case. Even if they ever did, the
scrub records are CRC-protected and the golden-copy check rejects any garbage
it does not own, so a stray `cyclic_buffer` write cannot silently restore wrong
data.

### Detection × correction interaction

- **Parity NMI (SRAM2 single-bit):** W2-3 records a `sram2_parity_record_t` in
  LastStates and reboots. On the next boot `scrub_init()` refreshes every
  registered region — including the SRAM2 `obsw_state` — from its CRC-verified
  golden copy, re-establishing the last-good struct.
- **Multi-bit / SRAM1 / missed upsets:** the 5 s scrub pass recomputes each
  live struct's CRC, and on mismatch copies the verified golden copy back and
  increments `scrub_repair_count()`. A golden-record CRC mismatch (FRAM bit
  flip) is reported via `scrub_fram_error_count()` and **not** used to restore —
  the backup is distrusted, never silently trusted.

## Integration points

- `state_machine_init()`:
  - `scrub_register(&obsw_state, sizeof(obsw_state), SCRUB_REGION_OBSW_STATE)`
  - `scrub_init()` — boot-time repair before mission logic.
- `try_transition()` (after a committed state change):
  - `scrub_sync(SCRUB_REGION_OBSW_STATE)` — write-through.
- `main()` (RTOS threads):
  - `scrub_task_create()` — low-priority 5 s scrub pass (`scrub_tick()`).

## Verification

- `make all` with arm-none-eabi-gcc (CI on PR): the firmware now links
  (`boot_crc.c` depends on the `__fw_image_start` / `__fw_crc_start` symbols
  added to `STM32L496VGTX_FLASH.ld`; the `.fw_crc` section is emitted last).
- Host unit tests (Ceedling, `JOS/test`): 14 cases green — `boot_crc` CRC-32
  vectors + `scrub` register/sync/refresh-repair/boot-init/FRAM-CRC-reject/
  transport-failure/invalid-args. Run:

  ```sh
  cd JOS/test
  ceedling test:all
  ```

## Telemetry

`scrub_repair_count()` and `scrub_fram_error_count()` are read by the beacon /
TM path so ground sees SEU activity and FRAM health.
