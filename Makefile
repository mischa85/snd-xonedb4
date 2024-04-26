obj-m += snd-usb-xonedb4.o
snd-usb-xonedb4-objs := common/ploytec.o linux-alsa/chip.o linux-alsa/pcm.o linux-alsa/midi.o

linux:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

linux-install:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
