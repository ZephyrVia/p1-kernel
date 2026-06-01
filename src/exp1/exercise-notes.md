# exp1 扩展练习报告

## 目标

在 `exp1` 的裸机 Hello World 基础上完成三个扩展：

1. 使用 `baud_rate` 常量计算 UART baud rate 寄存器值。
2. 将串口设备从 Mini UART 切换为 PL011 UART0。
3. 启动 4 个 processor cores，并让每个 core 打印自己的编号。

最终运行目标：

```text
Hello, from processor 0
Hello, from processor 1
Hello, from processor 2
Hello, from processor 3
```

输出顺序可以不同，但四个 core 都必须出现，且每一行不能发生字符交错。

## 1. baud rate 常量化

修改位置：

- [src/uart.c](src/uart.c)

核心修改：

```c
static const unsigned int uart_clock_freq = 48000000;
static const unsigned int baud_rate = 115200;

static void uart_baud_reg_values(unsigned int baud, unsigned int* ibrd, unsigned int* fbrd)
{
	unsigned int baud_divisor = 16 * baud;
	unsigned int remainder = uart_clock_freq % baud_divisor;

	*ibrd = uart_clock_freq / baud_divisor;
	*fbrd = ((remainder * 64) + (baud_divisor / 2)) / baud_divisor;

	if (*fbrd == 64) {
		*ibrd += 1;
		*fbrd = 0;
	}
}
```

在 `uart_init` 中使用计算结果：

```c
uart_baud_reg_values(baud_rate,&ibrd,&fbrd);
put32(UART0_IBRD,ibrd);
put32(UART0_FBRD,fbrd);
```

达到的目的：

- 不再写死 baud rate 寄存器值。
- 修改 `baud_rate` 后，可以重新计算 UART 分频寄存器。
- 当前 PL011 UART0 使用 48MHz 时钟作为 baud rate 基准。

## 2. 从 Mini UART 切换到 PL011 UART0

修改位置：

- [include/peripherals/uart.h](include/peripherals/uart.h)
- [include/uart.h](include/uart.h)
- [src/uart.c](src/uart.c)
- [src/kernel.c](src/kernel.c)

新增 PL011 UART0 寄存器定义：

```c
#define UART0_BASE  (PBASE + 0x00201000)

#define UART0_DR    (UART0_BASE + 0x00)
#define UART0_FR    (UART0_BASE + 0x18)
#define UART0_IBRD  (UART0_BASE + 0x24)
#define UART0_FBRD  (UART0_BASE + 0x28)
#define UART0_LCRH  (UART0_BASE + 0x2C)
#define UART0_CR    (UART0_BASE + 0x30)
#define UART0_IMSC  (UART0_BASE + 0x38)
#define UART0_ICR   (UART0_BASE + 0x44)
```

GPIO14/15 改为 UART0 的 ALT0：

```c
selector = get32(GPFSEL1);
selector &= ~(7<<12);
selector |= 4<<12;
selector &= ~(7<<15);
selector |= 4<<15;
put32(GPFSEL1,selector);
```

PL011 UART0 初始化：

```c
put32(UART0_CR,0);
put32(UART0_ICR,0x7FF);
put32(UART0_IMSC,0);

uart_baud_reg_values(baud_rate,&ibrd,&fbrd);
put32(UART0_IBRD,ibrd);
put32(UART0_FBRD,fbrd);
put32(UART0_LCRH,UART_LCRH_WLEN_8BIT | UART_LCRH_FEN);
put32(UART0_CR,UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
```

发送和接收改为使用 PL011 的 `DR` 和 `FR`：

```c
void uart_send(char c)
{
	while (1) {
		if (!(get32(UART0_FR)&UART_FR_TXFF))
			break;
	}
	put32(UART0_DR,c);
}

char uart_recv(void)
{
	while (1) {
		if (!(get32(UART0_FR)&UART_FR_RXFE))
			break;
	}
	return(get32(UART0_DR)&0xFF);
}
```

达到的输出目的：

- OS 不再依赖 Mini UART。
- QEMU 通过 PL011 UART0 输出文本。
- `kernel.c` 仍然通过 `uart_init`、`uart_send`、`uart_send_string` 使用统一接口。

## 3. 使用 4 个 processor cores

修改位置：

- [Makefile.qemu](Makefile.qemu)
- [env-qemu.sh](../../env-qemu.sh)
- [include/mm.h](include/mm.h)
- [include/sync.h](include/sync.h)
- [src/boot.S](src/boot.S)
- [src/kernel.c](src/kernel.c)
- [src/sync.S](src/sync.S)

### 3.1 QEMU 启动 4 个 core

在 [Makefile.qemu](Makefile.qemu) 中增加 `run` 目标：

```make
run : kernel8.img
	$(QEMU) -M raspi3b -smp 4 -kernel ./kernel8.img -serial mon:stdio -nographic
```

达到的目的：

- 在 `src/exp1` 下执行 `make run`，即可 build 并用 4 个 core 启动 QEMU。

### 3.2 每个 core 设置独立 stack

在 [include/mm.h](include/mm.h) 中增加：

```c
#define CORE_STACK_SIZE PAGE_SIZE
```

在 [src/boot.S](src/boot.S) 中根据 `processor_id` 设置 `sp`：

```asm
mrs	x0, mpidr_el1
and	x0, x0, #0xFF
mov	x19, x0
add	x1, x19, #1
mov	x2, #CORE_STACK_SIZE
mul	x1, x1, x2
mov	x2, #LOW_MEMORY
add	x1, x1, x2
mov	sp, x1
```

达到的目的：

- core0、core1、core2、core3 不共用同一个栈。
- 避免多核进入 C 函数后互相覆盖调用栈。

### 3.3 释放 secondary cores

core0 清空 `.bss` 后，向 low-memory spin-table 写入 `_start`：

```asm
ldr	x0, =_start
mov	x1, #0xe0
str	x0, [x1]
mov	x1, #0xe8
str	x0, [x1]
mov	x1, #0xf0
str	x0, [x1]
sev
```

含义：

```text
core1: 0xe0
core2: 0xe8
core3: 0xf0
```

达到的目的：

- `-smp 4` 只创建 4 个 core，不会自动让 core1/2/3 执行 `_start`。
- core0 写入启动地址并执行 `sev` 后，QEMU/RPi holding pen 中等待的 secondary cores 会跳转到 `_start`。

### 3.4 使用 release/acquire 同步

启动阶段在 [src/boot.S](src/boot.S) 中使用 `stlr/ldar` 同步 `boot_ready`：

```asm
mov	x0, #1
adr	x1, boot_ready
stlr	w0, [x1]
```

```asm
wait_for_master:
	adr	x1, boot_ready
1:
	ldar	w0, [x1]
	cbz	w0, 1b
```

C 阶段在 [src/kernel.c](src/kernel.c) 中同步 UART 初始化：

```c
if (processor_id == 0) {
	uart_init();
	store_release(&uart_ready, 1);
}

while (!load_acquire(&uart_ready)) {
}
```

达到的目的：

- core0 只初始化一次 UART。
- 其他 core 必须等 UART 初始化完成后才能输出。
- 不依赖 `volatile` 作为多核同步手段。

### 3.5 使用 spinlock 保护 UART 输出

在 [src/sync.S](src/sync.S) 中实现 spinlock：

```asm
spin_lock:
	mov	w2, #1
1:
	ldaxr	w1, [x0]
	cbnz	w1, 2f
	stxr	w1, w2, [x0]
	cbnz	w1, 1b
	ret
2:
	clrex
	b	1b

spin_unlock:
	stlr	wzr, [x0]
	ret
```

在 [src/kernel.c](src/kernel.c) 中保护输出：

```c
spin_lock(&uart_lock);
uart_send_string("Hello, from processor ");
uart_send('0' + processor_id);
uart_send_string("\r\n");
spin_unlock(&uart_lock);
```

达到的输出目的：

- 多个 core 不会同时写 UART。
- 每一行输出保持完整，不会出现字符级交错。

## 4. 验证结果

执行：

```bash
cd src/exp1
make run
```

观察到类似输出：

```text
Hello, from processor 3
Hello, from processor 2
Hello, from processor 0
Hello, from processor 1
```

输出顺序不固定是正常的，因为四个 core 并发运行。验证重点是：

- processor 0、1、2、3 都出现。
- 每一行完整，没有字符交错。
- UART 可以正常输出。

## 结论

本次扩展后，`exp1` 从单核串口 Hello World 变成了一个可以使用 PL011 UART0、支持 baud rate 常量计算、并能启动 4 个 processor cores 的裸机程序。通过 release/acquire 同步和 spinlock，保证了 UART 初始化只执行一次，并且多核输出不会互相打乱。
