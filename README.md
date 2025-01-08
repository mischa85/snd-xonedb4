# snd-xonedb4

This is a driver for the Allen & Heath Xone:DB4 mixer. The driver requires the latest Ploytec (audio) firmware to be flashed to the mixer. These are part of the "device software" section of the Allen&Heath website.

The development of the macOS driverkit driver has been an unpleasant experience to say the least. Apart from the largely undocumented functions and the severe lack of examples, which made the development a hell, there are many issues regarding the codesigning. Of course it would be easier if I could just make a signed binary available, but as Apple wants $99/year for this I kindly refuse to pay that out of my own pocket. If anyone is able to get me one of those I'll start supplying a signed binary for the driver.

Also, with minor modifications these drivers should also work on Allen&Heath Xone:DB2 and Allen&Heath Xone:4D.

**Supported Devices**:

- [x] Allen&Heath Xone:DB4
- [x] Allen&Heath Xone:DB2
- [x] Allen&Heath Xone:DX

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

**Mac**:

In MacOS recovery:

```
csrutil disable
sudo nvram boot-args="amfi_get_out_of_my_way=0x1"
```

Reboot to MacOS.

```
systemextensionsctl developer on
```

```
security find-identity
```

Change CHANGEME in codesign.sh.

```
cd mac-coreaudio
make build
```

<a href="https://www.buymeacoffee.com/mischa85" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174"></a>
