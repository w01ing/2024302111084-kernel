# Lab1 骨架 Makefile（课程提供；新增源文件时把它加入 OBJS）。
CC   = riscv64-unknown-elf-gcc
LD   = riscv64-unknown-elf-ld
OBJCOPY ?= riscv64-unknown-elf-objcopy
CFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc -fno-stack-protector -fno-pie
QEMU = qemu-system-riscv64

# LAB2-STAGE4：三个链接基址为 0 的内嵌用户程序。
UPROGS = sh hi spin badaccess memcap guardtest crosscopy

# LAB2-STAGE4：用户程序使用与内核一致的 freestanding RISC-V 选项，
# 额外的 -I. 让 user/*.c 能找到根目录下的 kernel/*.h。
UFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc \
         -fno-stack-protector -fno-pie -I.

# LAB2-STAGE1：
# trap.o 包含用户态 trap 路径的 C 语言部分；赠送的 trampoline 对象
# 包含汇编部分，必须一起链接进内核镜像。
# LAB2-STAGE2：
# syscall.o 负责按照 ABI 解析系统调用参数并进行分发。
# LAB2-STAGE4：
# exec.o 负责加载内嵌的平铺用户程序，并提供最小 fork/wait/exec 运行时；
# userimg.o 包含自动生成的用户程序表。
OBJS = kernel/entry.o kernel/start.o kernel/console.o kernel/printf.o \
       kernel/main.o kernel/trap.o kernel/syscall.o kernel/exec.o \
       kernel/vm.o kernel/userimg.o kernel/trampoline.o

kernel/kernel: $(OBJS) kernel/kernel.ld
	$(LD) -T kernel/kernel.ld -o $@ $(OBJS)

%.o: %.c kernel/riscv.h kernel/course_sid.h
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

# LAB2-STAGE4：由赠送的 usys.pl 生成用户态系统调用桩。
user/usys.S: user/usys.pl
	perl user/usys.pl > user/usys.S

# LAB2-STAGE4：用户 C 源码 -> ELF(基址 0) -> 平铺二进制。
user-flat/%.bin: user/%.c user/user.h user/ulib.c user/printf.c user/usys.S
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c user/ulib.c   -o user-flat/ulib.o
	$(CC) $(UFLAGS) -Iuser -c user/printf.c -o user-flat/printf.o
	$(CC) $(UFLAGS) -Iuser -c user/usys.S   -o user-flat/usys.o
	$(CC) $(UFLAGS) -Iuser -c $<            -o user-flat/$*.o
	@if [ -f user/common.c ]; then \
	  $(CC) $(UFLAGS) -Iuser -c user/common.c -o user-flat/common.o; \
	  EXTRA_OBJS=user-flat/common.o; \
	else EXTRA_OBJS=; fi; \
	$(LD) -T user/user.ld -o user-flat/$*.elf \
	    user-flat/$*.o $$EXTRA_OBJS user-flat/ulib.o user-flat/printf.o user-flat/usys.o
	$(OBJCOPY) -O binary --set-section-flags .bss=alloc,load,contents user-flat/$*.elf $@

# LAB2-STAGE4：生成内嵌程序表。每项是 start/end/name，程序二进制
# 紧跟在该项后面；内核侧按 end 向上对齐到 8 字节遍历下一项。
kernel/userimg.S: Makefile $(addprefix user-flat/,$(UPROGS:=.bin))
	printf '.section .rodata\n .global _uprog_table\n_uprog_table:\n' > $@.tmp
	for p in $(UPROGS); do \
	  printf ' .quad _uprog_%s_start\n .quad _uprog_%s_end\n .asciz "%s"\n .balign 8\n' \
	    $$p $$p $$p >> $@.tmp; \
	  printf ' .global _uprog_%s_start\n_uprog_%s_start:\n .incbin "user-flat/%s.bin"\n .global _uprog_%s_end\n_uprog_%s_end:\n .balign 8\n' \
	    $$p $$p $$p $$p $$p >> $@.tmp; \
	done
	printf ' .quad 0\n .quad 0\n' >> $@.tmp
	mv $@.tmp $@

# 验收环境固定(排雷环节禁止改动本行以下内容)
qemu: kernel/kernel
	$(QEMU) -machine virt -bios none -kernel kernel/kernel -nographic

clean:
	rm -f kernel/*.o kernel/kernel kernel/userimg.S user/usys.S
	rm -rf user-flat
