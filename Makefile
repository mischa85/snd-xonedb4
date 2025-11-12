obj-m += snd-usb-xonedb4.o
snd-usb-xonedb4-objs := common/ploytec.o linux-alsa/chip.o linux-alsa/pcm.o linux-alsa/midi.o

linux:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

mac:
	xcodebuild -project "$(CURDIR)/mac-coreaudio/PloytecDriver.xcodeproj" -configuration Release SYMROOT="$(CURDIR)/build"
	./codesign.sh

linux-clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

mac-clean:
	rm -rf build

mac-install:
	rsync -a --delete "build/Release/Ploytec Driver Extension.app" /Applications

mac-logstream:
	log stream --predicate 'sender == "sc.hackerman.ploytecdriver.dext"' --predicate 'sender == "Ploytec Driver Extension"'

mac-logshow:
	log show --predicate 'sender == "sc.hackerman.ploytecdriver.dext"' --predicate 'sender == "Ploytec Driver Extension"'
