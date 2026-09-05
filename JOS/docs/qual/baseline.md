# T0.1 — Baseline misurabile del repo JOS

Snapshot dello stato attuale del repository al branch `docs/baseline`,
misurato in ambiente host (sottosezione 1–2) e definito per CI / banco HW
(sottosezioni 3–4). Ogni numero riportato è verificabile con il comando
indicato accanto.

| Campo | Valore |
|---|---|
| HEAD branch | `docs/baseline` |
| HEAD commit | `cbd60dd docs(tasks): task 15 done — full-codebase review, PR #55 merged` |
| Commit `main` (HEAD `origin/main`) | `cbd60dd` (identico al branch baseline) |
| Working tree | clean |
| Piattaforma | Linux x86_64, gcc 15.2.0 (Ubuntu), Ruby 3.3.8, Ceedling 1.1.7 |
| Toolchain ARM su host | **assente** (`which arm-none-eabi-gcc` → vuoto) |

---

## 1) Suite Ceedling host — riesecuzione

Comando (eseguito in `JOS/test/`):

```sh
ceedling clobber && ceedling test:all
```

Toolchain installato al volo per la misura:
`apt-get install -y ruby-full` (Ruby 3.3.8) → `gem install ceedling`
(risultato: `ceedling 1.1.7` da `/usr/local/bin/ceedling`).
Una nota operativa: la `ceedling` di sistema lamenta
`uninitialized constant Thor` se invocata senza `ruby -I…/thor-1.5.0/lib`
nel load path; workaround usato solo per il lancio, non cambia l'esito.

Risultato (estratto di `/tmp/ceedling_run.log`):

```
👟 Building Test Executables
----------------------------
Linking test_bad_region.out...       Linking test_boot_crc.out...
Linking test_boot_policy.out...      Linking test_boot_unstamped_bench.out...
Linking test_comms.out...            Linking test_laststates.out...
Linking test_memory_faults.out...    Linking test_scrub.out...
Linking test_watchdog.out...

👟 Executing
------------
[9 eseguibili lanciati]

-----------------------
✅ OVERALL TEST SUMMARY
-----------------------
TESTED:  144
PASSED:  144
FAILED:    0
IGNORED:   0

Ceedling operations completed in 4.71 seconds
```

| Metrica | Valore |
|---|---|
| Test eseguiti | 144 |
| PASS | 144 |
| FAIL | 0 |
| IGNORED | 0 |
| Exit code | **0** |
| Exit code `make test` wrapper | **0** (`make -C JOS/test test` → `EXIT=0`) |
| Exit code `ceedling test:all` diretto | **0** |
| Esecuzioni binarie | 9 (`test_bad_region`, `test_boot_crc`, `test_boot_policy`, `test_boot_unstamped_bench`, `test_comms`, `test_laststates`, `test_memory_faults`, `test_scrub`, `test_watchdog`) |

## 2) Warning build host

`JOS/test/project.yml` impone ai test:

```yaml
:flags:
  :test:
    :compile:
      :*:
        - -std=gnu11
        - -Wall
        - -Wextra
        - -Wno-unused-parameter
        - -Werror
```

L'opzione `-Werror` promuove ogni warning diagnostico a errore di
compilazione; la build `ceedling test:all` sarebbe quindi **fallita
all'atto del linking** se anche un solo warning fosse stato emesso.
Il rebuild pulito (`ceedling clobber && ceedling test:all`) ha
completato senza errori e con exit 0.

Conteggio esplicito, calcolato sull'output completo di `ceedling test:all`
(circa 1.300 righe, file `/tmp/ceedling_run.log`):

```sh
grep -cE "warning:|error:" /tmp/ceedling_run.log
# → 0
```

| Metrica | Valore |
|---|---|
| Warning di compilazione (host, Ceedling) | **0** |
| Errori di compilazione (host, Ceedling) | **0** |
| Warning build firmware (host, ARM) | **solo CI** — `arm-none-eabi-gcc` non è installato sull'host (`which arm-none-eabi-gcc` → vuoto); il build ARM gira solo nel job `firmware-build` di `.github/workflows/build.yml` |

## 3) Protocollo misure banco HW

Definizione del protocollo per la campagna di misura a bordo (qualifica).
Le celle "valore" sono **vuote** e vanno popolate su HW con la strumentazione
indicata; ogni riga cita lo standard di riferimento e il modo in cui il
numero verrà estratto.

### 3.1 Consumo (run / sleep)

| Modalità | Corrente media [mA] | Picco [mA] | Tensione bus [V] | Strumentazione | Standard |
|---|---|---|---|---|---|
| Run nominale (state = ACTIVE, TX LoRa off) | — | — | 3,3 V / 5 V | shunt 0,1 Ω + oscilloscopio / INA219 su EPS | ECSS-E-ST-10-12 |
| Run + TX LoRa (potenza 14 dBm) | — | — | — | idem | idem |
| Sleep (FreeRTOS tickless, state = READY) | — | — | — | idem | idem |
| Off / power-down | — | — | — | idem | idem |

Procedura: alimentazione da bench PSU calibrata, shunt strumentale in
serie al rail 3V3 OBC; campionamento ≥ 1 kSa/s, durata ≥ 60 s per stato;
ripetere 3 run e riportare media ± deviazione.

### 3.2 Size `.text` / `.data` / `.bss` (da artifact CI)

| Sezione | Dimensione [B] | Limite budget [B] | Sorgente |
|---|---|---|---|
| `.text` (codice + rodata) | — | 512 KiB | `arm-none-eabi-size build/JOS.elf` |
| `.data` (inizializzate, RAM) | — | — | idem |
| `.bss` (zero-init, RAM) | — | — | idem |
| Flash totale occupata | — | — | idem |
| Stack riservato (`_estack - _Min_Stack_Size`) | — | — | da `STM32L496VGTX_FLASH.ld` |

Procedura: scaricare l'artefatto `firmware-build` (ELF) dall'ultima run
GitHub Actions su `main`, eseguire localmente `arm-none-eabi-size
--format=sysv build/JOS.elf` (sysv per avere tutte le sezioni, berserk
per il solo riepilogo); budget come da `docs/dev/hardening.md`
(512 KiB riservati al firmware su 1 MiB totali).

### 3.3 High-water stack per task

| Task | Allocato [word] | High-water [word] | Margine [%] | Sorgente |
|---|---|---|---|---|
| `defaultTask` | — | — | — | `uxTaskGetStackHighWaterMark` runtime |
| `comms` | — | — | — | idem |
| `state_machine` | — | — | — | idem |
| `clear` | — | — | — | idem |
| `cloud` | — | — | — | idem |
| `aocs` | — | — | — | idem |
| `rx` (LoRa) | — | — | — | idem |

Procedura: aggiungere in una build di qualifica (NON flight) il log
periodico di `uxTaskGetStackHighWaterMark(NULL)` su ciascun task
(`vTaskDelay(10000)`), stressare per 24 h con worst-case workload
(comms TX/RX continuo, AOCS B-dot loop a 10 Hz, state cycling),
raccogliere il minimo storico; confrontare con l'allocazione statica
in `main.c`.

## 4) Snapshot versioni (CI)

Versioni **pinnate** del job `build.yml` (riferimento autorevole:

* `.github/workflows/build.yml` — sezione `Install pinned cppcheck`,
  `Install ARM GCC toolchain 14.3.Rel1`, `Install Ceedling`, `Install gcovr`).

| Componente | Versione | Pin | Note |
|---|---|---|---|
| Runner image (static-analysis) | `ubuntu-24.04` | pinned, NON `ubuntu-latest` | `build.yml:32` |
| Runner image (firmware-build, unit-tests) | `ubuntu-latest` | — | `build.yml` |
| cppcheck | **2.13.0** (deb `2.13.0-2ubuntu3` con fallback `2.13.0*` e build da sorgente `tag 2.13.0`) | hard-asserted in `build.yml` (`grep -qE …`) e in `JOS/Makefile` (`CPPCHECK_VERSION`) | cambiare in PR unica dopo aver ri-girato il gate |
| ARM GNU toolchain | **14.3.Rel1** (`arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi.tar.xz`, scaricato da developer.arm.com) | pinned | job `firmware-build` |
| Ruby (CI) | **3.1** (`ruby/setup-ruby@v1`) | pinned | job `unit-tests` |
| Ceedling | **1.0.1** (`gem install ceedling -v 1.0.1 --no-document`) | pinned | job `unit-tests` (da notare: diverso da 1.1.7 installato al volo sull'host solo per la misura di cui al §1; il gate CI non cambia) |
| Unity/CMock | dal gem Ceedling (`:which_ceedling: gem`) — no `vendor/unity` | by construction | vedi `JOS/test/project.yml` |
| gcovr | ultima da `pipx install gcovr` (versione stampata nel log job) | non pinnata, solo dump | `gcovr --version` |
| gcc host (questa misura) | 15.2.0 (Ubuntu) | non rilevante per il gate | solo per compilazione host |

---

## Comandi di verifica (riproducibili)

```sh
# 1) Suite Ceedling
cd JOS/test && ceedling clobber && ceedling test:all   # exit 0, 144/144 PASS

# 2) Conteggio warning/errore build host
grep -cE "warning:|error:" /tmp/ceedling_run.log        # 0

# 3) Size firmware (richiede artifact CI; NON eseguibile qui)
arm-none-eabi-size --format=sysv build/JOS.elf

# 4) Versioni CI: leggere .github/workflows/build.yml, sezioni
#    "Install pinned cppcheck" / "Install ARM GCC toolchain 14.3.Rel1"
```

## Note / limiti della presente misura

- Toolchain ARM assente sull'host → la sezione 2 per il **build firmware**
  è marcata "solo CI". Per la sezione 3.2 il dato `.text/.data/.bss` va
  letto dall'artefatto CI, non dall'host.
- La versione Ceedling usata sull'host (1.1.7) differisce da quella
  pinnata in CI (1.0.1); entrambe eseguono gli stessi sorgenti di test e
  producono lo stesso verdetto (144/144 PASS), ma il numero autorevole
  da citare in documenti/CI è **1.0.1**.
- `make -n -C JOS build` (dry-run) non genera l'ELF: la dipendenza da
  `arm-none-eabi-gcc` non è risolvibile sull'host. Conferma ulteriore
  che il dato di build host per il firmware è "solo CI".
