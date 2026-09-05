# Scrub unificato — `seu_mitigation` (T1.6)

**Stato**: rimosso in T1.6 (branch `fix/scrub-unify`). Le funzioni descritte
in questo file vivevano in `App/obsw/scrub.{c,h}` e sono state cancellate;
la protezione SEU del W2-5 ora è fornita interamente da
`Core/Src/seu_mitigation.c`. Vedi [`seu_mitigation.md`](seu_mitigation.md)
per il design canonico.

## Cosa c'era qui

Il modulo rimosso implementava un *write-through* verso una copia "golden"
CRC-32-protected nella FRAM (FeRAM radiation-tolerant), con una task
periodica a bassa priorità (5 s) che rileggeva i record, ne verificava il
CRC e riscriveva la RAM live in caso di mismatch. Esponeva:

- `scrub_register(ram, len, id)` / `scrub_sync(id)` — registro e snapshot
  trusted in FRAM (write-through ad ogni mutazione).
- `scrub_refresh(id)` / `scrub_tick()` — verifica CRC del golden,
  confronto con live, restore da golden se differiscono.
- `scrub_init()` — boot-time restore dopo un parity-NMI reboot (W2-3).
- `scrub_task_create()` — task FreeRTOS a bassa priorità.
- `scrub_repair_count()` / `scrub_fram_error_count()` — telemetria.
- `scrub_bind_fram()` (solo build host) — seam per iniettare un modello
  FRAM nei test Ceedling.

## Perché è stato rimosso

Tre task in volo coprivano la stessa protezione sullo stesso oggetto
(`obsw_state`):

1. `App/obsw/scrub.c` — copia golden in FRAM, refresh 5 s.
2. `Core/Src/seu_mitigation.c` — shadow in SRAM2 (parity-covered),
   reference CRC in SRAM1, vote a 3 gambe, refresh 5 min.
3. `Core/Src/sram2_parity.c` (W2-3) — NMI hardware su ogni byte letto.

`seu_mitigation` ha policy di safety più forti del modulo rimosso
(3-way vote + escalation policy + lock PRIMASK NMI-safe + RTC backup
register counter), copre più regioni (OBSW_STATE + LastStates + 3 comms
TOUCH) ed è già ampiamente coperto da test (`test_laststates.c`,
`test_memory_faults.c`, `test_boot_policy.c`). Il golden-copy in FRAM
era ridondante: la shadow SRAM2 è già la copia trusted che la task
periodica usa per riparare, e la parity hardware copre la shadow stessa.

In più, `main.c` riportava un commento *load-bearing* che documentava
l'ordine obbligatorio `scrub_init()` → `seu_mitigation_init()` per
evitare che i due sistemi "raced" sullo stesso oggetto. Era una *race
riconosciuta* e fragile, non una feature: rimuovendo il primo
sparisce anche la fragilità.

## Cosa è stato aggiornato

- Rimossi `App/obsw/scrub.c`, `App/obsw/scrub.h`,
  `JOS/test/test/test_scrub.c`.
- Aggiornati i 6 call site in `App/obsw/state_machine.c` e i 3 in
  `Core/Src/main.c` (le 3 chiamate in `main()`: `scrub_init()`,
  `scrub_task_create()`, e l'include).
- Aggiornati i commenti che citavano `scrub.h` /
  `App/obsw/scrub.c` in `App/memory/memory.c`, `App/obsw/watchdog.c`,
  `App/obsw_types.h`.
- Aggiornata la tabella W2-5 in `docs/dev/hardening.md`.
- Questa pagina è il changelog di rimozione; `seu_mitigation.md` resta
  la documentazione canonica.

## Copertura test

`JOS/test/test/test_scrub.c` (≈10 unit test: register/sync/refresh-repair/
boot-init/FRAM-CRC-reject/transport-failure/invalid-args) è stato rimosso
perché testava un modulo che non esiste più. La logica equivalente
(copy-in-shadow, vote, repair) è già coperta da `test_laststates.c`,
`test_memory_faults.c` e `test_boot_policy.c` sotto il nome canonico.
Suite Ceedling post-refactor: 144 → 9 test eseguibili (test_scrub.out
rimosso); tutti i restanti passano, exit code 0.