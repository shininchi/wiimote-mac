FRAMEWORKS = -framework CoreFoundation -framework IOKit -framework Foundation \
             -framework IOBluetooth -framework CoreBluetooth
CORE = src/wiimote.c src/session.c src/pair.m
HEADERS = src/wiimote.h src/session.h src/pair.h
ADDON_DLL = yarg-addon/bin/Release/netstandard2.1/WiimoteGuitarBridge.dll
APP = build/WiimoteMac.app
# "-" signs ad hoc, which macOS treats as a new app on every build, so it asks for
# Bluetooth permission again. A personal certificate avoids that:
#   make app CODESIGN_IDENTITY="My Certificate"
CODESIGN_IDENTITY ?= -

wiimote-test: src/main.c $(CORE) $(HEADERS) src/Info.plist
	$(CC) -Wall -Wextra -O2 -fobjc-arc $(FRAMEWORKS) \
		-Wl,-sectcreate,__TEXT,__info_plist,src/Info.plist src/main.c $(CORE) -o $@
	codesign -f -s "$(CODESIGN_IDENTITY)" -i com.shininchi.wiimote-test $@

# The add-on compiles against YARG's own files. Set YARG_APP to your YARG.app.
app: $(CORE) $(HEADERS) src/app/main.m src/app/YargSetup.m src/app/YargSetup.h src/app/Info.plist
	@test -n "$(YARG_APP)" || { echo 'Set YARG_APP to your YARG.app, for example: make app YARG_APP="/path/to/YARG.app"'; exit 1; }
	(cd yarg-addon && dotnet build -c Release --nologo "-p:ManagedDir=$(YARG_APP)/Contents/Resources/Data/Managed")
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	$(CC) -Wall -Wextra -O2 -fobjc-arc -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 \
		$(FRAMEWORKS) -framework AppKit -weak_framework ServiceManagement -weak_framework UniformTypeIdentifiers \
		src/app/main.m src/app/YargSetup.m $(CORE) -o $(APP)/Contents/MacOS/WiimoteMac
	cp src/app/Info.plist $(APP)/Contents/Info.plist
	cp $(ADDON_DLL) $(APP)/Contents/Resources/
	codesign --force --deep -s "$(CODESIGN_IDENTITY)" $(APP)

test: tests/test_wiimote.c src/wiimote.c src/wiimote.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined tests/test_wiimote.c src/wiimote.c -o test_wiimote
	./test_wiimote

clean:
	rm -rf wiimote-test test_wiimote test_wiimote.dSYM build

.PHONY: app test clean
