PINE64 PinePhone
################

-  https://wiki.pine64.org/wiki/PinePhone

The original PinePhone, with the Allwinner A64. The PinePhone Pro is a
different SoC (Rockchip RK3399S) and is not covered here.

Hardware Information
====================

-  ARMv8 Architecture (64-bit)
-  Allwinner A64 (SoC): 4x Cortex-A53, GIC-400, Mali-400 GPU
-  2 or 3 GB RAM, 16 or 32 GB eMMC, microSD slot
-  720x1440 panel (Xingbangda XBD599, Sitronix ST7703) on 4-lane MIPI-DSI
-  Goodix GT917S touchscreen on I2C0
-  X-Powers AXP803 PMIC on RSB
-  Realtek RTL8723CS Wi-Fi/Bluetooth (SDIO), Quectel EG25-G modem (USB)

Booting
=======

The phone boots through Tow-Boot (U-Boot with TF-A and PSCI) installed in
the eMMC boot partition. Tow-Boot loads ``EFI/BOOT/BOOTAA64.EFI`` from the
ESP of the microSD card (hold volume down while it starts to force the card)
and passes its own device tree, ``sun50i-a64-pinephone-1.2``.

Tow-Boot leaves the display off and provides no GOP. The EFI loader brings
the panel up itself (``efi/arch/arm64/pinephone_display.cpp``): AXP803 rails
over RSB, PWM backlight, TCON0, MIPI DSI and D-PHY, the ST7703 init sequence,
then mixer 0 of the Display Engine scanning out a 720x1440 XRGB8888 frame
buffer in EFI-reserved memory. The kernel gets it like a GOP frame buffer:
the boot splash and app_server's framebuffer driver use it.

The serial console is UART0 (115200 8N1) on the headphone jack, with privacy
switch 6 off. Without the cable the RGB LED shows how far the loader got:

========================  ====================================================
LED                       Meaning
========================  ====================================================
3 white blinks            the loader found a PinePhone, starts the panel
red                       backlight and TCON0 set up
yellow                    AXP803 answered, panel rails on
magenta                   DSI controller and D-PHY on
blue                      panel took its init commands
cyan                      DSI video mode and Display Engine on
green                     frame buffer scanned out
white                     the loader starts the kernel
========================  ====================================================

A colour blinking quickly for three seconds means that step failed. Steady
green without white means the loader is stuck, usually at its boot menu
(no boot volume), which only shows on the EFI text console.

Setting up the SD card
======================

``build/scripts/pinephone/build-sd-image.sh <bfs image> <output>`` makes a
GPT image with a FAT16 ESP holding the loader and the BFS partition (needs
sfdisk and mtools, no root)::

	jam -q @minimum-raw
	build/scripts/pinephone/build-sd-image.sh haiku-minimum.image pinephone-sd.img

``build/scripts/pinephone/qemu-smoke.sh`` boots the same layout on QEMU's
virt machine with a Cortex-A53, a GICv2 and EDK2 handing over a device tree,
which is as close to the phone as QEMU gets; it exercises none of the A64's
devices.

The phone is usually switched off without unmounting. To read its syslog, copy
the card's BFS partition on a Linux PC (``pkexec dd if=/dev/sdX2
of=card.img bs=4M``) and run ``build/scripts/pinephone/read-syslog.sh
card.img``, which replays the BFS journal on a private copy.

Drivers
=======

-  Generic timer: the A64's counter reads can jump while bit 10 or above rolls
   over (erratum UNKNOWN1); ``arm64_read_counter()`` filters them.
-  SD/eMMC: ``sunxi_mmc`` (``allwinner,sun50i-a64-mmc``, ``-emmc``). The
   RTL8723CS SDIO slot (the one with an ``mmc-pwrseq``) is left alone.
-  Touchscreen: ``goodix_touch``, published as a tablet. U-Boot's device tree
   names the I2C controller only ``allwinner,sun6i-a31-i2c``.

Not supported yet: power and volume keys, battery and charging, USB, Wi-Fi,
audio, modem, cameras, display brightness and blanking, the GPU.
