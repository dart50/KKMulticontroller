all: build

build: xx.o

xx.raw: xx.o

xx.o: XXcontrol_KR.c
	avr-gcc -mmcu=atmega88a -Wall -g3 -gdwarf-2 -std=gnu99 -DF_CPU=8000000UL -Os -fsigned-char -funsigned-bitfields -fpack-struct -fshort-enums -o xx.o XXcontrol_KR.c
	avr-objcopy -j .text -j .data -O binary xx.o xx.raw
	size xx.o

program: xx.raw
	avrdude -c dragon_isp -p m88 -P usb -U flash:w:xx.raw:r