# Ploytec Driver

This repository provides a cross-platform open-source **audio and MIDI driver** for various **Ploytec-based interfaces**, including multiple **Allen & Heath Xone** models and compatible hardware.  
It supports both **BULK** and **INTERRUPT** USB streaming modes and is available for **Linux** and **macOS**.

---

## Overview

The goal of this project is to deliver reliable and well-documented driver support for devices that were previously **unavailable on Linux** and only worked on **very old macOS versions**.  
Both the **Linux kernel module** and **macOS DriverKit system extension** have reached a **fairly stable** stage and are usable for everyday audio work.

Developing under macOS **DriverKit** has proven challenging due to sparse documentation and limited reference code. This project reflects many lessons learned from building an end-to-end user-space + driver-space audio stack — from USB packet handling to CoreAudio and CoreMIDI integration.

---

## Supported Devices

| Device | Status |
|:--|:--|
| Allen & Heath Xone:DB4 | ✅ Supported |
| Allen & Heath Xone:DB2 | ✅ Supported |
| Allen & Heath Xone:DX | ✅ Supported |
| Allen & Heath Xone:2D | ⚪ Planned |
| Allen & Heath Xone:4D | ✅ Supported |
| Allen & Heath WZ4:USB | 🔧 *Work in progress — support for non-8x8 channel layouts is being implemented* |
| Smyth Research A16 Realiser | ⚪ Planned |

---

## Platform Support

### Linux
- ✅ PCM Out — 8 channels  
- ✅ PCM In — 8 channels  
- ✅ Sample Rate Switching  
- ✅ MIDI Out  
- ✅ MIDI In  

### macOS
- ✅ PCM Out — 8 channels  
- ✅ PCM In — 8 channels  
- ✅ Driver configuration via UI  
- ⚙️ Sample Rate Switching (partial support)  
- ✅ MIDI Out  
- ✅ MIDI In  

---

## Installation

### Linux

1. Clone the repository:
   ```bash
   git clone https://github.com/mischa85/snd-xonedb4
   cd snd-xonedb4
   ```

2. Build the kernel module:
   ```bash
   make linux
   ```

3. Compress and install:
   ```bash
   zstd snd-usb-xonedb4.ko
   sudo cp -f snd-usb-xonedb4.ko.zst /usr/lib/modules/$(uname -r)/kernel/sound/usb
   sudo depmod
   sudo modprobe snd-usb-xonedb4
   ```

---

### macOS

> **Note:** The driver uses **DriverKit** and runs as a **System Extension**.  
> These steps must be followed carefully to ensure proper loading and permission approval.

1. **Set DriverKit debug boot arguments (before rebooting):**
   ```bash
   sudo nvram boot-args="amfi_get_out_of_my_way=0x1"
   ```

2. **Reboot into Recovery Mode:**
   - **Intel:** Hold `⌘ + R` during boot.  
   - **Apple Silicon:** Hold the power button → select *Options → Continue*.

3. **Disable System Integrity Protection (SIP)** *(for testing/development use only)*:
   ```bash
   csrutil disable
   reboot
   ```

4. **After reboot, clone and build the project:**
   ```bash
   git clone https://github.com/mischa85/snd-xonedb4
   cd snd-xonedb4
   make mac
   ```

5. **Install Xcode and configure signing:**
   - Download **Xcode** from the Mac App Store.  
   - Open **Xcode → Settings → Accounts**, and sign in with your Apple ID to create a free developer account.  
   - Ensure a valid signing identity is available by running:
     ```bash
     security find-identity -v
     ```
     If this outputs `0 valid identities found`, you need to fix your signing setup in Xcode before proceeding.

6. **Copy the Ploytec Driver Extension to the Applications:**
   ```bash
   make mac-install
   ```

7. **Enable logging before starting the driver:**
   ```bash
   make mac-logstream
   ```
   This starts a live log view to catch errors during startup.

   Then start **Ploytec Driver Extension** from your **Applications** folder.

---

## Troubleshooting

- **System Extension approval prompt:**  
  After installation, macOS may block the extension. Open **System Settings → Privacy & Security**, scroll to the bottom, and click **Allow** next to the developer name.

- **⚠️ “Allow” button missing in Privacy & Securit):**  
  macOS only shows the “Allow” button for a short time after a system extension is blocked.  
  It will **hide the button** if:
  - more than ~30 minutes have passed  
  - the Mac has gone to sleep or locked  
  - the user rebooted before clicking Allow  
  - System Settings was already open when the extension was blocked  

  To force macOS to show the “Allow” dialog again:

  1. **Quit System Settings completely**  
     (right-click → Quit)
  2. Reset the System Extension state:  
     ```bash
     systemextensionsctl reset
     ```
  3. Reinstall from the app.  

  4. Immediately open **System Settings → Privacy & Security**  
     The **Allow** button should now be visible again.

- **Driver not loading:**  
  Run:
  ```bash
  make mac-logshow
  ```
  Review logs for entitlement, signature, or permission errors.

- **No valid developer identities:**  
  Ensure your Apple ID is added under **Xcode → Settings → Accounts**, then run:
  ```bash
  security find-identity -v
  ```
  If it still shows *0 valid identities*, recreate the certificate via Xcode’s automatic signing.

- **MIDI not working:**  
  On macOS, MIDI runs in user space. The **Ploytec Driver Extension** app must be running for MIDI I/O to function.

## Additional Notes


- Current builds support **8-in / 8-out PCM** layouts only. Work is ongoing for devices with alternate configurations.  
- Both Linux and macOS versions are stable for everyday use, though active development continues.  
- On macOS, rare audio distortion may occur if the host application changes buffer size — this is under investigation.

---

## Contributing

Contributions and testing feedback are welcome!  
Particularly valuable are reports on alternate hardware, unusual channel layouts, or improvements to the macOS DriverKit implementation.

---

## Support the Project ☕

If this project helped you, consider buying me a coffee:

<a href="https://www.buymeacoffee.com/mischa85" target="_blank">
  <img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174">
</a>

Your support helps keep this project maintained and compatible with evolving systems.

---

## License

MIT License © 2025  
Developed and maintained by the community.
