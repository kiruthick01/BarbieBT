# BarbieBT

Bluetooth calling inside a Barbie toy flip phone. An ESP32 pairs with an Android phone as a
Hands-Free (HFP) device, rings with a local ringtone, answers and hangs up, and bridges the call
audio to a speaker and an I2S microphone.

ESP-IDF v5.4.x, classic ESP32 (needs Bluetooth BR/EDR; BLE-only chips such as C3/S3 will not work).
Not Arduino.

## Hardware

- ESP32-32U 38-pin devkit (external u.FL antenna required)
- MAX98357A I2S amplifier + salvaged 8 ohm phone speaker
- INMP441 I2S microphone
- 3.7 V LiPo + TP4056 charger + MT3608 boost (later stages)

## Wiring (devkit GPIO, changeable in menuconfig > Barbie BT)

| Signal | GPIO | Goes to |
|---|---|---|
| BCLK | 26 | MAX98357A BCLK + INMP441 SCK |
| WS | 25 | MAX98357A LRC + INMP441 WS |
| DOUT | 22 | MAX98357A DIN |
| DIN | 19 | INMP441 SD |

- MAX98357A: Vin = 5 V, GND, SD and GAIN unconnected.
- INMP441: VDD = **3.3 V only**, GND, L/R = GND.
- Bench test on USB power only. Do not connect the battery/boost while USB is plugged in.

## Build / flash

```
idf.py set-target esp32
idf.py menuconfig        # Barbie BT > stage
idf.py build
idf.py -p COMx flash monitor
```

Many clone devkits need BOOT held while esptool prints `Connecting...`.
Defaults live in `sdkconfig.defaults`; the stage is selected by `CONFIG_BARBIE_STAGE_*`.

## Stages

| Stage | What it does | Status |
|---|---|---|
| 2 | 440 Hz tone through the amp | works |
| 3 | INMP441 level meter (dBFS) | works |
| 4 | HFP pairing, call events, answer/hang up over serial | works |
| 5 | Ringtone, voice link, two-way audio bridge | audio flows both ways; quality tuning in progress |

Stage 5 serial keys: `a` answer, `h` hang up, `1/2/3` ringtone, `r` ring preview, `c/d` audio on/off,
`x` dump the last ~2 s of both audio streams as hex (for offline analysis).

## Notes

- `CONFIG_BTDM_CTRL_BR_EDR_MAX_SYNC_CONN` must be at least 1 or the controller rejects every voice link.
- Audio is HFP over HCI, narrowband CVSD: 8 kHz 16-bit mono, 120-byte frames. The bridge resamples to 16 kHz I2S.
- Ringtones are placeholder melodies, not final.
