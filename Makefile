obj-m += snd-usb-xonedb4.o
snd-usb-xonedb4-objs := common/ploytec.o linux-alsa/chip.o linux-alsa/pcm.o linux-alsa/midi.o

linux:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

mac:
	xcodebuild -project "$(CURDIR)/mac-coreaudio/XoneDB4Driver.xcodeproj" -configuration Release SYMROOT="$(CURDIR)/build"
	./codesign.sh

linux-clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

mac-clean:
	rm -rf build

mac-install:
	rsync -a --delete build/Release/XoneDB4App.app /Applications
