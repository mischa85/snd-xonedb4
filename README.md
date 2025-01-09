# snd-xonedb4

This is a driver for several Ploytec audio/MIDI interfaces. It supports both BULK and INTERRUPT streaming modes.

The development of the macOS driverkit driver has been an unpleasant experience to say the least. Apart from the largely undocumented functions and the severe lack of examples, which made the development a hell, there are many issues regarding the codesigning. Of course it would be easier if I could just make a signed binary available, but as Apple wants $99/year for this I kindly refuse to pay that out of my own pocket. If anyone is able to get me one of those I'll start supplying a signed binary for the driver.

**Supported Devices**:

- [x] Allen&Heath Xone:DB4
- [x] Allen&Heath Xone:DB2
- [x] Allen&Heath Xone:DX
- [ ] Allen&Heath Xone:4D

**Linux**:

- [x] PCM out 8 channels
- [x] PCM in 8 channels
- [x] Sample Rate Switching
- [x] MIDI out
- [x] MIDI in

**Mac**:

- [x] PCM out 8 channels
- [x] PCM in 8 channels
- [ ] Driver config in UI
- [ ] Sample Rate Switching
- [ ] MIDI out
- [ ] MIDI in

How to install:

**Linux**:

```
make linux
```

**macOS (Apple)**:

- Reboot the system and keep ```COMMAND ⌘ + R``` pressed while booting, this will bring you in recovery.
- Open a terminal.
- ```csrutil disable``` to disable System Integrity Protection.
- ```sudo nvram boot-args="amfi_get_out_of_my_way=0x1"```
- Reboot to macOS.
- Open a terminal.
- ```systemextensionsctl developer on``` to enable developer mode. This allows for the app to run outside of the ```/Applications``` directory.
- Clone the repo using ```git clone https://github.com/mischa85/snd-xonedb4```
- Change the directory to the cloned repo: ```cd snd-xonedb4```
- Get a (free) Apple developer account via Xcode.
- Get your developer ID using ```security find-identity -v```
- Change CHANGEME in codesign.sh for this developer ID: ```sed -i '' 's/CHANGEME/XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX/g' mac-coreaudio/codesign.sh```
- ```cd mac-coreaudio```
- ```make build```
- Start the app in the build folder.

<a href="https://www.buymeacoffee.com/mischa85" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174"></a>
