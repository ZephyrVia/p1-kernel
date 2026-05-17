# exp1 练习总结

这份文档用于实验报告，简要记录本次在 `exp1` 中完成的三个练习。

## 练习 1：baud rate 常量化

要求：

```text
Introduce a constant baud_rate, calculate necessary Mini UART register values using this constant.
Make sure that the program can work using baud rates other than 115200.
```

完成内容：

- 在 UART 初始化代码中引入 `baud_rate` 常量。
- 不再直接写死 baud rate 寄存器值。
- 根据 UART 时钟和 `baud_rate` 计算需要写入的分频寄存器值。

当前实现位置：

- [src/uart.c](src/uart.c)

说明：

当前代码已经切换到 PL011 UART0，因此使用 48MHz UART 时钟计算 `UART0_IBRD` 和 `UART0_FBRD`。如果修改 `baud_rate`，对应寄存器值会随之重新计算。

## 练习 2：从 Mini UART 切换到 PL011 UART0

要求：

```text
Change the OS code to use UART device instead of Mini UART.
Use BCM2837 ARM Peripherals and ARM PrimeCell UART (PL011) manuals to figure out how to access UART registers and how to configure GPIO pins.
The UART device uses the 48MHz clock as a base.
```

完成内容：

- 新增 PL011 UART0 寄存器定义。
- 将 GPIO14/15 配置为 UART0 使用的 ALT0 功能。
- 重写 `uart_init`、`uart_send`、`uart_recv`，使用 `UART0_DR`、`UART0_FR`、`UART0_CR`、`UART0_IBRD`、`UART0_FBRD` 等 PL011 寄存器。
- 使用 48MHz 时钟作为 baud rate 计算基础。

相关文件：

- [include/peripherals/uart.h](include/peripherals/uart.h)
- [include/uart.h](include/uart.h)
- [src/uart.c](src/uart.c)
- [src/kernel.c](src/kernel.c)

验证结果：

```text
QEMU 可以通过 UART0 正常输出文本。
```

## 练习 3：使用 4 个 processor cores

要求：

```text
Try to use all 4 processor cores.
The OS should print Hello, from processor <processor index> for all of the cores.
Don’t forget to set up a separate stack for each core and make sure that Mini UART is initialized only once.
You can use a combination of global variables and delay function for synchronization.
```

完成内容：

- QEMU 使用 `-smp 4` 启动 4 个 core。
- core0 通过 low-memory spin-table 地址 `0xe0`、`0xe8`、`0xf0` 释放 core1/2/3。
- 每个 core 根据自己的 `processor_id` 设置独立 stack。
- 只有 core0 初始化 UART。
- 使用 release/acquire 同步等待 UART 初始化完成。
- 使用 spinlock 保护 UART 输出，避免多核同时写 UART 导致字符交错。
- 每个 core 打印自己的编号。

相关文件：

- [Makefile.qemu](Makefile.qemu)
- [env-qemu.sh](../../env-qemu.sh)
- [include/mm.h](include/mm.h)
- [include/sync.h](include/sync.h)
- [src/boot.S](src/boot.S)
- [src/kernel.c](src/kernel.c)
- [src/sync.S](src/sync.S)

验证方式：

```bash
make run
```

输出示例：

```text
Hello, from processor 0
Hello, from processor 3
Hello, from processor 2
Hello, from processor 1
```

说明：

输出顺序不固定是正常的，因为 4 个 core 并发执行。只要 0、1、2、3 都出现，并且每一行没有字符交错，就说明多核启动和 UART 输出同步工作正常。
