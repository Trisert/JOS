# AOCS API — Attitude Control (`App/aocs/`)

The OBC also acts as the AOCS microcontroller.

## Modes

- **Detumbling (B-dot):** angular rate from ASM330LHHXTR IMU @ 50 Hz; magnetometer
  (IIS2MDC) field vector; drives 3 magnetorquers (TIM2 PWM). Exit when all-axis
  rate < 5 deg/s.
- **Nadir-Pointing (EKF):** state = [quaternion(4), omega(3)]; predict with
  Euler @ 50 Hz, update with magnetometer. Ported from validated MATLAB model.

## API

| Function | Purpose |
|----------|---------|
| `iis2mdc_init()` / `iis2mdc_read()` | Magnetometer (I2C1, 50 Hz) |
| `asm330lhh_init()` / `asm330lhh_read()` | IMU (SPI1, 50 Hz) |
| `aocs_bdot_step()` | B-dot corrective dipole |
| `aocs_ekf_predict()` / `aocs_ekf_update()` | EKF steps |

> Status: integrated (placeholder control law in `aocs.c`).
