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
	open build/Release/PloytecApp.app

install:
	rsync -a --delete build/Release/PloytecApp.app /Applications

run: install
	open /Applications/PloytecApp.app

systemextensionsctl-list:
	systemextensionsctl list
