# exp1 启动流程：这个内核是如何跑起来的

这份文档放在 `src/exp1` 目录下，目的很直接：你不用先把所有源码都读完，先通过这一份文档理解 `exp1` 是如何从源码变成一个能在 QEMU 里运行的裸机内核的。

你可以把整个流程先记成这一条线：

```text
source ../../env-qemu.sh
  -> 加载 p1-run 这类辅助命令

make -f Makefile.qemu
  -> 编译 C 和汇编源码
  -> 链接成 build/kernel8.elf
  -> 转成 kernel8.img

p1-run
  -> QEMU 模拟 Raspberry Pi 3B
  -> 加载当前目录的 kernel8.img
  -> CPU 从 boot.S 的 _start 开始执行
  -> 设置最小运行环境
  -> 调用 kernel_main
  -> 初始化 UART
  -> 打印 Hello, world!
```

## 1. 先看目录结构

当前目录是 `src/exp1`。这一版实验是最小完整内核，关键文件如下：

- [Makefile.qemu](Makefile.qemu)：QEMU 版本的构建脚本。
- [Makefile.rpi3](Makefile.rpi3)：真实 Raspberry Pi 3 硬件版本的构建脚本。
- [src/linker-qemu.ld](src/linker-qemu.ld)：QEMU 版本的链接脚本，决定内核镜像的内存布局。
- [src/boot.S](src/boot.S)：CPU 最先执行的启动汇编。
- [src/kernel.c](src/kernel.c)：第一个 C 入口，里面有 `kernel_main`。
- [src/uart.c](src/uart.c)：PL011 UART 初始化和字符收发。
- [src/utils.S](src/utils.S)：`put32`、`get32`、`delay` 这些底层工具函数。
- [src/mm.S](src/mm.S)：`memzero`，用于启动时清空 `.bss`。
- [include/mm.h](include/mm.h)：内存相关常量，比如 `LOW_MEMORY`。
- [include/uart.h](include/uart.h)：UART 对外函数声明。
- [include/peripherals/base.h](include/peripherals/base.h)：外设基地址。
- [include/peripherals/gpio.h](include/peripherals/gpio.h)：GPIO 寄存器地址。
- [include/peripherals/uart.h](include/peripherals/uart.h)：PL011 UART0 寄存器地址。
- [../../env-qemu.sh](../../env-qemu.sh)：项目根目录下的 QEMU 辅助脚本。

这些链接在 Markdown 预览里会显示成蓝色，可以直接点击跳转。

## 2. 你应该怎么运行

推荐用 bash：

```bash
cd ~/code/p1-kernel
source env-qemu.sh

cd src/exp1
make -f Makefile.qemu
p1-run
```

如果你已经在 `src/exp1` 目录下，也可以这样：

```bash
source ../../env-qemu.sh
make -f Makefile.qemu
p1-run
```

退出 QEMU：

```text
Ctrl-a，然后按 x
```

如果你在 fish 里，不想切 bash，也可以直接手动运行 QEMU：

```bash
qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic
```

## 3. `source env-qemu.sh` 做了什么

文件：[../../env-qemu.sh](../../env-qemu.sh)

`source` 的意思是把脚本内容加载进当前 shell，而不是开一个子进程执行完就结束。

这个脚本定义了几个 shell 函数：

```bash
p1-run
p1-run-log
p1-run-debug
p1-run-gdb
```

其中 `p1-run` 的定义很简单：

```bash
p1-run() {
    run
}
```

而 `run` 最终会执行 QEMU：

```bash
qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic
```

所以要注意：

- `p1-run` 不是 `make` 命令。
- `p1-run` 不会编译代码。
- `p1-run` 只是启动当前目录下的 `kernel8.img`。
- 如果当前目录没有 `kernel8.img`，或者你不在具体实验目录下，它就跑不起来。

你可以用这个命令确认：

```bash
type p1-run
type run
```

## 4. `make -f Makefile.qemu` 做了什么

文件：[Makefile.qemu](Makefile.qemu)

这一条命令负责把源码构建成 QEMU 可以加载的内核镜像。

项目使用交叉编译器：

```make
ARMGNU ?= aarch64-linux-gnu
```

所以实际调用的是：

```text
aarch64-linux-gnu-gcc
aarch64-linux-gnu-ld
aarch64-linux-gnu-objcopy
```

为什么要交叉编译？因为你的电脑大概率是 x86-64，而这个内核要运行在 ARM64 的 Raspberry Pi 3B 上。

构建结果主要有两个：

```text
build/kernel8.elf
kernel8.img
```

它们的区别：

- `build/kernel8.elf`：带符号和段信息，适合 GDB 调试。
- `kernel8.img`：裸二进制镜像，QEMU 的 `-kernel` 参数加载它。

## 5. 编译 C 文件

[Makefile.qemu](Makefile.qemu) 里有这条规则：

```make
$(BUILD_DIR)/%_c.o: $(SRC_DIR)/%.c
	mkdir -p $(@D)
	$(ARMGNU)-gcc $(COPS) -MMD -c $< -o $@
```

意思是把 `src/*.c` 编译成 `build/*_c.o`。

比如：

```text
src/kernel.c     -> build/kernel_c.o
src/uart.c       -> build/uart_c.o
```

这里的 C 编译参数里有几个很关键：

```text
-nostdlib
  不链接 C 标准库。

-nostartfiles
  不使用普通程序的启动文件。

-ffreestanding
  告诉编译器：这是裸机环境，不要假设有普通操作系统。

-mgeneral-regs-only
  只使用通用寄存器，避免引入额外寄存器保存问题。
```

这也解释了为什么 [src/kernel.c](src/kernel.c) 里不能直接用普通 Linux 程序里的 `printf`。

## 6. 编译汇编文件

[Makefile.qemu](Makefile.qemu) 还有汇编规则：

```make
$(BUILD_DIR)/%_s.o: $(SRC_DIR)/%.S
	mkdir -p $(@D)
	$(ARMGNU)-gcc $(ASMOPS) -MMD -c $< -o $@
```

它会把 `src/*.S` 编译成 `build/*_s.o`。

比如：

```text
src/boot.S   -> build/boot_s.o
src/mm.S     -> build/mm_s.o
src/utils.S  -> build/utils_s.o
```

其中最重要的是 [src/boot.S](src/boot.S)，因为它包含 CPU 最早执行的 `_start`。

## 7. 链接：为什么 `_start` 会最先执行

文件：[src/linker-qemu.ld](src/linker-qemu.ld)

链接脚本控制最终镜像的内存布局：

```ld
SECTIONS
{
   . = 0x80000;
	.text.boot : { *(.text.boot) }
	.text : { *(.text) }
	.rodata : { *(.rodata) }
	.data : { *(.data) }
	. = ALIGN(0x8);
	bss_begin = .;
	.bss : { *(.bss*) }
	bss_end = .;
}
```

先看这句：

```ld
. = 0x80000;
```

它表示 QEMU 版本的内核从 `0x80000` 这个地址开始布局。

再看这句：

```ld
.text.boot : { *(.text.boot) }
```

它表示所有 `.text.boot` 段里的代码会被放在最前面。

而 [src/boot.S](src/boot.S) 开头写了：

```asm
.section ".text.boot"
.globl _start
_start:
```

所以最终关系是：

```text
linker-qemu.ld 把 .text.boot 放在镜像最前面
  -> boot.S 把 _start 放进 .text.boot
  -> QEMU 加载 kernel8.img 后，CPU 最先执行 _start
```

这就是整个项目启动链路里最关键的一步。

## 8. 生成 `kernel8.img`

链接之后，[Makefile.qemu](Makefile.qemu) 会执行：

```make
$(ARMGNU)-objcopy $(BUILD_DIR)/kernel8.elf -O binary kernel8.img
```

`objcopy` 做的事情是：

```text
build/kernel8.elf
  -> 去掉 ELF 格式信息
  -> 保留实际要加载执行的机器码和数据
  -> 生成 kernel8.img
```

QEMU 运行时用的是：

```bash
-kernel ./kernel8.img
```

所以 `kernel8.img` 是运行入口，`kernel8.elf` 主要用于调试和观察符号。

## 9. QEMU 如何启动内核

`p1-run` 实际执行：

```bash
qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic
```

参数含义：

```text
-M raspi3b
  模拟 Raspberry Pi 3B。

-kernel ./kernel8.img
  加载当前目录的裸机内核镜像。

-serial null
  第一个串口不用。

-serial mon:stdio
  把串口和 QEMU monitor 接到当前终端。

-nographic
  不开图形窗口，全部在终端里显示。
```

加载之后，QEMU 会让模拟出来的 CPU 开始执行这个镜像。因为链接脚本和 [src/boot.S](src/boot.S) 的安排，第一段项目代码就是 `_start`。

## 10. `boot.S`：进入 C 之前的准备

文件：[src/boot.S](src/boot.S)

裸机环境里，不能一上来就跑 C。C 函数运行前，至少要准备几个东西：

- 确定哪个 CPU core 负责初始化。
- 清空 `.bss`。
- 设置栈指针 `sp`。
- 跳转到 C 函数 `kernel_main`。

### 只让 core 0 继续

Raspberry Pi 3B 有多个 CPU core。启动时，多个 core 可能都开始执行同一份代码。

[src/boot.S](src/boot.S) 里先读 `mpidr_el1`：

```asm
mrs	x0, mpidr_el1
and	x0, x0, #0xFF
cbz	x0, master
b	proc_hang
```

含义：

```text
读当前 CPU core id
如果 id 是 0，跳到 master
否则进入 proc_hang 死循环
```

这个内核目前只使用 core 0，所以其他 core 被挂起。

### 清空 `.bss`

`.bss` 里放的是未显式初始化的全局变量。C 语言要求它们启动时是 0。

普通程序由操作系统和运行时帮你清，但这个内核没有那些东西，所以要自己清。

[src/boot.S](src/boot.S) 里：

```asm
adr	x0, bss_begin
adr	x1, bss_end
sub	x1, x1, x0
bl 	memzero
```

`bss_begin` 和 `bss_end` 来自 [src/linker-qemu.ld](src/linker-qemu.ld)：

```ld
bss_begin = .;
.bss : { *(.bss*) }
bss_end = .;
```

`memzero` 实现在 [src/mm.S](src/mm.S)：

```asm
memzero:
	str xzr, [x0], #8
	subs x1, x1, #8
	b.gt memzero
	ret
```

它不断写入 0，直到 `.bss` 被清完。

### 设置栈指针

C 函数调用需要栈，所以启动代码要先设置 `sp`：

```asm
mov	sp, #LOW_MEMORY
```

`LOW_MEMORY` 定义在 [include/mm.h](include/mm.h)：

```c
#define LOW_MEMORY (2 * SECTION_SIZE)
```

这里等于 4MB。也就是说，内核先把栈放到一个相对高的位置，避免栈一开始就覆盖内核镜像。

### 调用 `kernel_main`

最后：

```asm
bl	kernel_main
```

`bl` 是 branch with link，可以理解成汇编层面的函数调用。

它会跳到 [src/kernel.c](src/kernel.c) 里的：

```c
void kernel_main(void)
```

从这里开始，项目进入 C 代码。

## 11. `kernel_main`：第一个 C 入口

文件：[src/kernel.c](src/kernel.c)

代码如下：

```c
#include "uart.h"

void kernel_main(void)
{
	uart_init();
	uart_send_string("Hello, world!\r\n");

	while (1) {
		uart_send(uart_recv());
	}
}
```

这段逻辑很简单：

```text
uart_init()
  -> 初始化 UART 硬件

uart_send_string("Hello, world!\r\n")
  -> 通过串口输出 Hello, world!

while (1)
  -> 一直等输入
  -> 收到一个字符
  -> 原样发回去
```

所以你运行后能看到 `Hello, world!`，并且你在终端里输入字符时，它会回显。

## 12. UART：内核如何输出字符

文件：[src/uart.c](src/uart.c)

这个内核没有 Linux 的 stdout，也没有 C 标准库的 `printf`。它要输出字符，就必须直接操作 UART 硬件。

### 寄存器地址

外设基地址在 [include/peripherals/base.h](include/peripherals/base.h)：

```c
#define PBASE 0x3F000000
```

PL011 UART0 寄存器地址在 [include/peripherals/uart.h](include/peripherals/uart.h)：

```c
#define UART0_BASE  (PBASE + 0x00201000)
#define UART0_DR    (UART0_BASE + 0x00)
#define UART0_FR    (UART0_BASE + 0x18)
#define UART0_IBRD  (UART0_BASE + 0x24)
#define UART0_FBRD  (UART0_BASE + 0x28)
#define UART0_LCRH  (UART0_BASE + 0x2C)
#define UART0_CR    (UART0_BASE + 0x30)
```

GPIO 相关寄存器在 [include/peripherals/gpio.h](include/peripherals/gpio.h)。

这些地址不是普通变量地址，而是 memory-mapped I/O 地址。内核读写这些地址，就是在读写硬件寄存器。

### `get32` 和 `put32`

文件：[src/utils.S](src/utils.S)

```asm
put32:
	str w1,[x0]
	ret

get32:
	ldr w0,[x0]
	ret
```

含义：

```text
put32(addr, value)
  把 value 写到 addr 这个硬件寄存器地址。

get32(addr)
  从 addr 这个硬件寄存器地址读 32-bit 值。
```

[src/uart.c](src/uart.c) 里的 UART 初始化、发送、接收，本质上都是围绕这些寄存器读写展开的。

### 发送字符

[src/uart.c](src/uart.c) 里的 `uart_send`：

```c
void uart_send(char c)
{
	while (1) {
		if (!(get32(UART0_FR) & UART_FR_TXFF))
			break;
	}
	put32(UART0_DR, c);
}
```

意思是：

```text
不断检查 UART 状态寄存器
  -> 如果发送缓冲区可用
  -> 把字符写到 UART IO 寄存器
  -> 硬件把字符发出去
```

### 接收字符

`uart_recv`：

```c
char uart_recv(void)
{
	while (1) {
		if (!(get32(UART0_FR) & UART_FR_RXFE))
			break;
	}
	return get32(UART0_DR) & 0xFF;
}
```

意思是：

```text
不断检查 UART 状态寄存器
  -> 如果收到字符
  -> 从 UART IO 寄存器读出字符
```

因此 [src/kernel.c](src/kernel.c) 里的：

```c
uart_send(uart_recv());
```

就是“收一个字符，再发回去”。

## 13. 总流程图

最后把整条链路串起来：

```text
你在 src/exp1 执行 make -f Makefile.qemu
  |
  v
Makefile.qemu
  |
  +--> 编译 src/kernel.c、src/uart.c
  +--> 编译 src/boot.S、src/mm.S、src/utils.S
  +--> 根据 src/linker-qemu.ld 链接成 build/kernel8.elf
  +--> objcopy 生成 kernel8.img
  |
  v
你执行 p1-run
  |
  v
qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img ...
  |
  v
QEMU 加载 kernel8.img
  |
  v
CPU 执行 src/boot.S:_start
  |
  +--> 只让 core 0 继续
  +--> 清空 .bss
  +--> 设置 sp
  |
  v
src/kernel.c:kernel_main
  |
  +--> uart_init()
  +--> uart_send_string("Hello, world!\r\n")
  +--> while (1) uart_send(uart_recv())
```

## 14. 你现在最该记住什么

先记住这些对应关系：

- [Makefile.qemu](Makefile.qemu)：负责构建。
- [src/linker-qemu.ld](src/linker-qemu.ld)：负责安排镜像内存布局。
- `build/kernel8.elf`：带符号信息，适合调试。
- `kernel8.img`：裸镜像，QEMU 实际加载。
- [src/boot.S](src/boot.S)：CPU 进入 C 代码之前的启动汇编。
- [src/kernel.c](src/kernel.c)：第一个 C 入口。
- [src/uart.c](src/uart.c)：通过 PL011 UART0 做输入输出。
- [src/utils.S](src/utils.S)：读写硬件寄存器的底层工具。

这条链路清楚之后，再看 `exp2` 的 CPU exception level、`exp3` 的中断、`exp4` 的调度，就会有落脚点。
