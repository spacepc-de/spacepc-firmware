# Sleep Buddy

Sleep Buddy is a local snoring monitor for the
Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3. It analyzes the board's integrated
microphones directly on the ESP32-P4, presents the current result on the touch
display and stores night summaries on a microSD card.

Sleep Buddy is not a medical device. It does not diagnose sleep apnea, sleep
disorders, sleep stages or sleep quality.

## Features

- local EfficientAT AudioSet snoring inference without a cloud connection;
- live snoring confidence and an adjustable event threshold;
- monitoring with optional 32 kHz, 16-bit stereo WAV recording;
- elapsed time, detected event count and live event state;
- a morning summary with monitored time, detected snoring time, longest phase,
  event count and an explicitly non-medical acoustic score;
- a touch-selectable night timeline showing quiet and detected-snoring periods;
- seven-night history stored on microSD;
- periodic checkpoints and recovery of an interrupted session after power loss;
- Wi-Fi, NTP time synchronization and selectable time zones;
- display off with touch-to-wake.

## Hardware

- Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
- the two onboard microphones connected through the ES7210 codec
- FAT32-formatted microSD card
- a stable USB power supply

Insert the microSD card before powering on the board. Monitoring requires the
card because every completed or recovered night is persisted locally. Cards
larger than 32 GB must still use a FAT32 partition; exFAT is not supported by
this firmware.

## Using Sleep Buddy

1. Open **Settings**, configure Wi-Fi and select the correct time zone. Sleep
   Buddy obtains the clock from NTP. Network access is not needed for audio
   classification, but synchronized time is needed for useful clock times in
   summaries.
2. Return to **Monitor**. The center of the circle shows live **Snore
   Confidence**.
3. Drag the pink marker around the circle to change the event threshold. While
   dragging, the center shows **Snore Threshold** and the selected percentage.
   Releasing the marker saves the value and restores live confidence.
4. Tap the green **Start** button for analysis without storing audio, or the red
   **Start + Record** button to analyze and also save a WAV file.
5. End the session with **End Night** or **End + Save**. Sleep Buddy opens the
   summary automatically.

The threshold can be set from 5 to 90 percent and defaults to 50 percent. One
model result at or above the selected threshold starts a candidate event. Four
results below the lower end threshold finish it. This hysteresis avoids rapid
on/off transitions while still allowing an individual snore at a normal
breathing cadence to create an event.

## Screens

| Screen | Purpose |
|---|---|
| Monitor | Live confidence, threshold marker, elapsed time, events and start/stop actions |
| Summary | Results and touch-selectable timeline for the last completed night |
| History | Snoring duration for the seven most recent stored nights |
| Settings | Wi-Fi credentials, connection state, NTP state and time zone |

The **Off** button turns the panel and backlight off. A touch wakes the display;
audio analysis continues independently while the display is off.

## microSD files

| File | Contents |
|---|---|
| `NIGHTS.BIN` | Authoritative binary night history including the snoring timeline |
| `NIGHTS.JL` | Human-readable JSON Lines summary for each completed night |
| `EVENTS.JL` | Human-readable event start/end, confidence and relative sound level |
| `CURRENT.BIN` | Checksummed rolling checkpoint of the active night |
| `ST0001.WAV` ... `ST9999.WAV` | Optional stereo recordings made by **Start + Record** |

The active-night checkpoint is updated after completed events and at least every
30 seconds. On the next boot, a valid interrupted night is recovered into the
normal history before the checkpoint is removed.

Wi-Fi credentials, the selected time zone and the threshold are stored in ESP32
NVS, not in the microSD history files.

## On-device model

The firmware embeds the int8 EfficientAT AudioSet MN04 model reduced to its
pretrained `Snoring` output. The runtime uses microphone 2 and the same frontend
as desktop validation:

```text
Mic 2, 32 kHz mono
  -> 2-second window
  -> pre-emphasis
  -> 1024-point STFT
  -> 128 Kaldi Mel bands
  -> fixed normalization
  -> int8 EfficientAT inference
  -> snoring probability
  -> event hysteresis
  -> summary and timeline
```

Inference uses a 1.25-second stride. The model, DSP parameters and quantization
are not modified by the UI threshold; the threshold only controls conversion of
model probabilities into stored events.

The model is a practical prototype validated on the target board, not a
clinically validated classifier. Overnight false-positive and event-recall
testing across different rooms, people and distances remains necessary before
calling it a production model.

## Build and flash

ESP-IDF 5.5 is the validated toolchain. The required model is already present in
`local_models/` and is embedded by CMake.

```sh
cd projects/sleep-buddy
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.wchusbserial5B901592401 flash monitor
```

The generated application image is `build/sleep_buddy.bin`. GitHub Actions also
compiles this project with ESP-IDF 5.5 whenever its firmware, shared Waveshare
board component or workflow changes.

## Project layout

```text
projects/sleep-buddy/
  docs/                 Technical architecture
  local_models/         Embedded int8 model
  main/                 ESP-IDF firmware, audio, inference, storage and LVGL UI
  ml/                   Reproducible model evaluation and export tools
  CMakeLists.txt
  partitions.csv
  sdkconfig.defaults
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for task boundaries, runtime
flow and persistence details.

## Privacy and safety

Audio classification is local. A normal **Start** session stores only derived
event and summary data. Full audio is written only after the user explicitly
selects **Start + Record**. Obtain consent from everyone who may be recorded and
delete recordings when they are no longer required.

Do not use Sleep Buddy to make medical decisions. Persistent or concerning
snoring and suspected breathing interruptions require assessment by a qualified
medical professional.
