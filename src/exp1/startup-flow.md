# exp1 启动流程：从 QEMU 到四核输出

这份文档说明当前 `src/exp1` 是如何跑起来的。现在的 `exp1` 已经不是最初的单核 Mini UART Hello World，而是做了这些扩展：

- 使用 PL011 UART0，而不是 Mini UART。
- 使用 `make run` 一键 build + run。
- QEMU 以 `-smp 4` 启动 4 个 core。
- core0 释放 core1/2/3。
- 每个 core 设置独立 stack。
- core0 只初始化一次 UART。
- 其他 core 用 acquire 等待初始化完成。
- UART 输出由 spinlock 保护，避免多核输出交错。

最终运行时会看到类似：

```text
Hello, from processor 0
Hello, from processor 3
Hello, from processor 2
Hello, from processor 1
```

顺序不固定是正常的，因为 4 个 core 是并发运行的。

## 1. 如何运行

进入当前目录：

```bash
cd ~/code/p1-kernel/src/exp1
make run
```

[Makefile.qemu](Makefile.qemu) 里的 `run` 目标会先依赖 `kernel8.img`，所以它会自动构建，再运行 QEMU：

```bash
qemu-system-aarch64 -M raspi3b -smp 4 -kernel ./kernel8.img -serial mon:stdio -nographic
```

退出 QEMU：

```text
Ctrl-a，然后按 x
```

## 2. 关键文件

当前 `exp1` 的关键文件：

- [Makefile.qemu](Makefile.qemu)：构建 `kernel8.img`，并提供 `make run`。
- [src/linker-qemu.ld](src/linker-qemu.ld)：链接脚本，决定镜像从 `0x80000` 开始。
- [src/boot.S](src/boot.S)：最早执行的启动汇编，负责多核 bring-up 和进入 C。
- [include/mm.h](include/mm.h)：定义 `LOW_MEMORY` 和 `CORE_STACK_SIZE`。
- [src/kernel.c](src/kernel.c)：C 入口，负责 UART 初始化同步和四核打印。
- [include/sync.h](include/sync.h)：同步接口声明。
- [src/sync.S](src/sync.S)：`ldar/stlr` 和 `ldaxr/stxr` 同步原语。
- [src/uart.c](src/uart.c)：PL011 UART0 初始化和字符收发。
- [include/uart.h](include/uart.h)：UART 对外接口。
- [include/peripherals/uart.h](include/peripherals/uart.h)：PL011 UART0 寄存器定义。
- [include/peripherals/gpio.h](include/peripherals/gpio.h)：GPIO 寄存器定义。
- [src/utils.S](src/utils.S)：`put32`、`get32`、`delay`。
- [src/mm.S](src/mm.S)：`memzero`，用于清空 `.bss`。

## 3. 构建流程

[Makefile.qemu](Makefile.qemu) 使用交叉编译工具：

```text
aarch64-linux-gnu-gcc
aarch64-linux-gnu-ld
aarch64-linux-gnu-objcopy
```

构建过程：

```text
src/*.c
  -> build/*_c.o

src/*.S
  -> build/*_s.o

所有 .o
  -> build/kernel8.elf
  -> kernel8.img
```

`kernel8.elf` 带符号信息，适合调试；`kernel8.img` 是 QEMU 实际加载的裸二进制镜像。

## 4. QEMU 运行参数

`make run` 执行的核心命令是：

```bash
qemu-system-aarch64 -M raspi3b -smp 4 -kernel ./kernel8.img -serial mon:stdio -nographic
```

参数含义：

```text
-M raspi3b
  模拟 Raspberry Pi 3B。

-smp 4
  创建 4 个 CPU core。

-kernel ./kernel8.img
  加载当前目录下的裸机内核镜像。

-serial mon:stdio
  把 UART0 和 QEMU monitor 接到当前终端。

-nographic
  不打开图形窗口，全部在终端里显示。
```

注意：`-smp 4` 只是让 QEMU 创建 4 个 core，并不意味着 core1/2/3 会自动运行我们的 `_start`。secondary cores 仍然需要 core0 通过 spin-table + `sev` 释放。

## 5. 链接脚本与入口

[src/linker-qemu.ld](src/linker-qemu.ld) 开头：

```ld
. = 0x80000;
.text.boot : { *(.text.boot) }
```

[src/boot.S](src/boot.S) 把 `_start` 放进 `.text.boot`：

```asm
.section ".text.boot"
.globl _start
_start:
```

因此启动关系是：

```text
QEMU 加载 kernel8.img
  -> 镜像从 0x80000 布局
  -> .text.boot 在最前面
  -> CPU 首先执行 boot.S:_start
```

## 6. core id 和 per-core stack

[src/boot.S](src/boot.S) 开始会读取当前 core id：

```asm
mrs	x0, mpidr_el1
and	x0, x0, #0xFF
mov	x19, x0
```

`mpidr_el1 & 0xff` 得到 processor index：

```text
core0 -> 0
core1 -> 1
core2 -> 2
core3 -> 3
```

然后每个 core 设置独立 stack：

```asm
add	x1, x19, #1
mov	x2, #CORE_STACK_SIZE
mul	x1, x1, x2
mov	x2, #LOW_MEMORY
add	x1, x1, x2
mov	sp, x1
```

[include/mm.h](include/mm.h) 中：

```c
#define LOW_MEMORY       (2 * SECTION_SIZE)
#define CORE_STACK_SIZE  PAGE_SIZE
```

也就是每个 core 有 4KB 的栈空间，避免多个 core 共用同一个 `sp`。

## 7. core0 清空 `.bss`

只有 core0 会进入 `master`，并清空 `.bss`：

```asm
adr	x0, bss_begin
adr	x1, bss_end
sub	x1, x1, x0
bl 	memzero
```

`bss_begin` 和 `bss_end` 来自 [src/linker-qemu.ld](src/linker-qemu.ld)。`memzero` 实现在 [src/mm.S](src/mm.S)。

这个步骤必须只做一次。如果多个 core 同时清 `.bss`，可能会把其他 core 已经写入的全局状态清掉。

## 8. 释放 secondary cores

core1/2/3 在 QEMU/RPi 的 holding pen 中等待。它们会观察低地址的启动槽：

```text
core1: 0xe0
core2: 0xe8
core3: 0xf0
```

core0 清完 `.bss` 后写入 `_start`：

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

`sev` 是 Send Event。它会唤醒正在 `wfe` 等待的 secondary cores。它们读到自己的启动槽中有 `_start` 地址后，就会跳到 `_start`，重新进入我们的启动代码。

这里的等待逻辑不是项目源码里的代码，而是 QEMU `raspi3b` 机器模型或真实 RPi 固件提供的启动协议。

## 9. boot_ready：启动阶段同步

core0 释放 secondary cores 后，用 release store 发布 `boot_ready`：

```asm
mov	x0, #1
adr	x1, boot_ready
stlr	w0, [x1]
```

其他 core 等待：

```asm
wait_for_master:
	adr	x1, boot_ready
1:
	ldar	w0, [x1]
	cbz	w0, 1b
```

这里使用的是 `stlr/ldar`，不是普通 `str/ldr`，也不是只靠 `volatile`。

含义：

```text
stlr
  release store。core0 在发布 boot_ready 之前完成的初始化，对看到 boot_ready 的其他 core 可见。

ldar
  acquire load。其他 core 看到 boot_ready 后，后续执行不会被重排到 acquire 之前。
```

## 10. 进入 C：kernel_main(processor_id)

启动汇编最后把 core id 作为第一个参数传给 C：

```asm
kernel_entry:
	mov	x0, x19
	bl	kernel_main
```

AArch64 调用约定里，第一个参数放在 `x0`。所以 C 里是：

```c
void kernel_main(unsigned int processor_id)
```

## 11. UART 只初始化一次

[src/kernel.c](src/kernel.c) 中，只有 core0 初始化 UART：

```c
if (processor_id == 0) {
	uart_init();
	store_release(&uart_ready, 1);
}
```

其他 core 等待：

```c
while (!load_acquire(&uart_ready)) {
}
```

这一步避免多个 core 同时初始化 UART，也保证其他 core 开始输出时 UART 已经配置完成。

`store_release` 和 `load_acquire` 来自 [src/sync.S](src/sync.S)，底层分别使用 `stlr` 和 `ldar`。

## 12. UART 输出锁

多个 core 同时写 UART 会导致字符交错。为了保护输出，[src/kernel.c](src/kernel.c) 在打印时使用 spinlock：

```c
spin_lock(&uart_lock);
uart_send_string("Hello, from processor ");
uart_send('0' + processor_id);
uart_send_string("\r\n");
spin_unlock(&uart_lock);
```

锁的实现在 [src/sync.S](src/sync.S)：

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

关键点：

```text
ldaxr
  exclusive acquire load。

stxr
  exclusive store。如果期间有人改过 lock，写入失败并重试。

stlr wzr
  release store，把 lock 释放为 0。
```

这个锁保证每个 core 的整行输出不会和其他 core 的输出交错。

## 13. PL011 UART0

当前 `exp1` 使用的是 PL011 UART0，不是 Mini UART。

寄存器定义在 [include/peripherals/uart.h](include/peripherals/uart.h)：

```c
#define UART0_BASE  (PBASE + 0x00201000)
#define UART0_DR    (UART0_BASE + 0x00)
#define UART0_FR    (UART0_BASE + 0x18)
#define UART0_IBRD  (UART0_BASE + 0x24)
#define UART0_FBRD  (UART0_BASE + 0x28)
#define UART0_LCRH  (UART0_BASE + 0x2C)
#define UART0_CR    (UART0_BASE + 0x30)
```

[src/uart.c](src/uart.c) 中：

- GPIO14/15 配成 ALT0，对应 UART0 TX/RX。
- baud rate 使用 48MHz 时钟计算 `IBRD/FBRD`。
- `uart_send` 等待 `UART_FR_TXFF` 清零后写 `UART0_DR`。
- `uart_recv` 等待 `UART_FR_RXFE` 清零后读 `UART0_DR`。

## 14. 总流程图

```text
make run
  |
  +--> make 构建 kernel8.img
  |
  +--> qemu-system-aarch64 -M raspi3b -smp 4 ...
        |
        v
      core0 进入 boot.S:_start
        |
        +--> 设置 core0 stack
        +--> 清空 .bss
        +--> 写 0xe0/0xe8/0xf0 = _start
        +--> sev 唤醒 core1/2/3
        +--> stlr boot_ready = 1
        |
        v
      kernel_main(0)
        |
        +--> uart_init()
        +--> store_release(uart_ready, 1)
        +--> spin_lock
        +--> 打印 Hello, from processor 0
        +--> spin_unlock

      core1/2/3 被释放后进入 boot.S:_start
        |
        +--> 读取自己的 processor_id
        +--> 设置自己的 stack
        +--> ldar 等待 boot_ready
        |
        v
      kernel_main(1/2/3)
        |
        +--> load_acquire 等待 uart_ready
        +--> spin_lock
        +--> 打印 Hello, from processor N
        +--> spin_unlock
```

## 15. 当前 exp1 的验收点

执行：

```bash
make run
```

应该看到四行输出：

```text
Hello, from processor 0
Hello, from processor 1
Hello, from processor 2
Hello, from processor 3
```

顺序可以不同，但应该满足：

- 四个 processor 都出现。
- 每一行内部不发生字符交错。
- UART 初始化只执行一次。
- 使用 release/acquire 同步，不只依赖 `volatile`。
- UART 输出使用 spinlock 保护。
