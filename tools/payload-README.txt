GBFlash bootloader -- what to do with this folder
=================================================

Some GBFlash cartridge flashers ship with the bootloader region blank. On those
devices FlashGBX's Firmware Updater has nothing to talk to, so firmware updates
fail. This puts a bootloader back. Your firmware is left exactly as it is.


DOES YOUR DEVICE NEED IT?

Open a terminal IN THIS FOLDER and run one of these. It is read-only: no
jumper, a few seconds, nothing is written.

    Linux / macOS      python3 install.py --check
    Windows            py install.py --check

If firmware updates already work, you do not need this.


INSTALL

Close FlashGBX, plug in one GBFlash and nothing else, then:

    Linux / macOS      python3 install.py
    Windows            py install.py

It checks whether you need it, backs your device up, builds the install image
from the bootloader and your own firmware, walks you through the one jumper
step, writes it, and reads back what landed. Keep the backup file it leaves in
this folder -- it is the way back. Move it somewhere that is not your Downloads
folder.

Rehearse the whole thing against a pretend device, touching no hardware:

    python3 install.py --dry-run


BEFORE YOU WRITE ANYTHING

Installing rewrites all of the chip's CodeFlash. If it goes wrong there is one
way back: short the H1 pads on the PCB while plugging the device in, and flash
a good image over USB. So make sure first that you can open the case, reach the
H1 pads, and bridge them -- and that the device appears as USB 4348:55E0 when
you plug in with them shorted.

H1 is not the U22 button. No LEDs light in that mode; a dark board is correct.

If 4348:55E0 never appears, do not install this.


WHAT YOU NEED

  Python 3.8 or newer.

  pyserial:   Linux / macOS   python3 -m pip install -r requirements.txt
              Windows         py -m pip install -r requirements.txt

              If that says "externally-managed-environment", use your system
              package instead -- e.g. sudo apt install python3-serial.

  wchisp:     flashes the chip over USB for the one jumper step.
              install.py offers to download it for you. If you fetch it by
              hand on macOS, clear the download quarantine first:
              xattr -d com.apple.quarantine ./wchisp


THE OTHER FILES

  install.py                  the guided installer -- start here
  bootloader.bin              the bootloader itself; NOT flashable on its own
  requirements.txt            the one Python dependency
  build_composite.py          builds the flashable image from your backup
  backup-codeflash.py         reads your device's flash to a file
  check-bootloader-region.py  answers whether a bootloader is present
  SHA256SUMS                  checksums for everything above
  LICENSE                     MIT

install.py does all of it. The other three run standalone if you would rather
do the steps by hand.

Verify the downloads:

    Linux              sha256sum -c SHA256SUMS
    macOS              shasum -a 256 -c SHA256SUMS
    Windows            PowerShell, since Get-FileHash cannot read SHA256SUMS:

      Get-Content SHA256SUMS | ForEach-Object {
        $h, $f = $_ -split '\s+', 2
        $a = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
        if ($a -eq $h) { "OK   $f" } else { "FAIL $f" }
      }


NO WARRANTY

This writes flash on hardware you own. It is provided as-is, with no warranty
of any kind. The author is not liable for any damage, data loss or non-working
device resulting from its use. You install it at your own risk.

Full documentation, troubleshooting and recovery -- including docs/RECOVERY.md,
which install.py points at when something goes wrong:
@REPO_URL@
