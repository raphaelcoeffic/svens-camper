CC = arm-none-eabi-gcc
CFLAGS = -mcpu=cortex-m3 -mthumb -Os -g -ffreestanding -nostdlib -Wall -Wextra -T link.ld -Wl,--no-warn-rwx-segments
charger_emu.bin: main.c link.ld
	$(CC) $(CFLAGS) main.c -o charger_emu.elf
	arm-none-eabi-objcopy -O binary charger_emu.elf $@
	arm-none-eabi-size charger_emu.elf
clean:
	rm -f charger_emu.elf charger_emu.bin
