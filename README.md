# Ploytec Driver

This is a driver for several Ploytec audio/MIDI interfaces. It supports both BULK and INTERRUPT streaming modes.

The development of the macOS driverkit driver has been an unpleasant experience to say the least. Apart from the largely undocumented functions and the severe lack of examples, which made the development a hell, there are many issues regarding the codesigning. Of course it would be easier if I could just make a signed binary available, but as Apple wants $99/year for this I kindly refuse to pay that out of my own pocket. If anyone is able to get me one of those I'll start supplying a signed binary for the driver.

**Supported Devices**:

- [x] Allen&Heath Xone:DB4
- [x] Allen&Heath Xone:DB2
- [x] Allen&Heath Xone:DX
- [ ] Allen&Heath Xone:2D
- [x] Allen&Heath Xone:4D
- [x] Allen&Heath WZ4:USB
- [ ] Smyth Research A16 Realiser

**Linux**:

- [x] PCM out 8 channels
- [x] PCM in 8 channels
- [x] Sample Rate Switching
- [x] MIDI out
- [x] MIDI in

**macOS**:

- [x] PCM out 8 channels
- [x] PCM in 8 channels
- [x] Driver config in UI
- [ ] Sample Rate Switching
- [x] MIDI out
- [x] MIDI in

How to install:

**Linux**:

- Clone the repo using ```git clone https://github.com/mischa85/snd-xonedb4```
- Change the directory to the cloned repo: ```cd snd-xonedb4```
- ```make linux``` to compile the kernel module.
- ```zstd snd-usb-xonedb4.ko``` to compress the compiled kernel module.
- ```cp -f snd-usb-xonedb4.ko.zst /usr/lib/modules/$(uname -r)/kernel/sound/usb``` to copy the kernel module to the running kernel.
- ```depmod``` to rebuild the module dependency tree.
- ```modprobe snd-usb-xonedb4```

**macOS**:

- Open a terminal.
- ```sudo nvram boot-args="amfi_get_out_of_my_way=0x1"```
- Reboot the system. Intel: ```COMMAND ⌘ + R``` pressed while booting to enter recovery. Apple Silicon: Keep the power button pressed, and select Options to enter 1TR.
- Open a terminal.
- ```csrutil disable``` to disable System Integrity Protection.
- Reboot to macOS.
- Open a terminal.
- Clone the repo using ```git clone https://github.com/mischa85/snd-xonedb4```
- Change the directory to the cloned repo: ```cd snd-xonedb4```
- Get a (free) Apple developer account via Xcode. Make sure it's active in Settings -> Accounts. Double check if it actually works using ```security find-identity -v```. If this still gives "0 valid identities found", you need to troubleshoot the chain of trust. I know this is a frustrating issue, but please don't waste my time by opening issues about it. There's plenty of things to try if you search the net for this issue.
- ```make mac``` to compile the driver.
- If you installed an earlier version, uninstall it first! ```systemextensionsctl list``` to list, ```systemextensionsctl uninstall <teamID> <bundleID>``` to uninstall.
- ```make mac-install``` to move it to the ```/Applications``` directory.
- Start ```Ploytec Driver Extension``` from Applications.

Random comments:
- On macOS MIDI in/out is implemented in userspace. The Ploytec Driver Extension application needs to run for MIDI to work.
- On Linux MIDI out is still sent too fast. Small fix, will do soon.
- On macOS there can be rare instances where the audio gets distorted. This could happen when an application changes the audio buffer size. There's probably better ways to sync up the audio again.
- You might need to manually uninstall older versions of the driver on macOS. You can list them using ```systemextensionsctl list``` and uninstall using ```systemextensionsctl uninstall <teamID> <bundleID>```.

<a href="https://www.buymeacoffee.com/mischa85" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174"></a>
