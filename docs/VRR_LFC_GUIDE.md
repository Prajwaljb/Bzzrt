# VRR / LFC flicker: theory, test procedure and interpretation

Monitor in the examples: **BenQ EW270Q**: 2560×1440, 200 Hz, FreeSync/Adaptive-Sync, VRR range **48–200 Hz**,
DisplayPort or USB-C (DP Alt Mode). For your monitor, read 48 and 200 as its own minimum and maximum: the guided check
builds its tests from the range it detects (or that you enter), and the advanced tests take `--fps` for any rate.

---

## 1. What VRR and LFC actually do

With Adaptive-Sync the panel scans out a frame at its fastest timing (at 200 Hz one scanout takes ≈5 ms) and then
waits in an extended vertical blank for the next frame. The wait has a hard upper limit: the panel's **maximum frame
time**, 1000/48 ≈ **20.83 ms**. LCD pixels slowly drift if they are not refreshed, so the panel must scan out again
by then, new frame or not.

* **Inside the range** (5–20.83 ms per frame): every new frame starts a scanout immediately. The refresh rate follows the frame rate.
* **Below the floor** (>20.83 ms per frame): something has to send the old image again.
  * **LFC (Low Framerate Compensation)**: the **GPU driver**, not the monitor, re-sends each frame 2×, 3×, … so the
    refresh rate stays in range. Stable 47 FPS becomes 94 Hz with every frame shown twice. This needs
    max ≥ ~2 × min; 200 ≥ 96 holds.
  * **No LFC**: when the 20.83 ms limit is reached the hardware forces a repeat scanout of the old frame. A new
    frame arriving during that repeat waits up to one scanout (≈5 ms). The refresh pattern becomes irregular.

LFC is implemented by the driver, so **Windows, Linux and macOS can behave differently on the same monitor**:
AMD Windows driver ≠ amdgpu (kernel "BTR"), Intel Windows driver ≠ i915/xe, NVIDIA Windows ≠ NVIDIA Linux, and macOS
has its own display pipeline. On Linux the compositor sits in the path as well, so the timing it presents with
matters. A monitor whose range is narrower than about 2:1 (e.g. 48–75 Hz) cannot get LFC from any driver.

### Why VA panels flicker with VRR

The brightness a pixel reaches depends on how long it is held before the next refresh. Overdrive is tuned for fixed
refresh rates. When the time between refreshes changes abruptly, perceived brightness changes, most visibly in
**dark and mid grays**. VA panels are known for this. A brightness step at every refresh-rate jump is the
"VRR flicker" or strobing effect. 8-bit + FRC panels add temporal dithering, whose pattern is tied to the refresh
cadence, so cadence changes can also show as shimmer on gradients.

---

## 2. Why average FPS is not enough

The panel reacts to **every single frame interval**, not to an average:

* 60 FPS average with one 40 ms frame per second: the panel runs 60 Hz, then holds one frame past its 20.8 ms limit
  (forced repeat or LFC), then returns to 60 Hz. That gives one brightness blip per second while the average still
  reads ~59 FPS.
* A 1 % low is the 99th-percentile frame time. One spike every 10 s at 60 FPS is 0.17 % of frames: invisible in the
  1 % low, but visible on screen every 10 s.
* 47↔50 FPS switching averages ~48.5 FPS, which is above the floor, yet half of the frames are below it.

That's why the overlay and CSV report the **largest spike**, **frames below the VRR floor**, **floor crossings**, the full
**present-interval histogram**, and every frame individually in the CSV.

## 3. Why a brief spike below 48 FPS can trigger LFC (or a forced repeat)

A single 25–50 ms frame exceeds 20.83 ms. The panel cannot wait that long, so during that one frame:

1. The driver's LFC must insert a repeat (it decides *before* it knows when the next frame will come, based on
   recent frame times), **or** the hardware/monitor forces a self-refresh at the maximum frame time.
2. The next real frame may arrive while that repeat is being scanned out and has to wait up to ~5 ms. That adds
   judder and gives that frame a different hold time.
3. LFC algorithms use **hysteresis**: once they have engaged frame multiplication they may keep doubling for a few
   frames after the frame rate recovers. The refresh rate then jumps, for example 60 Hz → ~96 Hz → 60 Hz.

Each of these changes the refresh duration for one or a few frames, which on a VA panel can appear as a single flash
or dip. That is what `spikes60`, `drops55` and `lows` reproduce in a controlled, repeatable way (`--seed`).

## 4. Why stable 47 FPS can behave differently from repeated 47↔50 FPS

* **Stable 47 FPS**: once LFC has settled, the panel runs a constant ~94 Hz with every frame doubled. The cadence is
  steady, so brightness is steady. It may differ slightly from 50 Hz brightness, but it doesn't change over time.
  The OSD typically reads ~94 Hz. Without LFC, the cadence is a steady repeating pattern (see §6).
* **47↔50 switching**: each switch moves the panel between **native 50 Hz (20 ms holds)** and **LFC ~94 Hz (10.6 ms
  holds)**, or into the forced-repeat pattern. The hold time nearly halves or doubles at each switch, so brightness
  steps at every switch: flicker at the switching frequency.
* Drivers place LFC entry/exit thresholds with hysteresis (enter below the floor, exit somewhat above it). 47↔50 sits
  in the zone where implementations differ most. One driver may stay in LFC across the whole cycle (little flicker),
  another may toggle every second (strong flicker). So **the same monitor can flicker on one OS and not on
  the other** without being faulty.

`sweep` moves smoothly across the boundary. `switch60-45`, `switch100-45` and `switch200-45` make large jumps into LFC
from different starting rates.

---

## 5. Test procedure: "does it only happen on Linux, or is the display faulty?"

Keep everything identical between the two OSes: same cable and port (USB-C vs DP matters), same OSD settings
(FreeSync/Adaptive-Sync on, the **same overdrive / AMA level**, same brightness, same picture mode), 200 Hz desktop mode,
dim room, and let the monitor warm up for ~15 minutes.

Run each test for 30–60 s, overlay hidden (`H`), pattern **dark gray** (`--pattern 1`) and then **mid gray** (`--pattern 0`),
the same `--seed`, and note: flicker yes/no/strength, and the OSD refresh readout.

| Step | Tests | Purpose |
|---|---|---|
| A. VRR off (control) | `stable60`, `stable47` with VRR disabled (OSD FreeSync off, or Linux `--api kms --present fixed`) | flicker here has nothing to do with VRR |
| B. Inside the range | `stable144`, `stable100`, `stable60`, `stable50` | VRR at fixed rates must not flicker |
| C. Below the floor, steady | `stable47`, `stable45`, `stable40`, `stable30` | LFC steady state. Under LFC the OSD shows a multiple (e.g. ~94, ~90, ~80, 60–90 Hz) |
| D. Transitions | `switch47-50`, `sweep`, `switch60-45`, `switch100-45`, `switch200-45` | entry/exit of LFC |
| E. Spikes / jitter | `spikes60`, `drops55`, `jitter45-55`, `lows` | single-frame excursions |

Example:

```bash
vrrdiag --test stable100 --pattern 1 --no-overlay --start --duration 45 --seed 1
vrrdiag --test switch47-50 --pattern 1 --no-overlay --start --duration 45 --seed 1
```

### Interpreting the results

| Observation | Most likely cause |
|---|---|
| Flicker in **A** (VRR off) | Not VRR: panel/backlight, cable, USB-C dock/link, FRC. **Possible hardware fault.** |
| Flicker in **B** (steady in-range rates) on **both** OSes | Abnormal. Check the cable and port (try plain DP), then suspect the monitor. **RMA candidate.** |
| Flicker only in **C/D/E**, on **both** OSes | Typical VA VRR flicker. The monitor works as designed: avoid near-floor frame rates, cap FPS inside the range, try a lower overdrive level. |
| Flicker in **C/D/E** only on **Linux**, with identical tests | Linux driver/compositor (LFC implementation or compositor timing). **Not the monitor.** |
| Linux Vulkan flickers but `--api kms` does not | The compositor is responsible. |
| `--api kms` flickers too (Linux only) | The kernel driver's VRR/LFC handling is responsible. |
| Flicker only on Windows | Windows driver path or settings (check PresentMon: independent flip?). |

---

## 6. Reading the driver-reported timing

`display_interval_ms` (CSV) and "Driver-reported present interval" (overlay) are the gaps between presentation
timestamps reported by DXGI, Vulkan or KMS. Typical signatures on a 48–200 Hz monitor:

* **Quantized to multiples of 5 ms (1000/200)**: e.g. 55 FPS shows alternating 20 / 15 ms. The display path runs a
  **fixed 200 Hz**: VRR is not active (typical when windowed or with VRR disabled).
* **Equal to the target** (e.g. 55 FPS → 18.2 ms ± 0.5): frames are shown when submitted, so **VRR is active** in range.
* **Below the floor, uniform** (47 FPS → 21.28 ms every frame): frames are shown on arrival. Any repeats (LFC) were
  inserted in between without disturbing the cadence.
* **Below the floor, alternating ≈26 / ≈16.5 ms** at 47 FPS: the panel reached its ≈20.8 ms limit and started a forced
  repeat scanout, and the new frame waited for that scanout to finish (≈5 ms at 200 Hz timing). The next frame, arriving
  only 16.5 ms later, was shown immediately. This is the signature of **no effective frame multiplication**: the panel's
  refresh intervals jump between ~38 Hz and ~60 Hz on every frame.

  This signature was measured with this tool on Linux (KDE Plasma Wayland, Intel i915, EW270Q on DP, `stable47` and
  the 47 FPS phases of `switch47-50`). The 50 FPS phases showed uniform 20 ms intervals and in-range rates followed the
  target exactly, which confirms that VRR itself was active.

Compare the same test on Windows. If the Windows driver applies LFC there, OSD and camera will show a steady doubled
refresh for the same input.

---

## 7. What can and cannot be proven without the OSD or a camera

**Software can establish:**

* exactly when each frame was submitted (on Linux pacing was measured within ~0.05 ms of target; Windows uses a high-resolution waitable timer plus a spin and is expected to be similar; check `late` and the histogram);
* whether `Present`/`vkQueuePresentKHR` blocked, and what it returned;
* whether the frame-time sequence crossed the VRR floor, how often, and by how much;
* what the OS/driver *reports* as presentation time. Its spacing is strong evidence for whether that path runs
  fixed-rate or variable-rate (§6);
* identical, seed-reproducible input sequences on Windows and Linux.

**Software cannot establish:**

* that the panel is in VRR mode. A variable frame rate does not imply variable refresh, and no Wayland, DXGI or
  Vulkan API reports "VRR active";
* whether and when LFC inserted repeated scanouts. Repeats are invisible to every API, including KMS flip events;
* the refresh rate the monitor scaler actually runs. Driver timestamps can be derived from scanout position or vblank
  counters, and on Wayland they come from the compositor;
* brightness changes or flicker. Only light measurement can show these;
* the link format (8-bit + FRC versus 10-bit): Windows reports the link bpc and KMS reports `max bpc`, but the
  monitor's own FRC/dithering is invisible.

**How to close the gap:**

* **Monitor OSD**: shows the current refresh rate on many monitors (sometimes in an information menu, sometimes as an
  FPS/Hz counter). It is sampled/averaged, so it is good for steady states (stable 47 → ~94 Hz under LFC) but won't
  show single-frame events.
* **High-speed camera** (≥ 960 fps slow motion, ideally 1000+ fps): enable the frame counter (`K`). Count scanouts
  per counter value: a value appearing in two consecutive scanouts means a repeat (LFC). Brightness modulation is
  also visible.
* **Photodiode + oscilloscope** (or tools such as OSRTT): objective measurement of brightness steps at refresh changes.
  This is the strongest evidence for an RMA claim.
