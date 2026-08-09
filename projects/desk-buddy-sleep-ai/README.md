# Desk Buddy Sleep AI

Local-first snore detection research for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.
This is not a medical device and does not diagnose sleep apnea or sleep disorders.

## Milestone 0: audio recorder

The current firmware is deliberately not an ML classifier yet. It validates the final hardware data path:

- onboard ES7210 microphone input
- 16 kHz, 16-bit mono PCM
- FAT32 microSD storage
- valid WAV header finalized when recording stops
- touch-controlled start/stop
- live RMS and peak level in dBFS
- no Wi-Fi, cloud upload, or permanent background recording

Insert a FAT32 microSD card before booting. Tap **Start recording**, generate the test audio, then tap
**Stop & save** before removing the card. Files are named `/SN0001.WAV` through
`/SN9999.WAV`. The first ES7210 channel is selected; channel comparison and configurable gain
are intentionally left for the hardware measurement pass.

## Build

Use ESP-IDF 5.5:

```sh
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

## Dataset privacy

Recordings exist only on the inserted microSD card. This prototype has no network component. Delete or
retain recordings according to the consent of everyone who could be recorded.
