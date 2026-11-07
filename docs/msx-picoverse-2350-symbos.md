# Running SymbOS on MSX PicoVerse 2350

This guide explains how to install SymbOS with JZZY's patched Sunrise IDE driver and add networking with the PicoVerse-specific SymbOS Network Daemon.

Status checked on **October 17, 2026**:

| PicoVerse firmware | SymbOS disk access | SymbOS networking |
| --- | --- | --- |
| LoadROM, Sunrise IDE mode | Working with the patched disk driver | Working with a WiFi-enabled image and the PicoVerse network daemon |
| Explorer v2.58 and earlier, Sunrise Nextor SYSTEM entry | Working with the patched disk driver | Adapter detection fails even when WiFi is enabled for the entry |
| Explorer v2.59, Sunrise Nextor SYSTEM entry | Working with the patched disk driver | Adapter-detection mismatch fixed; **end-to-end networking on hardware is not yet validated** |

**LoadROM remains the hardware-confirmed choice for both disk access and networking in SymbOS.** Explorer v2.59 restores the memio status bits required by the daemon's adapter probe; users testing this version should enable Wifi on the Nextor SYSTEM entry and follow the same daemon setup below. Explorer's File Hunter working by itself does not validate the SymbOS path.

The Explorer failure was a firmware-interface mismatch: the daemon requires `(status & 0x88) == 0x88` at `0x7F07`, but Explorer returned neither bit 7 nor bit 3. It therefore rejected the adapter before sending a network-state query. Version 2.59 restores those bits, matching LoadROM; host tests verify detection and status behavior, not real WiFi traffic.

The reported network setup used **SymbOS 4.0 on an MSX2**, with `loadrom_nextor_1mb_sd_wifi_v2.78.uf2`. This is a known-working reference, not a claim that v2.78 is a minimum required version or that every other firmware/profile has been tested.

## 1. What you need

- An MSX supported by SymbOS. An MSX2 or later is the straightforward starting point; consult the [official SymbOS downloads and manual](https://www.symbos.org/download.htm) for machine-specific requirements.
- A PicoVerse 2350 and a microSD card or USB mass-storage device.
- A Nextor-bootable storage setup. Use an MBR-partitioned device with a first primary **FAT16 partition**, and start with a simple single-partition layout. The PicoVerse Nextor documentation allows FAT16 partitions up to 4 GB; that is a Nextor limit, not an independently verified SymbOS partition-size recommendation.
- Mapper RAM. SymbOS requires at least 128 KB, but more RAM is strongly recommended. The LoadROM `-m1`/`-m2` modes and corresponding Explorer `+ 1MB Mapper` entries provide PicoVerse mapper RAM.
- The **MSX package of SymbOS 4.0** from the official download page.
- The patched disk driver and, for networking, the network daemon listed below.
- For networking: an installed ESP-01/ESP8266 module with [Oduvaldo Pavan Jr.'s compatible ESP8266 UNAPI firmware](https://github.com/ducasp/ESP8266-UNAPI-Firmware), a configured WiFi connection, and DHCP available on that network.

Back up your storage device before partitioning it or modifying an existing SymbOS installation. FAT32/exFAT volumes usable by Explorer are not substitutes for a Nextor boot volume. See the [microSD guide](./msx-picoverse-2350-microsd.md) and [Sunrise Nextor guide](./msx-picoverse-2350-sunrise-nextor.md) for storage preparation.

## 2. Download the right files

Download individual binaries using GitHub's **Raw / Download raw file** action, or extract the repositories' ZIP downloads. Do not save a GitHub HTML file-view page as a driver.

### Disk driver

Repository: [Jzzy-dev/msx-symbos-sunrise-ide-driver-picoverse](https://github.com/Jzzy-dev/msx-symbos-sunrise-ide-driver-picoverse).

| File | Purpose | Needed on the MSX? |
| --- | --- | --- |
| [`-IDEsdPV.DRV`](https://github.com/Jzzy-dev/msx-symbos-sunrise-ide-driver-picoverse/blob/main/-IDEsdPV.DRV) | Ready-to-use SymbOS Sunrise IDE driver patched for PicoVerse | **Yes**; place it in the SymbOS installation directory |
| `Drv-IDE-Sunrise-SD-PicoVerse.asm` | Assembly source for the patched driver | No; development only |
| `SymbOS-File-Const.asm` | Storage constants used when assembling the driver | No; development only |

Keep the leading hyphen in **`-IDEsdPV.DRV`**. Despite the filename, JZZY's installation instructions cover PicoVerse storage through **SD or USB**.

The driver is based on [Prodatron's SymbOS MSX mass-storage drivers](https://github.com/Prodatron/symdrv-msx-massstorage). In its initialization routine, the patched source omits issuing the original `ide_c_lbadat` command. The change is in the **SymbOS driver**, not a replacement Nextor ROM or an extra DOS resident program. Successful disk access in Nextor alone does not prove that SymbOS has the correct driver installed.

### Network daemon

Repository: [Jzzy-dev/msx-picoverse-symbos-network-daemon](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon).

| File | Purpose | Needed on the MSX? |
| --- | --- | --- |
| [`netd-esp.exe`](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon/blob/main/netd-esp.exe) | Compiled **SymbOS application** that provides networking through PicoVerse | **Yes**, for networking; copy it into the SymbOS directory |
| `README-ESP-driver.md` | Driver status, capabilities, limitations, and build notes | Reference only |
| `Dmn-Network-ESP.asm` | PicoVerse memio transport and ESP hardware/protocol layer | No; development only |
| `Dmn-Network.asm` | Network daemon with the `DRIVER 6` integration | No; development only |
| `Dmn-Network-DRIVER6.diff` | Patch describing the daemon changes | No; development only; do not apply it to PicoVerse firmware |
| `Dmn-Network-libs.asm`, `Dmn-Network-i18n-direct.asm`, `SymbOS-Constants.asm` | Assembly support code, text, and constants | No; development only |

**`netd-esp.exe` is not a Windows program and is not run from MSX-DOS. Run it inside SymbOS.** Use this PicoVerse-specific build rather than assuming that a generic UNAPI daemon also supports PicoVerse's ROM/direct memio interface.

No assembler is needed for normal installation. The upstream build notes also refer to a wrapper named `Dmn-Network-_ESP.asm` and `Dmn-Network-head.asm`; these were not present in the repository's main-branch file list when checked. Developers rebuilding the daemon must supply the matching SymbOS build context. Likewise, assembling the disk source in isolation without its constants and relocation/build setup is not the installation procedure. The forum's unresolved-label errors such as `stodatsub` and `stoerrdvp` are build-context issues; use the supplied `.DRV` binary.

## 3. Boot PicoVerse into Nextor

SymbOS is installed from DOS **after Nextor Sunrise IDE has booted**. It then uses its own patched storage driver.

### Recommended: LoadROM with mapper RAM and WiFi

From this repository's root directory on a Windows PC, choose the command matching your storage:

```powershell
# microSD: Nextor 2.1.4 + 1MB mapper + WiFi
.\2350\software\loadrom.pio\tool\dist\loadrom.exe -m1 -w -o symbos_sd_wifi.uf2
```

```powershell
# USB mass storage: Nextor 2.1.4 + 1MB mapper + WiFi
.\2350\software\loadrom.pio\tool\dist\loadrom.exe -m2 -w -o symbos_usb_wifi.uf2
```

These commands select the appropriate firmware features; the forum's explicitly reported network test used the **microSD** variant. USB is an available Sunrise storage configuration, not a separate USB-network test result.

1. Flash the chosen UF2 using the normal PicoVerse BOOTSEL procedure. Flashing LoadROM replaces Explorer in cartridge flash; it does not install SymbOS on the storage device.
2. With the MSX powered off, install the ESP-01 if networking is required, and connect the storage device matching the image.
3. Power on the MSX and boot to the Nextor DOS prompt.
4. Check that DOS can list and read the intended installation drive.

For disk-only use, omit `-w`. The `-s1`/`-s2` modes are alternatives if the MSX already has sufficient mapper RAM. Do not substitute MegaRAM alone for a standard memory mapper, or assume that Carnivore2/MegaRAM modes are the WiFi configuration described here.

Use the regular **Nextor 2.1.4** modes for the documented starting point. The Nextor 3.0.0 entries are not the setup demonstrated in the linked SymbOS discussion. See the [LoadROM manual](./msx-picoverse-2350-loadrom-tool-manual.en-us.md) for other options and flashing details.

### Explorer: disk access and v2.59 network testing

For SymbOS disk access, you can keep Explorer and launch a Sunrise Nextor SYSTEM entry, preferably **Nextor Sunrise + 1MB Mapper (SD)** or its USB counterpart. Use **v2.59 or later** when testing the network daemon; earlier Explorer builds fail its adapter probe.

For example, to package the microSD mapper entry from the repository root:

```powershell
.\2350\software\explorer.pio\tool\dist\explorer.exe -m1 -o explorer_symbos_sd.uf2
```

The Explorer tool also includes supported ROMs from its source folder; the Nextor entry is the one to select for this workflow. If your existing Explorer image already contains it, there is no need to regenerate the image.

For the simplest disk-only setup, leave the entry's **Wifi** option set to **No**. On current Explorer versions offering **SD Part: WHOLE DISK**, use that setting for an MBR-partitioned SymbOS disk: it exposes the partition table at sector zero, which the patched SymbOS driver reads. Selecting a single filesystem volume instead changes the sector-zero view and is not equivalent to the raw-disk LoadROM setup.

Launch the SYSTEM entry, boot Nextor DOS, and continue with the same SymbOS disk-driver installation below. The Explorer `F2` file browser and its `P` partition selection are not the SymbOS boot path. See the [Explorer manual](./msx-picoverse-2350-explorer-tool-manual.en-us.md) for packaging and menu controls.

For network testing on Explorer v2.59, set the entry's **Wifi** option to **Yes**, then configure the disk driver's WiFi-enabled IDE slot as explained below. The adapter-detection fix is host-tested, but successful DHCP, DNS, TCP, and UDP operation still needs to be confirmed on hardware. Use LoadROM if you need the already hardware-confirmed setup.

## 4. Install SymbOS and select the patched driver

The following example uses **`A:\SYMBOS`**. Substitute the actual DOS drive letter and installation path if yours differ.

1. Extract the official `symbos-msx40.zip` package on your PC, then extract its nested **`SymbOS-MSX40-Setup.ZIP`**. The setup archive contains **`SYMSETUP.COM` and `SYMSETUP.DAT`**; keep both together on the MSX-accessible storage device.
2. Create or choose the intended installation directory, for example `A:\SYMBOS`, and copy **`-IDEsdPV.DRV`** into it **before configuring the device driver**. If you keep the setup files elsewhere, the patched driver still needs to be available in the SymbOS directory used by setup.
3. At the Nextor DOS prompt, change to the directory containing the setup files and run:

   ```text
   SYMSETUP
   ```

4. Choose a new installation and select `A:\SYMBOS` as the destination, or use setup's driver-settings/expert mode for an existing installation. Follow the official manual for the other machine, display, and input settings.
5. In the storage/device-driver configuration, explicitly choose the patched file and the correct IDE slot:

   | Setting | Starting value |
   | --- | --- |
   | Driver filename | **`-IDEsdPV.DRV`** |
   | Primary slot / subslot | The slot containing **Nextor + Sunrise IDE**; see below |
   | Device/channel | Master / channel `0`, if requested |
   | Partition | **`1`** for the first primary partition used in this guide |
   | Removable | **`N`**, as specified by JZZY |

   The official SymbOS release notes warn that Sunrise IDE is not autodetected when Nextor is installed. Do not rely on automatic driver detection.

6. Save the configuration and complete the installation.
7. From the installed SymbOS directory, run **`SYM`** (`SYM.COM`) to start the normal MSX version.
8. Before adding networking, confirm that SymbOS can browse the installed disk and open files. Make a small test file, save it, and reopen it to verify writes as well as reads.

The official **Mass Storage Completion** package is optional and adds applications and data after the platform-specific installation. It does not replace `SYMSETUP` or the PicoVerse disk-driver configuration.

### Selecting the correct slot

The forum example uses **slot `1-1`**: primary slot 1, subslot 1. That matches the Nextor IDE interface of a WiFi-enabled LoadROM mapper image when PicoVerse is in primary slot 1. **It is not a universal value.**

For the standard modes in this guide, with PicoVerse in primary slot `P`:

| Active mode | Nextor + Sunrise IDE | WiFi interface |
| --- | --- | --- |
| `-m1` / `-m2`, WiFi off | `P-0` | None |
| `-m1 -w` / `-m2 -w` | `P-1` | `P-0` |
| `-s1` / `-s2`, WiFi off | Primary slot `P`, no PicoVerse subslot | None |
| `-s1 -w` / `-s2 -w` | `P-1` | `P-0` |

Explorer's equivalent plain/mapper profiles follow the same basic layout according to their Wifi setting. Use the actual machine slot arrangement if an external expander is involved.

The **disk driver** must target the IDE interface, not the WiFi ROM or mapper RAM. The network daemon scans for its WiFi interface separately. If you change physical slots, switch between WiFi and non-WiFi profiles, or move an installation between LoadROM and Explorer profiles, revisit the disk slot setting in `SYMSETUP`. See the [slot organization guide](./msx-picoverse-2350-slot-organization.md).

## 5. Connect SymbOS to the network

First confirm that SymbOS disk access works. Then:

1. Boot a **LoadROM WiFi-enabled** Sunrise image, such as `-m1 -w`, for the hardware-confirmed setup. Alternatively, test an **Explorer v2.59** Sunrise SYSTEM entry with **Wifi: Yes**, bearing in mind the validation status above.
2. Make sure the ESP-01 has compatible firmware and has been configured to join your access point. The daemon uses the ESP's existing WiFi connection; copying the executable does not supply an SSID or password. Use the module's WiFi configuration workflow before starting SymbOS. If you are already using Explorer, its **F4 WiFi setup** can be used to configure the connection before launching the chosen Nextor profile. Refer to the [ESP-01 programming instructions](./msx-picoverse-2350-build-instructions.md#programming-the-esp-01-module) and [PicoVerse WiFi guide](./msx-picoverse-2350-wifi.md).
3. Copy **`netd-esp.exe`** to `A:\SYMBOS` on your storage device.
4. Boot Nextor, start SymbOS, and launch **`netd-esp.exe` from inside SymbOS**.
5. Let the daemon detect the WiFi interface. It first tries its configured slot, then scans the slots automatically. Do not force the disk driver's `1-1` example onto the network interface.
6. Check the daemon's Driver/status displays. The documented v0.4 driver identifies itself as **`MSX PcVrs ESP v0.4`**. It should obtain IP settings from the ESP through DHCP and show an online link. A wrong saved slot can make the initial scan slow.
7. Leave the daemon running while using network applications. Start it again after rebooting SymbOS unless you have separately configured application startup.

The forum procedure does **not** require launching an additional DOS network daemon first. The PicoVerse WiFi ROM/transport and ESP firmware must already be present, but the SymbOS networking service is started by the SymbOS executable.

The v0.4 upstream notes report successful **DNS and TCP**, including `wget` and **SymTel**, and **UDP**, tested with **Net Test / World clock / SNTP**. Test a simple connection or time query before introducing a web-browser proxy.

### Current daemon limits

- The ESP has **four shared TCP/UDP connections**, not four of each.
- The documented UDP payload limit is **576 bytes**; larger sends are rejected and larger receives are truncated.
- The displayed access-point name is not a measured signal-strength indicator; RSSI display is not implemented in the documented build.
- The v0.4 notes list TLS, language packs, server mode, and resetting all connections as untested or unfinished.
- On October 17, JZZY announced an upcoming **v0.5 with HTTPS support**. The checked repository documentation still describes v0.4; do not assume the supplied executable includes that announced update. Recheck upstream before following instructions specific to a newer release.

## 6. Browse websites with SymZilla and GB-proxy

For the browser workflow described in the forum, the network daemon is only one part of the setup. **SymZilla also needs GB-proxy running on a PC.** The proxy is not required merely to start the daemon, use SymTel, or make the reported non-browser network tests.

1. Obtain a [network-enabled SymZilla build](https://github.com/salvogendut/symapp-symzilla) and install it in SymbOS.
2. On a PC on the same reachable network, install and start [salvogendut/GB-proxy](https://github.com/salvogendut/GB-proxy), following its current quick-start instructions. Its documented source setup requires Python 3.9 or newer; a Windows PowerShell launcher is also provided.
3. Configure the proxy to listen on a LAN-accessible address and port. Its current quick start uses port **`5001`** and an explicit LAN bind; the default loopback-only listener cannot be reached from the MSX. Allow that port through the PC's firewall for your trusted local network. Do not expose the proxy to the public Internet.
4. Use the PC's **LAN IP address**, for example `192.168.1.10`, in the proxy's advertised host setting where required. Do not use `localhost` or `127.0.0.1` as the MSX's proxy destination.
5. In SymZilla, open **Edit > Options...**, then set **Network / GB proxy** to the PC endpoint, for example:

   ```text
   http://192.168.1.10:5001
   ```

   Replace both address and port with your actual proxy settings.

6. Keep both the PC proxy and the SymbOS network daemon running, then try loading a page.

SymZilla connects to GB-proxy over **plain HTTP**; the proxy fetches upstream pages over HTTP or HTTPS and converts the content for the browser. Viewing an HTTPS site's content through this proxy is **not evidence of native TLS support in the v0.4 PicoVerse daemon**, nor is the browser-to-proxy leg encrypted.

## 7. Troubleshooting

| Symptom | What to check |
| --- | --- |
| Nextor boots, but SymbOS cannot access its disk | Confirm `-IDEsdPV.DRV` is selected, not the stock Sunrise driver; recheck the IDE slot, master channel, partition, and `Removable: N` setting |
| `SYMSETUP` cannot find the patched driver | Keep the leading hyphen and `.DRV` extension; put the actual binary in the installation directory; check that you did not download an HTML page |
| Sunrise IDE is not autodetected by setup | Add/configure the driver manually; this is a known SymbOS issue when Nextor is installed |
| Explorer can browse the card, but DOS/SymbOS cannot use it | Confirm the boot volume is FAT16, not FAT32/exFAT; for an MBR layout use the Nextor entry's `WHOLE DISK` setting where available, and select the correct primary partition in SymbOS |
| Disk access breaks after changing firmware/profile | WiFi changes the IDE subslot; update the disk driver's slot in `SYMSETUP` |
| Disk works in Explorer, but the daemon does not | Upgrade to v2.59 for the adapter-detection fix and enable Wifi on the Nextor entry. Further hardware validation is pending; a LoadROM WiFi image remains the confirmed fallback |
| The daemon cannot find the adapter under LoadROM | Verify `-w`, ESP-01 installation and firmware, and the active slot/profile; allow the automatic scan to finish |
| The daemon shows `No IP` or loses its link | Check access-point association, credentials, DHCP, and WiFi availability; the daemon periodically rechecks the link |
| Network tests work, but SymZilla cannot load pages | Check the GB-proxy process, PC LAN IP, listening interface, port, firewall, and SymZilla proxy setting |
| Trying to open `netd-esp.exe` on the PC fails | It is a SymbOS executable; copy it to the MSX storage and run it inside SymbOS |
| Assembling disk/network sources gives missing labels or files | Use the supplied binaries for installation; standalone source assembly requires the corresponding constants, wrappers, and SymbOS build environment |

## 8. Sources and credits

The setup above combines the upstream files with the reported installation procedure and PicoVerse's current storage/slot documentation. It is not a new hardware validation of every combination.

- **JZZY / Jzzy-dev**: [patched Sunrise IDE driver](https://github.com/Jzzy-dev/msx-symbos-sunrise-ide-driver-picoverse) and [PicoVerse SymbOS network daemon](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon), especially its [ESP driver notes](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon/blob/main/README-ESP-driver.md).
- **Prodatron / SymbiosiS**: [SymbOS downloads and official manual](https://www.symbos.org/download.htm) and [original MSX mass-storage drivers](https://github.com/Prodatron/symdrv-msx-massstorage).
- **Oduvaldo Pavan Jr. / ducasp**: [ESP8266 UNAPI firmware](https://github.com/ducasp/ESP8266-UNAPI-Firmware).
- **salvogendut**: [GB-proxy](https://github.com/salvogendut/GB-proxy) and [network-enabled SymZilla](https://github.com/salvogendut/symapp-symzilla).
- **MSX Resource Center, page 37**: [October 10 disk-driver installation instructions](https://www.msx.org/forum/msx-talk/hardware/msx-picoverse?page=36#comment-491224).
- **MSX Resource Center, page 38**: [October 11 daemon startup and known-working LoadROM image](https://www.msx.org/forum/msx-talk/hardware/msx-picoverse?page=37#comment-491274), [confirmation that switching from Explorer to LoadROM fixed networking](https://www.msx.org/forum/msx-talk/hardware/msx-picoverse?page=37#comment-491281), and [October 17 v0.5 HTTPS announcement](https://www.msx.org/forum/msx-talk/hardware/msx-picoverse?page=37#comment-491574).

Driver source snapshots checked: [disk driver `bbfc9d4`](https://github.com/Jzzy-dev/msx-symbos-sunrise-ide-driver-picoverse/tree/bbfc9d4a25330729a43c4b7676f5376dfc1641b4) and [network daemon `5504da8`](https://github.com/Jzzy-dev/msx-picoverse-symbos-network-daemon/tree/5504da893350259391bbf14a79e8fc571fa65956). Download links above track the main branches; consult newer upstream notes when using updated binaries.
