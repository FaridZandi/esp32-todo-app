# Todo Checker

A small, offline daily checklist for the Waveshare ESP32-S3-Touch-LCD-3.49.

The initial task list is deliberately hardcoded. Tap anywhere on a task card to
cycle its state, and drag the task list up or down to scroll it directly under
your finger. The display is used
in its natural 172 × 640 portrait orientation. The arrows move between
independent, persistent day views. Completed tasks turn their whole card green.
Each tap cycles a task through default, done (green), and not done (red).
Filter hides the cards that were explicitly marked either done or not done when
the button was pressed, leaving only the unresolved tasks.

The header shows a voltage-based battery estimate. It is refreshed every 30
seconds; the Stats page also shows the estimated percentage and measured
voltage. The board does not expose reliable USB/charging-state detection, so
this is a battery-level estimate rather than a charging indicator.

Checklist state is stored in the ESP32's NVS flash and survives resets and power
loss. To make dates real calendar dates, copy `src/Secrets.example.h` to
`src/Secrets.h`, fill in 2.4 GHz Wi-Fi credentials, then flash. The app fetches
time from NTP in the background; it otherwise uses the firmware build date as a
temporary fallback.

## Private task list

Your task names belong only in `src/Tasks.h`, which is Git-ignored. To set up a
new checkout, copy `src/Tasks.example.h` to `src/Tasks.h` and edit that local
file. The public repository contains only the generic example list.

## Build and flash

```bash
pio run -e rev2 -t upload && pio device monitor
```

Use `rev1` for the original board revision. The revisions swap the backlight and
touch-interrupt pins, and rev1 also needs a different QSPI host.

## Next design decisions

- How tasks are authored: a small web page, a phone app, SD-card import, or a
  deliberately constrained on-device editor.
