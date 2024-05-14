

ULIB = usys.o ulib.o printf.o

usys.S : usys.pl
	perl usys.pl > usys.S

usys.o : usys.S
	$(CC) $(CFLAGS) -c -o usys.o usys.S

.PRECIOUS: %.o

$(TOPDIR)/user/_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -T ./user.ld -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

initcode: initcode.S 
	$(CC) $(CFLAGS) -march=rv64g -nostdinc -I ../kernel/include/ -I ../arch/riscv/include/ -c initcode.S -o initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o initcode.out initcode.o
	$(OBJCOPY) -S -O binary initcode.out initcode
	$(OBJDUMP) -S initcode.o > initcode.asm

all: initcode $(UPROGS)
	@echo test

clean:
	@rm -rf initcode initcode.out usys.S
	@rm -rf _*