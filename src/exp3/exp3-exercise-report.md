# exp3 中断实验报告

## 目标

本次在 `exp3` 中完成两个中断相关练习：

1. 使用 local/generic timer 产生 processor interrupt。
2. 使用 MiniUART RX interrupt 处理用户输入，并将 `kernel_main()` 最后的轮询 echo 改为空循环。

最终效果：

```text
timer 能周期性触发中断；
用户输入字符后，由 MiniUART 中断处理函数回显字符。
```

## 1. local/generic timer 修改

修改位置：

- [src/kernel.c](src/kernel.c)
- [src/timer.c](src/timer.c)
- [src/timer.S](src/timer.S)
- [include/timer.h](include/timer.h)
- [src/irq.c](src/irq.c)

在 [src/kernel.c](src/kernel.c#L14) 中，初始化流程为：

```c
irq_vector_init();
local_timer_init();
enable_interrupt_controller();
enable_irq();
```

`local_timer_init()` 位于 [src/timer.c](src/timer.c#L18)，负责读取 `cntfrq_el0`、启动 timer，并设置第一次到期时间。

底层寄存器操作在 [src/timer.S](src/timer.S#L12)：

```asm
msr CNTP_CTL_EL0, x0
msr CNTP_TVAL_EL0, x0
```

其中：

- `CNTP_CTL_EL0`：开启 EL1 physical timer。
- `CNTP_TVAL_EL0`：设置 timer 到期时间。

timer 到期后，硬件产生中断，进入 `el1_irq -> handle_irq()`。在 [src/irq.c](src/irq.c#L40) 中检查 `INT_SOURCE_0`：

```c
if (local_irq & GENERIC_TIMER_INTERRUPT) {
	handle_local_timer_irq();
}
```

`handle_local_timer_irq()` 会打印信息并重新设置下一次 timer。

## 2. MiniUART RX interrupt 修改

修改位置：

- [include/peripherals/mini_uart.h](include/peripherals/mini_uart.h)
- [include/peripherals/irq.h](include/peripherals/irq.h)
- [include/mini_uart.h](include/mini_uart.h)
- [src/mini_uart.c](src/mini_uart.c)
- [src/irq.c](src/irq.c)
- [src/kernel.c](src/kernel.c)

### 2.1 打开 MiniUART RX interrupt

在 [include/peripherals/mini_uart.h](include/peripherals/mini_uart.h#L6) 中增加 AUX/MiniUART 中断相关定义，例如：

```c
#define AUX_IRQ_REG     (PBASE+0x00215000)
#define AUX_IRQ_MINI_UART      (1 << 0)
#define AUX_MU_IER_RX_IRQ      (1 << 0)
#define AUX_MU_LSR_DATA_READY  (1 << 0)
```

在 [src/mini_uart.c](src/mini_uart.c#L57) 中，将 MiniUART 初始化改为打开 RX interrupt：

```c
put32(AUX_MU_IER_REG,AUX_MU_IER_RX_IRQ);
```

### 2.2 打开 ARM interrupt controller 中的 AUX IRQ

在 [include/peripherals/irq.h](include/peripherals/irq.h#L17) 中增加：

```c
#define IRQ_PENDING_1_AUX	(1 << 29)
```

在 [src/irq.c](src/irq.c#L29) 中打开 AUX IRQ：

```c
put32(ENABLE_IRQS_1, IRQ_PENDING_1_AUX);
```

### 2.3 新增 MiniUART IRQ handler

在 [src/mini_uart.c](src/mini_uart.c#L23) 中新增：

```c
void handle_uart_irq(void)
{
	if (!(get32(AUX_IRQ_REG) & AUX_IRQ_MINI_UART)) {
		return;
	}

	while (get32(AUX_MU_LSR_REG) & AUX_MU_LSR_DATA_READY) {
		uart_send(get32(AUX_MU_IO_REG) & 0xFF);
	}
}
```

它负责从 RX FIFO 读取新字符，并立即回显。

### 2.4 在 `handle_irq()` 中分发 MiniUART 中断

在 [src/irq.c](src/irq.c#L40) 中，`handle_irq()` 同时处理 timer 和 MiniUART：

```c
if (local_irq & GENERIC_TIMER_INTERRUPT) {
	handle_local_timer_irq();
}

if (irq_pending_1 & IRQ_PENDING_1_AUX) {
	handle_uart_irq();
}
```

### 2.5 主循环改为空循环

原来的 `kernel_main()` 通过轮询回显：

```c
while (1) {
	uart_send(uart_recv());
}
```

现在在 [src/kernel.c](src/kernel.c#L32) 中改为：

```c
while (1) {
}
```

因此字符回显不再依赖主循环，而是由 MiniUART RX interrupt 完成。

## 3. 整体处理流程

```text
用户输入字符
  -> MiniUART RX FIFO 有数据
  -> MiniUART 产生 RX interrupt
  -> AUX_IRQ_REG 标记 MiniUART pending
  -> IRQ_PENDING_1 标记 AUX pending
  -> CPU 进入 IRQ exception
  -> el1_irq -> handle_irq()
  -> handle_irq() 调用 handle_uart_irq()
  -> handle_uart_irq() 读取字符并回显
```

## 4. 验证结果

编译：

```bash
make
```

运行 timer 验证：

```bash
timeout 4s make run
```

观察到：

```text
kernel boots...
System count freq (CNTFRQ) is: 62500000
interval is set to: 67108864
Timer interrupt received. next in 67108864 ticks
```

MiniUART RX interrupt 验证：

```bash
(sleep 2; printf Z; sleep 2) | timeout 5s make run
```

输出中出现：

```text
ZTimer interrupt received...
```

由于 `kernel_main()` 已经是空循环，`Z` 能回显说明 MiniUART RX interrupt handler 工作正常。

## 结论

本次实验完成了 exp3 的中断扩展：

- timer 使用 local/generic timer，通过 `CNTP_CTL_EL0` 和 `CNTP_TVAL_EL0` 产生周期性中断。
- MiniUART 打开 RX interrupt，用户输入字符后由 `handle_uart_irq()` 读取并回显。
- `kernel_main()` 不再主动轮询 UART，输入处理变成中断驱动。
