# exp2 启动流程：从 EL2 切到 EL1

这份文档放在 `src/exp2` 目录下，用来串起 `exp2` 的完整运行流程。`exp2` 继承了 `exp1` 的基本启动、UART 输出和 QEMU 运行方式，但新增了一个核心目标：

```text
让 CPU 从启动时的高 Exception Level 进入 EL1，然后在 kernel_main 中确认当前 EL。
```

在 QEMU 路径下，本实验大致是：

```text
make -f Makefile.qemu
  -> 编译 exp2 的 C 和汇编源码
  -> 链接生成 build/kernel8.elf
  -> objcopy 生成 kernel8.img

p1-run
  -> QEMU 加载 kernel8.img
  -> CPU 进入 boot.S:_start
  -> boot.S 初始化 EL1/EL2 相关系统寄存器
  -> 通过 eret 从 EL2 返回到 EL1
  -> 进入 el1_entry
  -> 设置 sp
  -> 调用 kernel_main
  -> 初始化 UART 和 printf
  -> 打印当前 Exception Level
```

## 1. 关键文件

`exp2` 的关键文件如下，链接可以直接点击跳转：

- [Makefile.qemu](Makefile.qemu)：QEMU 版本构建脚本。
- [src/linker-qemu.ld](src/linker-qemu.ld)：链接脚本，决定内核镜像从 `0x80000` 开始布局。
- [src/boot.S](src/boot.S)：启动汇编，也是本实验最重要的文件。
- [src/kernel.c](src/kernel.c)：C 入口，打印当前 Exception Level。
- [src/utils.S](src/utils.S)：提供 `get_el`、`put32`、`get32`、`delay`。
- [include/utils.h](include/utils.h)：声明 `get_el` 等汇编函数。
- [include/arm/sysregs.h](include/arm/sysregs.h)：定义系统寄存器配置值，比如 `SCTLR_VALUE_MMU_DISABLED`、`HCR_VALUE`、`SPSR_VALUE`。
- [src/mini_uart.c](src/mini_uart.c)：UART 和 `putc`。
- [src/printf.c](src/printf.c)：轻量级 `printf` 实现。
- [include/printf.h](include/printf.h)：`printf` 接口声明和宏。
- [../../env-qemu.sh](../../env-qemu.sh)：项目根目录的 QEMU 运行辅助脚本。

## 2. 如何运行 exp2

和 `exp1` 一样：

```bash
cd ~/code/p1-kernel
source env-qemu.sh

cd src/exp2
make -f Makefile.qemu
p1-run
```

也可以手动运行：

```bash
cd ~/code/p1-kernel/src/exp2
make -f Makefile.qemu
qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic
```

退出 QEMU：

```text
Ctrl-a，然后按 x
```

如果运行成功，你应该能看到类似：

```text
Exception level: 1
```

这就是 `exp2` 的核心验收点：内核最终运行在 EL1。

## 3. exp2 相比 exp1 新增了什么

`exp1` 的主线是：

```text
boot.S
  -> 设置栈
  -> kernel_main
  -> UART 输出 Hello, world!
```

`exp2` 的主线变成：

```text
boot.S
  -> 配置系统寄存器
  -> 用 eret 切换到 EL1
  -> 设置栈
  -> kernel_main
  -> get_el()
  -> printf 当前 EL
```

新增的重点是 ARM64 的 Exception Level。

ARM64 有多个异常级别：

```text
EL0  用户程序
EL1  操作系统内核
EL2  Hypervisor
EL3  Secure monitor
```

这个项目最终要实现的是一个内核，所以希望内核主逻辑运行在 EL1。

## 4. QEMU 从哪个 EL 启动

在 [src/boot.S](src/boot.S) 中可以看到：

```asm
#ifdef USE_QEMU
	...
#else
	...
#endif
```

`Makefile.qemu` 会加上：

```make
-DUSE_QEMU
```

所以 QEMU 编译路径会走 `#ifdef USE_QEMU` 分支。

这个分支的背景是：

```text
QEMU 启动时 CPU 在 EL2。
真实 Raspberry Pi 3 硬件启动时 CPU 通常在 EL3。
```

所以 `exp2` 里同时保留了 QEMU 和真实硬件两条路径：

- QEMU：从 EL2 切到 EL1。
- Rpi3：从 EL3 切到 EL1。

你现在主要关心 QEMU 路径即可。

## 5. `boot.S` 的启动流程

文件：[src/boot.S](src/boot.S)

开头仍然和 `exp1` 类似，先只保留 core 0：

```asm
mrs	x0, mpidr_el1
and	x0, x0,#0xFF
cbz	x0, master
b	proc_hang
```

含义：

```text
读取当前 CPU core id
如果是 core 0，跳到 master
否则进入死循环
```

`master` 是真正开始初始化 CPU 状态的地方。

## 6. 配置 `SCTLR_EL1`

在 [src/boot.S](src/boot.S) 中：

```asm
ldr	x0, =SCTLR_VALUE_MMU_DISABLED
msr	sctlr_el1, x0
```

`SCTLR_EL1` 是 EL1 的 System Control Register。它控制很多 EL1 层面的行为，比如 MMU、cache、大小端等。

配置值来自 [include/arm/sysregs.h](include/arm/sysregs.h)：

```c
#define SCTLR_VALUE_MMU_DISABLED \
	(SCTLR_RESERVED | SCTLR_EE_LITTLE_ENDIAN | SCTLR_I_CACHE_DISABLED | \
	 SCTLR_D_CACHE_DISABLED | SCTLR_MMU_DISABLED)
```

这一步的意思是：

```text
为即将进入的 EL1 准备一个简单状态：
  -> little endian
  -> 关闭 I-cache
  -> 关闭 D-cache
  -> 关闭 MMU
```

现在还没有虚拟内存，所以 MMU 先关着。虚拟内存会在后面的 `exp6` 里出现。

## 7. 配置 `HCR_EL2`

接着：

```asm
ldr	x0, =HCR_VALUE
msr	hcr_el2, x0
```

`HCR_EL2` 是 Hypervisor Configuration Register。

配置值在 [include/arm/sysregs.h](include/arm/sysregs.h)：

```c
#define HCR_RW (1 << 31)
#define HCR_VALUE HCR_RW
```

`HCR_RW` 表示下一级 EL1 使用 AArch64，而不是 AArch32。

也就是说，这一步在告诉 CPU：

```text
等会从 EL2 返回到 EL1 时，EL1 要以 64-bit 模式运行。
```

## 8. 准备 `eret`

`eret` 是 exception return，异常返回指令。

它不是普通函数返回。它会根据系统寄存器里的信息，决定：

```text
返回到哪个 Exception Level
返回后从哪条指令开始执行
返回后 PSTATE 是什么状态
```

在 QEMU 路径下，[src/boot.S](src/boot.S) 做了两件事。

### 设置 `SPSR_EL2`

```asm
ldr	x0, =SPSR_VALUE
msr	spsr_el2, x0
```

`SPSR_EL2` 决定 `eret` 之后的目标状态。

配置值来自 [include/arm/sysregs.h](include/arm/sysregs.h)：

```c
#define SPSR_MASK_ALL (7 << 6)
#define SPSR_EL1h     (5 << 0)
#define SPSR_VALUE    (SPSR_MASK_ALL | SPSR_EL1h)
```

这里最关键的是 `SPSR_EL1h`。

它表示：

```text
eret 后进入 EL1，并且使用 EL1 自己的栈指针 SP_EL1。
```

`SPSR_MASK_ALL` 则是先屏蔽异常，避免还没装异常处理时被打断。

### 设置 `ELR_EL2`

```asm
adr x0, el1_entry
msr	elr_el2, x0
```

`ELR_EL2` 是 Exception Link Register。它决定 `eret` 后从哪里继续执行。

这里把它设成：

```text
el1_entry
```

所以后面执行：

```asm
eret
```

CPU 就会：

```text
从 EL2 切到 EL1
然后跳到 el1_entry
```

## 9. QEMU 路径下的 `.bss` 问题

你会看到 [src/boot.S](src/boot.S) 里清 `.bss` 的代码在 `el1_entry_another`：

```asm
el1_entry_another:
	adr	x0, bss_begin
	adr	x1, bss_end
	sub	x1, x1, x0
	bl 	memzero

el1_entry:
	mov	sp, #LOW_MEMORY
	bl	kernel_main
```

但是 QEMU 路径里设置的是：

```asm
adr x0, el1_entry
msr	elr_el2, x0
```

也就是说，QEMU 路径会直接跳到 `el1_entry`，不会经过 `el1_entry_another`。

这一版 `exp2` 的 C 代码没有依赖必须清零的大型 `.bss` 状态，所以这样也能运行。真实硬件路径设置的是 `el1_entry_another`，会先清 `.bss` 再进入 `el1_entry`。

后面实验会继续调整启动流程，你主要记住：

```text
exp2 的核心不是 .bss，而是 EL 切换。
```

## 10. 进入 `kernel_main`

切到 EL1 之后执行：

```asm
el1_entry:
	mov	sp, #LOW_MEMORY
	bl	kernel_main
```

和 `exp1` 一样，这里先设置栈指针 `sp`，然后调用 C 函数。

`LOW_MEMORY` 定义在 [include/mm.h](include/mm.h)，值是 4MB。

## 11. `kernel_main` 做了什么

文件：[src/kernel.c](src/kernel.c)

代码：

```c
#include "printf.h"
#include "utils.h"
#include "mini_uart.h"

void kernel_main(void)
{
	uart_init();
	init_printf(0, putc);
	int el = get_el();
	printf("Exception level: %d \r\n", el);

	while (1) {
		uart_send(uart_recv());
	}
}
```

流程是：

```text
uart_init()
  -> 初始化 UART

init_printf(0, putc)
  -> 告诉 printf：以后输出字符时调用 putc

get_el()
  -> 读取当前 CPU Exception Level

printf(...)
  -> 通过 UART 打印当前 EL

while (1)
  -> 继续回显输入字符
```

## 12. `get_el` 在哪里

声明在 [include/utils.h](include/utils.h)：

```c
extern int get_el(void);
```

实现 在 [src/utils.S](src/utils.S)：

```asm
.globl get_el
get_el:
	mrs x0, CurrentEL
	lsr x0, x0, #2
	ret
```

`CurrentEL` 是 ARM64 系统寄存器。它的值里包含当前 Exception Level，但最低两位保留不用，所以代码右移 2 位：

```text
CurrentEL >> 2
```

最终返回：

```text
1 表示 EL1
2 表示 EL2
3 表示 EL3
```

所以如果你看到：

```text
Exception level: 1
```

说明这次 EL 切换成功。

## 13. `printf` 为什么能输出

`exp1` 里只有：

```c
uart_send_string("Hello, world!\r\n");
```

`exp2` 加了一个轻量级 `printf`，文件是：

- [src/printf.c](src/printf.c)
- [include/printf.h](include/printf.h)

但是 `printf` 自己不知道如何输出一个字符，所以 `kernel_main` 里先调用：

```c
init_printf(0, putc);
```

`putc` 定义在 [src/mini_uart.c](src/mini_uart.c)：

```c
void putc(void* p, char c)
{
	uart_send(c);
}
```

所以输出链路是：

```text
printf
  -> putc
  -> uart_send
  -> 写 UART 硬件寄存器
  -> QEMU 把 UART 输出显示到终端
```

## 14. exp2 的总流程图

把完整流程串起来：

```text
make -f Makefile.qemu
  |
  v
编译 src/kernel.c、src/mini_uart.c、src/printf.c
编译 src/boot.S、src/mm.S、src/utils.S
  |
  v
根据 src/linker-qemu.ld 链接成 build/kernel8.elf
  |
  v
objcopy 生成 kernel8.img
  |
  v
p1-run 启动 QEMU
  |
  v
CPU 进入 src/boot.S:_start
  |
  +--> 只保留 core 0
  +--> 设置 SCTLR_EL1
  +--> 设置 HCR_EL2
  +--> 设置 SPSR_EL2，指定 eret 后进入 EL1h
  +--> 设置 ELR_EL2，指定 eret 后跳到 el1_entry
  |
  v
eret
  |
  v
CPU 从 EL2 切到 EL1
  |
  v
el1_entry
  |
  +--> 设置 sp
  +--> 调用 kernel_main
  |
  v
kernel_main
  |
  +--> uart_init()
  +--> init_printf(0, putc)
  +--> get_el()
  +--> printf("Exception level: %d", el)
  +--> 回显输入字符
```

## 15. exp2 你需要完成/确认什么

`exp2` 的验收点可以这样看：

1. 能编译：

```bash
make -f Makefile.qemu
```

2. 能运行：

```bash
p1-run
```

3. 能看到当前 EL：

```text
Exception level: 1
```

4. 能解释为什么是 1：

```text
QEMU 从 EL2 启动
  -> boot.S 设置 SPSR_EL2 和 ELR_EL2
  -> eret 后进入 EL1
  -> get_el 读取 CurrentEL
  -> printf 打印 1
```

如果这些你能说清楚，`exp2` 的主线就完成了。
