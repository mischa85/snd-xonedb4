# vim: noexpandtab tabstop=8


.PHONY: build

all:

build: clean
	xcodebuild -configuration Release SYMROOT="$(CURDIR)/build"
	./codesign.sh

clean:
	rm -rf build

xcode:
	open USBApp.xcodeproj

open: 
	open build/Release/XoneDB4App.app

install:
	rsync -a --delete build/Release/XoneDB4App.app /Applications

run: install
	open /Applications/XoneDB4App.app

systemextensionsctl-list:
	systemextensionsctl list
