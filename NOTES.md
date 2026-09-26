# Miyoo Mini (SSD202D) Linux 6.5 - bring-up notes

## Display / timing findings (2026-07-28)

- **QEMU generic-timer ran ~10x too fast (fixed).** The vendor DT pins the
  arm,armv7-timer at 6 MHz (`mstar-v7.dtsi`), but QEMU defaulted the cortex-a7
  generic timer to the 62.5 MHz back-compat rate, so guest time (and the DRM
  60 Hz vblank waits) ran ~10x fast and `drm_atomic_helper_wait_for_vblanks()`
  timed out on modesets. Fixed in the QEMU model (`hw/arm/mstarv7.c`: pin CPU
  `cntfrq` to 6 MHz). Verified with a `DRM_IOCTL_WAIT_VBLANK` probe: was
  ~5 Hz guest-perceived, now ~52 Hz.

- **Panel is mounted upside down; nothing in the pipe compensates.** The GOP
  scan-out can't flip in hardware on SSD202D (`mstar_gop.c`: flip bits not
  writable) and the kernel doesn't flip fbcon either. The DT `panel@0
  rotation = <180>` only sets the informational DRM "panel orientation"
  property. So userspace (the compositor) must compensate. DirectFB2's drmkms
  module now does (see its rotationfixes branch / the br2directfb2 patch): it
  flips the scan-out 180 for a "Upside Down" panel, covering full-screen SDL
  primaries too. fbcon, a plain DirectFB primary and chocolate-doom all come
  out upright and consistent.

- **CMA bumped to 16 MiB.** The shared 4 MiB `linux,cma` pool was too small
  for a double-buffered 640x480 primary + fbcon + DirectFB's rotation scan-out
  buffer (allocations failed with -ENOMEM). Overridden in the board DT.

## TODO / known issues

- **BACH audio: duplicate debugfs directory warning.** The `msc313-bach`
  driver prints on probe:

      debugfs: Directory '1f2a0400.bach' with parent 'msc313-bach' already present!

  (twice). Harmless - the card still registers as ASoC card #0 - but the
  driver is creating its debugfs dir more than once. Fix later:
  `sound/soc/mstar/msc313-bach.c`, probe path around the debugfs setup.

- **Backlight dimming.** Only the top (max) PWM level currently lights the
  panel; the LED-boost driver seems to want a different PWM frequency/duty
  response. Parked with `default-brightness-level = <7>` (max) in
  `arch/arm/boot/dts/sigmastar/mstar-infinity2m-ssd202d-miyoo-mini.dts` so the
  screen is visible. Needs proper tuning of the PWM period / brightness curve.

## SD card-detect IRQ / early-boot hardware lockup (2026-07-29)

- **Symptom (hardware only):** around FCIE/SDIO init the boot log shows
  `gpio gpiochip1: (gpio-msc313-pm): failed to allocate parent hwirq 73 for
  hwirq 1` right after `msc313-fcie 1f282000.sdio: Got CD GPIO`, and the
  machine sometimes hard-locks shortly after. The `msc313-fcie ... err during
  job; status: 0008 ... cmdrspsz: 0505` lines are benign: status 0x0008 is
  `SD_STS_NORSP` for the CMD1 (SEND_OP_COND) probes the core always sends;
  the FCIE driver's waits are all bounded (`wait_event_timeout` /
  `regmap_*_poll_timeout` in `drivers/mmc/host/mstar-fcie.c`,
  `mstar_fcie_start_transfer_and_wait()`), so FCIE is not the lockup.

- **Why hwirq 73 fails:** `drivers/gpio/gpio-msc313-pm.c`
  `msc313e_pm_gpio_child_to_parent_hwirq()` computes the parent line as
  `(register offset >> 2) + 2`; for the SD_CDZ pad (offset 0x11c) that is 73.
  But the parent domain (`pmintc`, `mstar,msc313-pm-intc`,
  `drivers/irqchip/irq-msc313-pm-intc.c`) only has `NUM_IRQ 32` lines
  (PMSLEEP INTSTATUS is two 16-bit regs at pmsleep+0x10/+0x14) *and* is a
  plain linear domain with only `.map`/`.xlate` - no `.alloc` - so the
  hierarchical alloc from gpiolib (`gpiochip_hierarchy_irq_domain_alloc()`,
  `drivers/gpio/gpiolib.c` ~line 1242) always returns -ENOSYS. Every PM GPIO
  interrupt is unusable, not just SD_CDZ (the driver's top comment even says
  "SD interrupt is broken"). The failure itself is soft: `gpiod_to_irq()`
  returns -ENXIO and the mmc core falls back to `MMC_CAP_NEEDS_POLL`, so the
  card still enumerates (rootfs works).

- **Lockup mechanism (best supported by source):** both
  `irq-msc313-pm-intc.c` and `irq-msc313-pm-wakeup.c` request the *shared,
  level-triggered* GIC SPI 2 line and their handlers returned `IRQ_HANDLED`
  unconditionally. The PM sleep intc irqchip has no-op mask/unmask/eoi and
  nothing ever clears `MSTAR_PMSLEEP_INTSTATUS` (sources can only be acked at
  the source, e.g. the PM GPIO `BIT_IRQ_CLEAR`, which is unreachable because
  the mapping above never gets created). So any stray PMSLEEP status bit
  (CDZ pad glitch when the CD GPIO gets re-muxed / card VDD ramps - exactly
  where the log sits - or IR-pad noise) leaves the level line asserted
  forever while the handlers keep claiming it: an unbreakable IRQ storm the
  spurious-irq detector can never see -> intermittent hard lockup.

- **Fixes applied (branch internfixes-2026-07-29):**
  1. Both PM intc chained handlers now only return `IRQ_HANDLED` when
     `generic_handle_domain_irq()` actually dispatched something; otherwise
     `IRQ_NONE`, so a storm gets contained by genirq's "nobody cared"
     (line is masked and the machine keeps running). Also dropped the
     unconditional `printk` in the wakeup handler.
  2. `gpio-msc313-pm.c` refuses `child_to_parent_hwirq` translations beyond
     the parent's 32 lines instead of asking for bogus line 73.
  3. `mstar-infinity2m-ssd20xd.dtsi`: `&sdio` now has `broken-cd;` next to
     `cd-gpios` - the mmc core then polls the CD GPIO (1s interval) and never
     attempts the doomed CD interrupt request. Tradeoff: card
     insert/removal is noticed up to ~1s late; the Miyoo's card holds the
     rootfs and stays inserted, so no practical impact.

- **To confirm on hardware:** enable `CONFIG_DETECT_HUNG_TASK` +
  magic-sysrq; if the old lockup was the storm, the patched kernel should
  instead log `irq N: nobody cared (try booting with the "irqpoll" option)`
  plus a disabled-irq backtrace naming `pmsleep`. A JTAG halt during a
  lockup on an unpatched kernel should show the CPU in the GIC/`handle_irq`
  path with PMSLEEP INTSTATUS non-zero (read pmsleep+0x10/+0x14).
- **Left alone for now:** `msc313_pm_gpio_irq_set_type()` clobbers the whole
  pad register (`reg &= BIT_IRQ_TYPE` wipes OEN/OUT/MASK) - currently dead
  code because no PM GPIO irq can be allocated, but fix before ever making
  the PM GPIO interrupts real (that also needs pmintc converted to a
  hierarchical domain with a real `.alloc`, and the true PMSLEEP line for
  SD_CDZ discovered from vendor code).
