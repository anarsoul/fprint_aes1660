.PHONY: clean all
all: fprint_aes1660

fprint_aes1660: fprint_aes1660.c
	gcc $^ -o $@ `pkg-config --cflags --libs libusb-1.0`

clean:
	rm -f fprint_aes1660
