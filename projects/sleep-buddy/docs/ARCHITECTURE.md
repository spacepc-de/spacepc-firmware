# Sleep Buddy architecture

This document describes the firmware currently implemented in
`projects/sleep-buddy`. Sleep Buddy keeps audio capture, model inference, UI and
storage separate so display work cannot interrupt microphone capture.

## Runtime flow

```text
ES7210 dual microphones, 32 kHz / 16-bit stereo
                |
                +---- optional WAV writer -> microSD
                |
                v
       Mic 2 mono PCM callback
                |
                v
       FreeRTOS audio queue
                |
                v
       2-second rolling window
                |
                v
       EfficientAT int8 frontend and model
                |
                v
       snoring probability every 1.25 seconds
                |
                v
       event detector and hysteresis
                |
                v
       session statistics, timeline and checkpoints
                |
                v
             microSD
```

## Components

| Component | Responsibility |
|---|---|
| `audio_recorder` | Initializes ES7210, captures stereo PCM, reports levels and optionally writes WAV files |
| `snore_classifier` | Implements the fixed DSP frontend and invokes the embedded EfficientAT TFLite Micro model |
| `sleep_engine` | Queues Mic 2 audio, runs inference, owns monitoring state and coordinates persistence |
| `snore_event_detector` | Converts probabilities into events using start/end thresholds and temporal hysteresis |
| `sleep_session` | Aggregates time, events, longest phase, acoustic score and minute-level timeline |
| `sleep_storage` | Persists history, event logs and power-loss checkpoints to microSD |
| `wifi_manager` | Connects the ESP32-C6 Wi-Fi companion and synchronizes NTP time |
| `time_zones` | Provides the UI time-zone list and applies the selected POSIX zone |
| `app_settings` | Stores Wi-Fi, time zone and threshold settings in NVS |
| `recorder_ui` | Owns the landscape LVGL screens and touch interactions |

## Scheduling and memory

The audio callback converts 512 stereo frames to Mic 2 mono and places them in
a 96-block FreeRTOS queue. The queue holds approximately 1.536 seconds of audio,
which absorbs the measured EfficientAT inference latency while capture
continues. The inference task runs at low priority on CPU 0 and yields after each
prediction so LVGL and display maintenance remain responsive.

The model consumes two seconds, or 64,000 mono samples, per inference. After a
prediction the window advances by 40,000 samples, producing a 1.25-second
stride. The rolling audio window is allocated in PSRAM when available.

## Event state machine

The UI-configured start threshold is limited to 5-90 percent. The end threshold
is derived as 60 percent of the selected start value with a minimum of 5
percent.

```text
inactive
  -> one probability >= start threshold
active
  -> four consecutive probabilities <= end threshold
inactive + completed event
```

Events shorter than one second are discarded. A completed event stores its
start and end offset, mean and peak probability, and peak relative input level.
Changing the UI threshold does not retrain or alter the classifier.

## Persistence

`NIGHTS.BIN` is the authoritative append-only night history. Versioned records
contain Unix start/end times, aggregate statistics and a minute-level snoring
bitset. `NIGHTS.JL` and `EVENTS.JL` are human-readable convenience logs.

During monitoring, `CURRENT.BIN` receives checksummed checkpoints at least every
30 seconds and after a completed event. At startup the latest valid checkpoint
is recovered unless an equivalent completed record already exists. This makes a
power interruption recoverable without duplicating the night.

The most recent summary is also cached in NVS for display fallback. The microSD
binary history remains authoritative.

## Time handling

Classification is completely offline. Wi-Fi is used only for NTP time. The
selected time zone is applied locally, and Unix timestamps are stored with each
night so the UI can render correct local clock ranges. Without valid NTP time,
monitoring still works but summaries cannot provide reliable wall-clock times.

## UI behavior

The Monitor page renders the live probability on an inner arc and the event
threshold as a draggable outer marker. The LVGL refresh timer continues updating
the inner arc while the numeric label is temporarily reserved for the selected
threshold during a drag. A release or lost press immediately restores the latest
live confidence.

Summary timeline selection maps the touch position to one of 96 display
segments, then finds the corresponding minute range in the stored bitset. Green
indicates quiet time and red indicates detected snoring.

Turning the display off disables the LCD panel and backlight only. The audio
capture and inference tasks continue running, and the touch overlay wakes the
display without changing monitoring state.

## Non-medical score

The 0-100 acoustic score is derived from detected snoring percentage, event rate
and longest event. It is a compact visualization of acoustic detections, not a
sleep-quality measurement or diagnostic result.
