# Device facts

Recorded: 2026-09-14
Recorded by: user, via SSH over USBNet (RNDIS/Ethernet Gadget)

**Network (USBNet):** Kindle = **`192.168.15.244`** (use this for ssh/scp/deploy). Mac side of the RNDIS link = `192.168.15.201/24` (manual IP) — not a device address.

**STATUS: complete for M1.** Block 2 (framework stop/start) is done. Remaining: "Hotfix installed" (non-blocking). Touch transform and digitizer range resolved in T05. See "Outstanding" at the end.

---

## Identity

- Model: base/entry Kindle (NOT Paperwhite-class hardware — see §2 correction)
- Firmware: **5.17.1.0.3**
- System software string: `007-juno_17010003_moonshine_rex-435186`
- Jailbreak: WinterBreak
- Hotfix installed: _(not recorded — non-blocking for now)_
- Launcher: KUAL (confirmed — `/mnt/us/documents/KUAL.kual` present; no KPM)
- Root filesystem: **mounted read-only** by default (`mntroot rw` needed to write outside `/mnt/us`). `/mnt/us` itself (27.3G, `fsp` filesystem) is writable without this — deployment target is under `/mnt/us/extensions`, so this should not block T13 packaging. Flag for T01/T13 anyway.

## `/proc/version`
```
Linux version 4.1.15-lab126 (builder@eink-builds) (gcc version 4.9.1 (GCC) ) #1 SMP PREEMPT Thu Nov 28 11:38:31 UTC 2024
```

## `uname -a`
```
Linux kindle 4.1.15-lab126 #1 SMP PREEMPT Thu Nov 28 11:38:31 UTC 2024 armv7l GNU/Linux
```

## `/proc/cpuinfo`
```
processor       : 0
model name      : ARMv7 Processor rev 1 (v7l)
BogoMIPS        : 3.00
Features        : half thumb fastmult vfp edsp neon vfpv3 tls vfpd32
CPU implementer : 0x41
CPU architecture: 7
CPU variant     : 0x4
CPU part        : 0xc09
CPU revision    : 1

Hardware        : Freescale i.MX6 SoloLite (Device Tree)
Revision        : 0000
Serial          : <redacted>
```

### ⚠️ CORRECTION TO DESIGN DOC AND M1 SPEC

- **Only one `processor` entry → single-core**, not the dual-core i.MX7D assumed throughout `sumiyomi-design-doc.md` §2.1 and `sumiyomi-M1-implementation-spec.md` §3.2.
- **CPU part `0xc09` = ARM Cortex-A9**, not Cortex-A7 or A53.
- SoC is **Freescale/NXP i.MX6 SoloLite**, a cheaper/older part than the i.MX7D the design doc assumed.
- NEON **is** present, but as **vfpv3**, not vfpv4.

**Action for T01:** `cmake/kindle-toolchain.cmake`'s `SUMI_ARCH_FLAGS` must be:
```cmake
set(SUMI_ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard")
```
not the `-mcpu=cortex-a53 -mfpu=neon-vfpv4` in the spec as written.

**Action for M2+ threading design:** §3.1's "two worker threads because two cores" rationale in the design doc no longer applies as written — this hardware has one core. Two threads may still be worth keeping for I/O-latency hiding (network + disk don't block compute), but revisit before assuming any real parallelism. Not a blocker for M1.

## Toolchain target (U1) — RESOLVED

- Hard-float confirmed: `/lib/ld-linux-armhf.so.3` exists (symlink to `ld-2.20.so`); no soft-float loader present.
- **Use `kindlehf` target**, with `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard` (not the a53/neon-vfpv4 in the spec's example).

## `free -m` (framework RUNNING)
```
             total       used       free     shared    buffers     cached
Mem:           490        470         20          0         63        117
-/+ buffers/cache:        289        200
Swap:          127          0        127
```
Effective free (buffers/cache reclaimable): **~200 MB** while framework is running.

## `free -m` (framework STOPPED)
```
             total       used       free     shared    buffers     cached
Mem:           490        305        185          0         66        117
-/+ buffers/cache:        121        369
Swap:          127          0        127
```

Compared to the "before" reading (`-/+ buffers/cache` free: 193 MB), stopping the framework freed **+176 MB** of effective memory (193 → 369 MB). Comfortably above the "100+ MB" the M1 spec hoped for — the memory budget in the design doc §10.2 has good headroom on this hardware.

### U11 (memory freed by stopping framework) — RESOLVED
**+176 MB effective free** (369 MB total available after stop, vs 193 MB with framework running). Confirmed via `-/+ buffers/cache` comparison, not raw `free`.

### Restore-order finding
```
[root@kindle us]# start lab126_gui
lab126_gui start/running
[root@kindle us]# start framework
start: Job is already running: framework
```
`framework` came back on its own after `start lab126_gui` ran, before it was explicitly started — suggesting `framework` has an Upstart `start on` dependency on `lab126_gui` rather than being fully independent. **Action for T02:** when writing the restore sequence in `power_kindle.cpp` / `run.sh`, start `lab126_gui` first and treat a subsequent "already running" response from `start framework` as success, not an error to fail on.

### Visual/responsiveness check — NOT independently verified at Phase 0
The screen's behavior during the stop window (blank vs. frozen vs. still responsive) and full post-restore usability were not manually observed during this test. **This is not a blocker** — T02's own acceptance criteria in the M1 spec require the identical test (stop framework, sleep, restore, confirm device usable with no reboot) to be performed as part of implementing `PowerGuard`, so it will be verified for real once there is actual code driving it. Flagged here only so it isn't assumed to have already been confirmed.

## fbink

- **Not on `PATH`.** Located at: `/mnt/us/libkh/bin/fbink` (world-executable, dated Mar 1 2025). All references in build scripts / `run.sh` must use this full path, or add it to `PATH` explicitly — do not assume `fbink` resolves.

### `fbink -v`
```
[FBInk] Detected a Kindle PaperWhite 4 (16Q -> 0x4D8 => Moonshine on Rex)
[FBInk] Enabled Kindle Rex platform quirks
[FBInk] Clock tick frequency appears to be 100 Hz
[FBInk] Screen density set to 300 dpi
[FBInk] Variable fb info: 1072x1448, 8bpp @ rotation: 3 (Counter Clockwise, 270°)
[FBInk] Fixed fb info: ID is "mxc_epdc_fb", length of fb mem: 6782976 bytes & line length: 1088 bytes
[FBInk] Framebuffer pixel format: Y8
[FBInk] Pen colors set to #000000 for the foreground and #FFFFFF for the background

FBInk 84bfe3b for Kindle [Draw=Yes, Bitmap=Yes, Fonts=Yes, Unifont=No, OpenType=Yes, Image=Yes, Input=Yes, ButtonScan=No]
```

Note the device identity string: FBInk classifies this as **"Moonshine on Rex"** — an entry-level Kindle, not literal Paperwhite 4 hardware — but with a panel that matches PW4's specs closely enough that FBInk buckets it in that family for quirk purposes. This is consistent with the i.MX6 SoloLite / single-core Cortex-A9 finding above (real PW4 hardware is i.MX7D, dual-core) — this is cheaper, newer, and differently-specced hardware wearing a similar panel.

### `fbink -e` (full state dump)
```
FBINK_VERSION='84bfe3b'; FBINK_TARGET=2 (KINDLE); FBINK_FEATURES=0xb7
viewWidth=1072; viewHeight=1448; screenWidth=1072; screenHeight=1448
viewHoriOrigin=0; viewVertOrigin=4; viewVertOffset=4
DPI=300; BPP=8; lineLength=1088
invertedGrayscale=0
FONTW=24; FONTH=24; FONTSIZE_MULT=3; glyphWidth=8; glyphHeight=8
MAXCOLS=44; MAXROWS=60; isPerfectFit=0
FBID='mxc_epdc_fb'; USER_HZ=100
penFGColor=0; penBGColor=255
deviceName='PaperWhite 4'; deviceId=1240; deviceCodename='Moonshine'; devicePlatform='Rex'
isMTK=0; isSunxi=0
isKindleLegacy=0; isKoboNonMT=0
unreliableWaitFor=0; canWakeEPDC=0
ntxBootRota=0; ntxRotaQuirk=0; rotationMap='{ 0, 0, 0, 0 }'
touchSwapAxes=0; touchMirrorX=0; touchMirrorY=0; isNTX16bLandscape=0
currentRota=3; canRotate=0
canHWInvert=1; hasEclipseWfm=0; hasColorPanel=0
pixelFormat='Y8'
canWaitForSubmission=1
```

### U2 (grayscale polarity) — RESOLVED
`invertedGrayscale=0`, `penFGColor=0` (black), `penBGColor=255` (white). **Standard polarity: 0=black, 255=white, matching the Canvas layer's assumption in the M1 spec exactly.** The polarity-normalization branch described in §5.4 of the M1 spec (`display_fbink.cpp` implementation requirements, point 8) is **not needed on this device** — but keep the flag in the code path anyway, since other Kindle models do report inverted. Just don't spend time debugging it here if it's never triggering.

### U6 (touch digitizer range vs panel range) — RESOLVED (T05): X 0..1072, Y 0..1448, 1:1 with panel; no swap/mirror
`touchSwapAxes=0`, `touchMirrorX=0`, `touchMirrorY=0` — **all false**. The transform in M1 spec §6.2 degenerates to a straight passthrough (no swap, no mirror) on this device; only the min/max scaling term remains relevant, and only if the digitizer's `ABS_MT_POSITION_X/Y` range differs from 1072×1448. Still confirm the actual `EVIOCGABS` min/max in T05 — don't assume 1:1 without checking, but the swap/mirror complexity the spec worried about does not apply here.

### U7 (unreliable_wait_for) — RESOLVED
`unreliableWaitFor=0`, `canWaitForSubmission=1`. `fbink_wait_for_complete`/`fbink_wait_for_submission` can be used normally; no special-casing needed for T03.

### Bonus finding: BPP already 8
`BPP=8` at the FBInk level (matches the sysfs reading). **T03 can skip the `fbink_set_fb_info` bpp-switch call entirely** — the device is already in the correct mode. Fewer state transitions on startup, less chance of stranding the fb.

### Bonus finding: native rotation is non-zero
`currentRota=3` (270° CCW), `canRotate=0` (no gyro — this rotation is fixed, not dynamic). `rotationMap='{ 0, 0, 0, 0 }'` and `isNTX16bLandscape=0` suggest FBInk is not applying additional rotation compensation beyond what's already reported. Since `viewWidth`/`viewHeight` and `screenWidth`/`screenHeight` already match (1072×1448 both ways), FBInk appears to be reporting already-corrected logical dimensions — T03 should draw against `screenWidth`/`screenHeight` as given and not attempt to apply rotation math on top; that appears to already be handled beneath this layer. Flag for T03 to confirm empirically (draw an asymmetric test pattern, e.g. a rectangle only in the top-left, and confirm it appears top-left on the physical device) rather than trusting this inference alone.

## `/dev/fb0` sysfs (raw, NOT to be trusted over FBInk's own state — see note)
```
virtual_size: 1088,6144
bits_per_pixel: 8
rotate: 3
```
`virtual_size` of 1088×6144 is almost certainly a multi-buffer/virtual allocation, not the visible panel resolution — this is exactly the situation `sumiyomi-M1-implementation-spec.md` §5.1 warns about, which is why `fbink -e`'s `screen_width`/`screen_height` (from `FBInkState`) is the value the code must use, never raw sysfs. `bits_per_pixel: 8` suggests the device may already be in 8bpp mode — worth confirming via `fbink -e`'s `bpp` field, which would let T03 skip the `fbink_set_fb_info` bpp-switch call entirely. `rotate: 3` (non-zero native rotation) *may* mean the touch coordinate transform in §6.2 of the M1 spec is not a pure passthrough here, which conflicts with the `touchSwapAxes/MirrorX/MirrorY=0` reading in the U6 section above.

**RESOLVED in T05 (corner-tap test):** no rotation, swap or mirror is needed for touch. FBInk's touch flags (all 0) are correct despite `rotate: 3`.

## Input devices

```
/dev/input/event0 -> 20cc000.snvs:snvs-powerkey   (power button)
/dev/input/event1 -> bd71827-power                (power button, PMIC-side)
/dev/input/event2 -> goodix-ts                     (touchscreen)
```

No dedicated page-turn buttons device — consistent with this being a base Kindle rather than an Oasis. Reader page-turn-button support (design doc §8.4) is not applicable to this specific device; leave the code path in for other devices, but it won't be testable on this hardware.

### MT protocol (U5) — RESOLVED, no Block 3 needed

`goodix-ts` device entry:
```
B: EV=b
B: KEY=0
B: ABS=6e58000 0
```

Decoding the `ABS` bitmask: bit 47 (`ABS_MT_SLOT`, code `0x2f`) is set. **This is Protocol B.** The M1 spec's Block 3 (hexdump tap test) is not needed — implement the Protocol B path in T05 directly (`ABS_MT_SLOT` + `ABS_MT_TRACKING_ID`, not the Protocol A `SYN_MT_REPORT` path).

Touch digitizer range (`ABS_MT_POSITION_X/Y` min/max, needed for U6's scaling) is still outstanding — that's in-scope for T05 itself per the spec, not Phase 0.

## Framework control (U3) — SUPERSEDED: Sumiyomi coexists with the framework (see M1 spec §8)

The stop/start record below is kept as Phase 0 history; no code uses it.

> ⚠️ The stop/restore sequence below brings the jobs back to `start/running`, but the UI does not fully recover: after sleep/wake the screen goes blank on its own, and only a reboot fixes it. See `docs/M1-notes.md` → "T02 device run #1". Treat this sequence as unverified until an isolated post-reboot test passes a hands-on usability check.

`/etc/init.d/framework` does **not** exist. This firmware uses **Upstart**, with two separate jobs:

```
lab126_gui_setup stop/waiting
framework_setup stop/waiting
framework start/running, process 6881
lab126_gui start/running
lab126_gui_monitor stop/waiting
```

**Stop command:**
```sh
stop framework
stop lab126_gui
```

**Restore command** (order matters — `lab126_gui` first; see "Restore-order finding" above, confirmed by hand):
```sh
start lab126_gui
start framework   # usually prints "Job is already running: framework" — that is success
```

**Action for T02 / `run.sh`:** replace every `/etc/init.d/framework stop|start` reference in `sumiyomi-M1-implementation-spec.md` (§3.2, §8) with the two-job Upstart pair above. The `run.sh` template's `restore()` and the pre-launch block both need this substitution.

## lipc (U4) — RESOLVED

Present and working:
```
/usr/bin/lipc-get-prop
/usr/bin/lipc-set-prop
```
`lipc-get-prop com.lab126.powerd status` succeeded, showing `prevent_screen_saver:0` currently — confirms both the binary and the property namespace work as the spec expects. No fallback path needed.

## Launchers (U12) — RESOLVED

- `/mnt/us/extensions` exists.
- `/mnt/us/documents/KUAL.kual` exists → **KUAL confirmed.**
- No `/mnt/us/kpm*`, no `*.kpm` files → not KPM.

**Action for T13:** use the `menu.json` KUAL packaging path in the M1 spec as written; no KPM adaptation needed.

## Tools on device

- `strace`: **not installed** (`command -v strace` empty, checked before T10). T10's idle check uses `/proc/<pid>/status` context-switch counters instead.
- `fbink`: at `/mnt/us/libkh/bin/fbink` (see above).

## Storage

```
Filesystem                Size      Used Available Use% Mounted on
fsp                      27.3G      2.2G     25.1G   8% /mnt/us
```
Ample room for the app, extensions, and cache budgets in the design doc.

---

## Outstanding

- **Hotfix installed (y/n)** — unrecorded, non-blocking for now.
- ~~Touch transform~~ — resolved in T05: passthrough.
- ~~Touch digitizer range~~ — resolved in T05: X 0..1072, Y 0..1448, 1:1 with the 1072×1448 panel.

Everything else needed by `sumiyomi-M1-implementation-spec.md` §13's 🔴 list is resolved above.
