# 🎛️ Ploytec USB Audio & MIDI Driver

**Resurrecting the best DJ mixers ever made.**

This repository contains an open-source, reverse-engineered driver for **Ploytec-based USB interfaces**. If you own an **Allen & Heath Xone:DB4**, **DB2**, or **DX**, you probably noticed that the official drivers died years ago.

We fixed that.

---

## 🚀 The Philosophy

We provide two ways to run this.

1.  **CoreAudio HAL Plug-in (Recommended):**
    This uses **Classic IOKit** to punch a hole directly to the USB bus from userspace.
    * **SIP Compatible:** Works perfectly with System Integrity Protection (SIP) and AMFI **enabled**.
    * **Zero-Copy:** Audio doesn't bounce around user-space unnecessarily.
    * **Zero-Latency:** (Well, near zero). We use direct ring-buffers in shared memory.
    * **Robust:** No "System Extension Blocked" loops. It just works.

2.  **DriverKit System Extension:**
    The "modern" Apple way using a **System Extension (dext)**.
    * **Sandboxed:** Runs in the DriverKit environment.
    * **Requires SIP Disabled:** Because we do not yet have the specific DriverKit entitlements from Apple, you must disable SIP and AMFI to load this driver. One day, if Apple grants us the entitlements, this will be the signed, secure standard.

---

## 🎚️ Supported Devices

| Device | Status | Notes |
| :--- | :--- | :--- |
| **Allen & Heath Xone:DB4** | ✅ **Perfect** | The beast works. 8-in/8-out. |
| **Allen & Heath Xone:DB2** | ✅ **Perfect** | Your effects unit is back online. |
| **Allen & Heath Xone:DX** | ✅ **Perfect** | 2010 called, it wants its Serato controller back. |
| **Allen & Heath Xone:4D** | ✅ **Verified** | Analog filters meet digital routing. |
| **Allen & Heath WZ4:USB** | 🚧 *WIP* | We are still teaching it how to count channels. |
| **Smyth Research A16** | ⚪ *Planned* | Audioholics, we see you. |

---

## 🖥️ Platform Support

### 🐧 Linux (The "Just Works" Kernel Module)
If you run Linux, you are already cool. This is a standard ALSA kernel module.
* **Audio:** 8x8 Channels (PCM)
* **MIDI:** In/Out fully supported via ALSA Sequencer.
* **Modes:** Supports both **BULK** and **INTERRUPT** transfer modes automatically.

### 🍎 macOS (HAL Plugin)
We bypass the generic class drivers to give you raw performance.
* **Audio:** 8x8 Channels (CoreAudio HAL).
* **MIDI:** CoreMIDI driver utilizing a lock-free ring buffer in shared memory.

### 🍎 macOS (DriverKit Extension)
The modern, sandboxed implementation.
* **Audio:** Standard CoreAudio integration via `AudioDriverKit`.
* **Security:** Runs entirely in DriverKit.

---

## 💿 Installation

### 🍎 macOS: Option A - HAL Plugin (Recommended)

This gives you the lowest latency and works **without** disabling system security.

1.  **Download** the latest release.
2.  **Run the Installer:**
    * Right-click `install_hal.command` and select **Open**.
    * *Note:* If you double-click and get a "Developer cannot be verified" or "Permission Denied" error, go to **System Settings** -> **Privacy & Security** and look for the "Open Anyway" button near the bottom, or just right-click and Open again.
3.  **Authenticate:** Type your password (we need `sudo` to copy files to `/Library/Audio`).
4.  The script will:
    * Install the drivers.
    * Nuke the "Gatekeeper Quarantine" (Apple doesn't like fun).
    * Restart `coreaudiod`. **(Your audio will glitch for 2 seconds. Relax.)**
5.  Plug in your mixer. Rock on.

### 🍎 macOS: Option B - DriverKit (System Extension)

**⚠️ Important:** To run this extension without an official Apple entitlement, you **must** disable System Integrity Protection (SIP) and Apple Mobile File Integrity (AMFI).

**Prerequisites:**
1.  Boot into **Recovery Mode** (Hold Cmd+R on Intel, or Power Button on Apple Silicon).
2.  Open Utilities -> Terminal.
3.  Run: `csrutil disable`
4.  Reboot normally.
5.  Open Terminal and run: `sudo boot-args="amfi_get_out_of_my_way=0x1"`
6.  Reboot again.

**Installation:**
1.  **Download** the release.
2.  **Run the Installer:**
    * Right-click `install_dext.command` and select **Open**.
    * *Note:* If you double-click and get a "Developer cannot be verified" or "Permission Denied" error, go to **System Settings** -> **Privacy & Security** and look for the "Open Anyway" button near the bottom, or just right-click and Open again.
3.  The script will copy the "Ploytec Driver Extension.app" to `/Applications` and launch it.
4.  **Activate:** Click "Activate" in the window that appears.
5.  **Allow:** Go to **System Settings** -> **Privacy & Security** and allow the extension.

### 🐧 Linux (Power User Mode)

1.  Clone and build:
    ```bash
    git clone https://github.com/mischa85/snd-xonedb4
    cd snd-xonedb4
    sudo ./install_linux.sh
    ```
2.  That's it. We even reload the module for you.

---

## 🏗️ Building from Source (The "Signing" Pain)

If you are a developer or want to build the latest code, you **must** code-sign the binaries.

### 1. Get a Free Signing Identity
You do not need a paid Apple Developer program membership ($99/yr). A free Apple ID works fine.
1.  Open **Xcode**.
2.  Go to **Settings (Cmd+,)** -> **Accounts**.
3.  Click the **`+`** button and add your Apple ID.
4.  Select your Personal Team.
5.  Click **Manage Certificates...**.
6.  Click the **`+`** in the bottom left and select **Apple Development**.
7.  Wait for it to say "Created". You now have a valid certificate in your Keychain.

### 2. Build & Install
Run the build script. It will auto-detect your new certificate:
```bash ./install_hal.command```

---

## 🕵️ Troubleshooting

### 🔑 Signing & Build Issues

**"The build script says '0 valid identities found'."**
This is the most common error. It means `security find-identity -v -p codesigning` returns nothing, even if you added your account in Xcode.

**Fix 1: The "Trust" Trap (Most Likely)**
Did you manually change the certificate trust settings in Keychain Access?

1. Open **Keychain Access**.
2. Find your **"Apple Development: [Your Name]"** certificate.
3. Right-click -> **Get Info**.
4. Expand the **Trust** section.
5. **CRITICAL:** Set everything to **"Use System Defaults"**.
* *Why?* If you set this to "Always Trust", macOS adds a custom trust policy that breaks the codesign toolchain. It must stay on "System Defaults".

**Fix 2: Missing WWDR Intermediate Certificate**
Your certificate relies on an Apple intermediate authority. If it's missing, your cert is invalid.

1. Open Keychain Access.
2. Select "System Roots" or "Login" and look for **"Apple Worldwide Developer Relations Certification Authority"**.
3. If missing or expired, download the [Worldwide Developer Relations - G3 (Intermediate)](https://www.apple.com/certificateauthority/) from Apple and double-click to install.

### 🛠 Uninstalling

* **HAL:** Run `uninstall_hal.command`.
* **DriverKit:** Run `uninstall_dext.command`.
* **Linux:** Run `uninstall_linux.sh`.

---

## 🛠️ For Developers

The **HAL Plugin** implementation in this project is a rare example of a **User-Space USB Audio Driver** on macOS that *doesn't* use DriverKit. It utilizes:

1. **IOUSBLib:** For raw pipe access directly from the HAL plugin.
2. **AudioServerPlugIn:** To talk to CoreAudio.
3. **POSIX Shared Memory:** To tunnel MIDI data between the Audio driver and the MIDI driver.

Feel free to steal our code for your own obscure hardware projects.

---

## ☕ Fuel the Project

If this driver saved your $2000 mixer from becoming a doorstop, consider buying me a coffee. It fuels the late-night reverse engineering sessions.

<a href="https://www.buymeacoffee.com/mischa85" target="_blank">
<img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174">
</a>

---

## License

MIT License.
*Do whatever you want*
