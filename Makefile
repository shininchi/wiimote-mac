CFLAGS = -std=c11 -Wall -Wextra -O2
FRAMEWORKS = -framework CoreFoundation -framework IOKit -framework Foundation \
             -framework IOBluetooth -framework CoreBluetooth
SOURCES = src/main.c src/wiimote.c src/pair.m
HEADERS = src/wiimote.h src/pair.h

wiimote-test: $(SOURCES) $(HEADERS) src/Info.plist
	$(CC) -Wall -Wextra -O2 -fobjc-arc $(FRAMEWORKS) \
		-Wl,-sectcreate,__TEXT,__info_plist,src/Info.plist $(SOURCES) -o $@
	codesign -f -s - -i com.shininchi.wiimote-guitar $@

test: tests/test_wiimote.c src/wiimote.c src/wiimote.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined tests/test_wiimote.c src/wiimote.c -o test_wiimote
	./test_wiimote

clean:
	rm -rf wiimote-test test_wiimote test_wiimote.dSYM

.PHONY: test clean
