LDFLAGS=`pkg-config --libs libusb-1.0`
CFLAGS= -Wall -O2 `pkg-config --cflags libusb-1.0`

all: hidkey_gpio

hidkey_gpio: main.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	@rm -f hidkey_gpio *.o
