# MiniUART 中断回显流程说明

这份文档说明本次 MiniUART interrupt 练习新增了哪些流程。重点是：`kernel_main()` 不再主动轮询 `uart_recv()`，用户输入字符后，由 MiniUART RX interrupt 触发 IRQ handler 来读取并回显字符。

## 0. 总体变化

原来的回显方式是主循环主动等待输入：

```c
while (1) {
	uart_send(uart_recv());
}
```

现在改成空循环：

```c
while (1) {
}
```

对应代码在 [`src/kernel.c`](src/kernel.c#L32)。

新的处理链路是：

```text
用户输入字符
  -> MiniUART RX FIFO 有数据
  -> MiniUART 产生 RX interrupt
  -> AUX 外设块 pending
  -> ARM interrupt controller pending
  -> CPU 进入 IRQ exception
  -> el1_irq -> handle_irq()
  -> handle_irq() 分发到 handle_uart_irq()
  -> handle_uart_irq() 读取字符并回显
```

## 1. 用户输入字符，MiniUART RX FIFO 有数据

当用户在 QEMU 终端输入字符时，字符会进入 MiniUART 的 RX FIFO。代码里用 `AUX_MU_LSR_REG` 的 data-ready bit 判断 RX FIFO 是否有数据。

新增的 bit 定义在 [`include/peripherals/mini_uart.h`](include/peripherals/mini_uart.h#L28)：

```c
#define AUX_MU_LSR_DATA_READY  (1 << 0)
```

后续 handler 会用它判断是否可以读取字符：

[`src/mini_uart.c`](src/mini_uart.c#L30)

```c
while (get32(AUX_MU_LSR_REG) & AUX_MU_LSR_DATA_READY) {
	uart_send(get32(AUX_MU_IO_REG) & 0xFF);
}
```

## 2. MiniUART 产生 RX interrupt

MiniUART 本身要被配置成“收到字符就产生 RX interrupt”。这个开关在 `AUX_MU_IER_REG`。

新增 RX interrupt bit 定义在 [`include/peripherals/mini_uart.h`](include/peripherals/mini_uart.h#L21)：

```c
#define AUX_MU_IER_RX_IRQ      (1 << 0)
```

初始化时打开 RX interrupt：

[`src/mini_uart.c`](src/mini_uart.c#L57)

```c
put32(AUX_MU_IER_REG,AUX_MU_IER_RX_IRQ);
```

这一步的意思是：

```text
MiniUART RX FIFO 收到新字符后，MiniUART 可以向外产生接收中断。
```

## 3. AUX 外设块 pending

MiniUART 属于 AUX 外设块。MiniUART 产生中断后，AUX 外设块会在 AUX IRQ pending 寄存器里记录“MiniUART 有中断”。

新增 AUX IRQ 寄存器定义：

[`include/peripherals/mini_uart.h`](include/peripherals/mini_uart.h#L6)

```c
#define AUX_IRQ_REG     (PBASE+0x00215000)
```

新增 MiniUART pending bit：

[`include/peripherals/mini_uart.h`](include/peripherals/mini_uart.h#L19)

```c
#define AUX_IRQ_MINI_UART      (1 << 0)
```

handler 里会先确认 AUX pending 的确来自 MiniUART：

[`src/mini_uart.c`](src/mini_uart.c#L23)

```c
if (!(get32(AUX_IRQ_REG) & AUX_IRQ_MINI_UART)) {
	return;
}
```

## 4. ARM interrupt controller pending

AUX 外设块 pending 之后，还需要 ARM interrupt controller 允许 AUX IRQ 进入 CPU。

新增 AUX pending bit 定义在 [`include/peripherals/irq.h`](include/peripherals/irq.h#L17)：

```c
#define IRQ_PENDING_1_AUX	(1 << 29)
```

在 [`src/irq.c`](src/irq.c#L29) 的 `enable_interrupt_controller()` 里打开 AUX IRQ：

```c
put32(ENABLE_IRQS_1, IRQ_PENDING_1_AUX);
```

同一个函数里仍然保留 local timer IRQ 的打开：

```c
put32(TIMER_INT_CTRL_0, TIMER_INT_CTRL_0_VALUE);
```

所以当前 interrupt controller 同时允许：

```text
local timer interrupt
MiniUART/AUX interrupt
```

## 5. CPU 进入 IRQ exception

CPU 能进入 IRQ exception，依赖 exp3 原本已经建立好的异常向量流程。

`kernel_main()` 中会注册异常向量表：

[`src/kernel.c`](src/kernel.c#L14)

```c
irq_vector_init();
```

`irq_vector_init()` 把 `vectors` 写入 `vbar_el1`：

[`src/irq.S`](src/irq.S#L5)

```asm
irq_vector_init:
	adr	x0, vectors
	msr	vbar_el1, x0
	ret
```

最后 `enable_irq()` 打开 CPU 对 IRQ 的响应：

[`src/kernel.c`](src/kernel.c#L17)

```c
enable_irq();
```

这部分不是本练习新加的，但 MiniUART interrupt 依赖这条已有 IRQ 基础设施。

## 6. `el1_irq -> handle_irq()`

IRQ 发生后，CPU 根据 `vbar_el1` 跳进异常向量表。当前 kernel 运行在 EL1h，所以会进入 `el1_irq`。

入口在 [`src/entry.S`](src/entry.S#L177)：

```asm
el1_irq:
	kernel_entry
	bl	handle_irq
	kernel_exit
```

这里的作用是：

```text
保存寄存器现场
  -> 调用 C 层 handle_irq()
  -> 恢复寄存器现场
  -> eret 返回被中断的位置
```

这部分也是 exp3 原有 IRQ 框架，本练习是在 `handle_irq()` 里新增 MiniUART 分发。

## 7. `handle_irq()` 分发到 `handle_uart_irq()`

本次修改了 [`src/irq.c:handle_irq`](src/irq.c#L40)，让它同时处理 local timer IRQ 和 MiniUART/AUX IRQ。

首先读取两个 pending 来源：

```c
unsigned int local_irq = get32(INT_SOURCE_0);
unsigned int irq_pending_1 = get32(IRQ_PENDING_1);
```

local timer 仍然走原来的处理：

```c
if (local_irq & GENERIC_TIMER_INTERRUPT) {
	handle_local_timer_irq();
	handled = 1;
}
```

新增 MiniUART/AUX 分发：

```c
if (irq_pending_1 & IRQ_PENDING_1_AUX) {
	handle_uart_irq();
	handled = 1;
}
```

所以现在 `handle_irq()` 的职责变成：

```text
local timer IRQ -> handle_local_timer_irq()
MiniUART IRQ    -> handle_uart_irq()
```

## 8. `handle_uart_irq()` 读取字符并回显

新增的 MiniUART IRQ handler 在 [`src/mini_uart.c`](src/mini_uart.c#L23)：

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

它做两件事：

1. 先确认 AUX pending 里确实是 MiniUART。
2. 只要 RX FIFO 还有字符，就不断读取 `AUX_MU_IO_REG` 并用 `uart_send()` 回显。

函数声明加在 [`include/mini_uart.h`](include/mini_uart.h#L7)：

```c
void handle_uart_irq(void);
```

## 9. 为什么这证明不是主循环轮询

因为 `kernel_main()` 最后的循环已经变成空循环：

[`src/kernel.c`](src/kernel.c#L32)

```c
while (1) {
}
```

如果输入字符还能回显，就说明字符处理路径不是：

```text
while loop -> uart_recv() -> uart_send()
```

而是：

```text
MiniUART RX interrupt -> handle_irq() -> handle_uart_irq()
```

## 10. 当前完整流程

```text
用户输入字符
  -> MiniUART RX FIFO 有数据
  -> AUX_MU_IER_REG 允许 RX interrupt
  -> MiniUART 产生 RX interrupt
  -> AUX_IRQ_REG 记录 AUX_IRQ_MINI_UART
  -> IRQ_PENDING_1 记录 IRQ_PENDING_1_AUX
  -> CPU 响应 IRQ
  -> vectors 中的 EL1h IRQ 入口
  -> el1_irq
  -> kernel_entry 保存现场
  -> handle_irq()
       -> 检查 IRQ_PENDING_1_AUX
       -> handle_uart_irq()
            -> 检查 AUX_IRQ_MINI_UART
            -> 读取 AUX_MU_IO_REG
            -> uart_send() 回显
  -> kernel_exit 恢复现场
  -> eret 返回空循环
```

## 11. 验证方式

构建：

```bash
make
```

运行：

```bash
make run
```

进入 QEMU 后输入字符。如果字符能回显，说明 MiniUART RX interrupt handler 正在工作。

我也用非交互方式验证过一次：

```bash
(sleep 2; printf Z; sleep 2) | timeout 5s make run
```

输出中能看到 `Z` 出现在 timer interrupt 日志之间，说明字符由中断路径处理，而不是由主循环轮询处理。
