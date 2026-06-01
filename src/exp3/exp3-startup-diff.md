# exp3 启动流程介绍：对比 exp2 新增了什么

这份文档按 CPU 的实际启动顺序来读：先从 `_start` 进入 EL1，再进入 `kernel_main()`，然后配置异常向量、timer 和 IRQ，最后说明 timer 到期后硬件如何进入中断处理流程。

重点结论：

```text
exp3 没有大改早期 boot.S 启动骨架。
exp3 真正新增的是 kernel_main() 之后的异常向量表、timer、中断控制器和 IRQ handler。
```

## 0. 总体对比

exp2 的流程：

```text
_start
  -> 只让 core 0 运行
  -> 从 EL2/EL3 切到 EL1
  -> 清 BSS
  -> 设置 sp
  -> kernel_main()
       -> uart_init()
       -> init_printf()
       -> 打印当前 EL
       -> UART echo loop
```

exp3 的流程：

```text
_start
  -> 只让 core 0 运行
  -> 从 EL2/EL3 切到 EL1
  -> 清 BSS
  -> 设置 sp
  -> kernel_main()
       -> uart_init()
       -> init_printf()
       -> irq_vector_init()
       -> local_timer_init()
       -> enable_interrupt_controller()
       -> enable_irq()
       -> UART echo loop

timer 到期后：
  -> 硬件产生 IRQ
  -> CPU 根据 VBAR_EL1 找到 vectors
  -> 进入 el1_irq
  -> handle_irq()
  -> handle_local_timer_irq()
  -> 重新设置下一次 timer
  -> eret 返回原来的 echo loop
```

## 1. `_start`：只让 core 0 继续运行

exp2 从 [`../exp2/src/boot.S:_start`](../exp2/src/boot.S#L8) 开始，读取 `mpidr_el1` 判断当前 CPU core：

- [`mrs x0, mpidr_el1`](../exp2/src/boot.S#L9)
- [`and x0, x0, #0xFF`](../exp2/src/boot.S#L10)
- [`cbz x0, master`](../exp2/src/boot.S#L11)
- [非 core 0 进入 `proc_hang`](../exp2/src/boot.S#L14)

exp3 这里基本一样：

- [`mrs x0, mpidr_el1`](src/boot.S#L9)
- [`and x0, x0, #0xFF`](src/boot.S#L10)
- [`cbz x0, master`](src/boot.S#L11)
- [非 core 0 进入 `proc_hang`](src/boot.S#L14)

这一阶段 exp3 没有新增中断相关内容。它仍然先保证只有一个 CPU core 跑内核启动代码。

## 2. `master`：配置 EL1/EL2 基础寄存器

exp2 在 [`../exp2/src/boot.S:master`](../exp2/src/boot.S#L17) 里配置：

- [`sctlr_el1`](../exp2/src/boot.S#L18)
- [`hcr_el2`](../exp2/src/boot.S#L21)

exp3 也一样，在 [`src/boot.S:master`](src/boot.S#L17)：

- [`sctlr_el1`](src/boot.S#L18)
- [`hcr_el2`](src/boot.S#L21)

这些值来自 [`include/arm/sysregs.h`](include/arm/sysregs.h#L1)：

- [`SCTLR_VALUE_MMU_DISABLED`](include/arm/sysregs.h#L16)：让 EL1 在 MMU/cache 关闭、小端模式下启动。
- [`HCR_VALUE`](include/arm/sysregs.h#L23)：设置 `HCR_RW`，让下一层 EL1 使用 AArch64。

这一阶段 exp3 也没有本质新增。它还是在准备从当前异常级别进入 EL1。

## 3. 从 EL2/EL3 进入 EL1

在 QEMU 下，exp2 从 EL2 进入 EL1：

- [写 `spsr_el2`](../exp2/src/boot.S#L27)
- [写 `elr_el2`](../exp2/src/boot.S#L44)
- [`eret`](../exp2/src/boot.S#L57)

exp3 同样从 EL2 进入 EL1：

- [写 `spsr_el2`](src/boot.S#L25)
- [写 `elr_el2`](src/boot.S#L28)
- [`eret`](src/boot.S#L42)

如果是真 RPi3 硬件，exp3 会走 `#else` 分支：

- [写 `scr_el3`](src/boot.S#L32)
- [写 `spsr_el3`](src/boot.S#L35)
- [写 `elr_el3`](src/boot.S#L38)
- [`eret`](src/boot.S#L42)

这些值也在 [`include/arm/sysregs.h`](include/arm/sysregs.h#L25)：

- [`SCR_VALUE`](include/arm/sysregs.h#L32)：让 lower EL 进入 Non-secure AArch64。
- [`SPSR_VALUE`](include/arm/sysregs.h#L40)：让 `eret` 后进入 EL1h，并先屏蔽异常/中断。

这一阶段的关键理解：

```text
QEMU: 当前 EL2 -> eret -> EL1h
RPi3: 当前 EL3 -> eret -> Non-secure EL1h
```

所以 exp3 和 exp2 在“如何进入 EL1”这件事上仍然基本一致。

## 4. `el1_entry`：清 BSS，设置栈，进入 C 代码

exp2 进入 EL1 后，会做运行 C 代码前的最低限度准备：

- [清 BSS](../exp2/src/boot.S#L59)
- [设置 `sp`](../exp2/src/boot.S#L70)
- [调用 `kernel_main`](../exp2/src/boot.S#L71)

exp3 同样做这些事情：

- [清 BSS](src/boot.S#L44)
- [设置 `sp`](src/boot.S#L50)
- [调用 `kernel_main`](src/boot.S#L51)

这里的 `bss_begin` / `bss_end` 来自 linker script：

- [`src/linker-qemu.ld`](src/linker-qemu.ld#L1)

`LOW_MEMORY` 来自：

- [`include/mm.h`](include/mm.h#L1)

这一阶段也不是 exp3 的主要变化点。也就是说，到 `kernel_main()` 之前，exp3 和 exp2 的启动路径可以看成同一类骨架。

## 5. `kernel_main()`：从这里开始，exp3 明显不同

exp2 的 C 入口在 [`../exp2/src/kernel.c:kernel_main`](../exp2/src/kernel.c#L5)。它做的是：

- [初始化 UART](../exp2/src/kernel.c#L7)
- [初始化 printf](../exp2/src/kernel.c#L8)
- [读取并打印当前 EL](../exp2/src/kernel.c#L9)
- [进入 UART echo loop](../exp2/src/kernel.c#L12)

exp3 的 C 入口在 [`src/kernel.c:kernel_main`](src/kernel.c#L8)。它仍然保留 UART 初始化：

- [初始化 UART](src/kernel.c#L10)
- [初始化 printf](src/kernel.c#L11)
- [打印 `kernel boots...`](src/kernel.c#L12)

然后 exp3 新增了四个关键步骤：

- [`irq_vector_init()`](src/kernel.c#L14)
- [`local_timer_init()`](src/kernel.c#L15)
- [`enable_interrupt_controller()`](src/kernel.c#L16)
- [`enable_irq()`](src/kernel.c#L17)

这四步就是 exp3 相比 exp2 的核心新增启动流程。

## 6. 新增步骤一：注册异常向量表 `irq_vector_init()`

exp3 在 [`src/kernel.c`](src/kernel.c#L14) 调用：

```c
irq_vector_init();
```

它的实现位于 [`src/irq.S:irq_vector_init`](src/irq.S#L5)：

- [取 `vectors` 地址](src/irq.S#L6)
- [写入 `vbar_el1`](src/irq.S#L7)

`vectors` 定义在 [`src/entry.S`](src/entry.S#L88)：

- [`vectors:`](src/entry.S#L90)
- [EL1h IRQ 入口是 `ventry el1_irq`](src/entry.S#L100)

这一步的作用：

```text
告诉 CPU：
以后 EL1 发生异常/中断时，到 vectors 这张表里找入口。
```

exp2 没有 `vectors`，也没有写 `vbar_el1`。所以 exp2 没有建立正式的 IRQ/异常入口。

## 7. 新增步骤二：初始化 local/generic timer

exp3 在 [`src/kernel.c`](src/kernel.c#L15) 调用：

```c
local_timer_init();
```

实现位于 [`src/timer.c:local_timer_init`](src/timer.c#L18)：

- [读取 `cntfrq_el0`](src/timer.c#L11)
- [调用 `local_timer_enable()`](src/timer.c#L24)
- [调用 `local_timer_reset(interval)`](src/timer.c#L25)

底层汇编在 [`src/timer.S`](src/timer.S#L12)：

- [`CNTP_CTL_EL0 = 1`](src/timer.S#L19)：打开 EL1 physical timer，并不屏蔽 timer interrupt。
- [`CNTP_TVAL_EL0 = interval`](src/timer.S#L29)：设置 timer 多久后到期。

这一点是新手最容易混淆的地方：

```text
CNTP_TVAL_EL0 只是设置“多久以后到期”。
真正到期后产生 IRQ，是硬件 timer 自动完成的，不是某一行代码主动调用 handler。
```

exp2 没有 local/generic timer 初始化，因此不会周期性产生 timer IRQ。

## 8. 新增步骤三：打开 RPi per-core timer interrupt

exp3 在 [`src/kernel.c`](src/kernel.c#L16) 调用：

```c
enable_interrupt_controller();
```

实现位于 [`src/irq.c`](src/irq.c#L30)：

- [写 `TIMER_INT_CTRL_0`](src/irq.c#L36)

相关寄存器定义在 [`include/peripherals/irq.h`](include/peripherals/irq.h#L25)：

- [`TIMER_INT_CTRL_0`](include/peripherals/irq.h#L25)
- [`INT_SOURCE_0`](include/peripherals/irq.h#L26)
- [`TIMER_INT_CTRL_0_VALUE`](include/peripherals/irq.h#L28)
- [`GENERIC_TIMER_INTERRUPT`](include/peripherals/irq.h#L29)

这一步的作用：

```text
允许 Core 0 接收 generic timer 的中断信号。
```

timer 自己到期是一回事，中断信号能不能送到当前 CPU core 又是另一回事。exp3 新增了这道通路配置，exp2 没有。

## 9. 新增步骤四：打开 CPU IRQ 响应

exp3 在 [`src/kernel.c`](src/kernel.c#L17) 调用：

```c
enable_irq();
```

实现位于 [`src/irq.S`](src/irq.S#L17)：

- [`msr daifclr, #2`](src/irq.S#L19)

这一步的作用：

```text
清除 PSTATE.DAIF 里的 I bit，让 CPU 开始响应 IRQ。
```

注意前面 [`SPSR_VALUE`](include/arm/sysregs.h#L40) 进入 EL1 时会先屏蔽中断。exp3 等异常向量表、timer 和 interrupt controller 都配置好之后，才在这里真正打开 IRQ。

exp2 没有这一步，因此不会响应 IRQ。

## 10. 主循环仍然是 UART echo

exp3 最后仍然进入 UART echo loop：

- [`while (1)`](src/kernel.c#L32)
- [`uart_send(uart_recv())`](src/kernel.c#L33)

这和 exp2 的 echo loop 很像：

- [`while (1)`](../exp2/src/kernel.c#L12)
- [`uart_send(uart_recv())`](../exp2/src/kernel.c#L13)

区别在于：

```text
exp2 的 echo loop 只会等待串口输入。
exp3 的 echo loop 会被 timer IRQ 周期性打断。
```

## 11. timer 到期后：硬件自动触发 IRQ

当 [`CNTP_TVAL_EL0`](src/timer.S#L29) 设置的倒计时到 0 后，generic timer 硬件产生 IRQ。这个“到期触发”的动作不体现在某一行代码中，它是硬件行为。

如果前面几步都配置好了：

- [timer 已启用](src/timer.S#L19)
- [timer 到期时间已设置](src/timer.S#L29)
- [Core 0 timer interrupt 已打开](src/irq.c#L36)
- [CPU IRQ mask 已清除](src/irq.S#L19)
- [VBAR_EL1 已指向 vectors](src/irq.S#L7)

那么 CPU 会进入异常向量表。

当前 kernel 运行在 EL1h，所以 timer IRQ 会进入：

- [`vectors` 中的 EL1h IRQ 槽位](src/entry.S#L100)
- [`el1_irq`](src/entry.S#L177)

`el1_irq` 的流程：

- [`kernel_entry`](src/entry.S#L178)：保存 x0-x30 等寄存器现场。
- [`bl handle_irq`](src/entry.S#L179)：进入 C 层中断分发。
- [`kernel_exit`](src/entry.S#L180)：恢复现场并 `eret` 返回。

`kernel_entry` 宏定义在 [`src/entry.S`](src/entry.S#L31)，`kernel_exit` 宏定义在 [`src/entry.S`](src/entry.S#L55)。

## 12. `handle_irq()`：判断是哪种 IRQ

exp3 的 C 层 IRQ handler 在 [`src/irq.c:handle_irq`](src/irq.c#L44)：

- [读取 `INT_SOURCE_0`](src/irq.c#L49)
- [判断是否为 `GENERIC_TIMER_INTERRUPT`](src/irq.c#L51)
- [调用 `handle_local_timer_irq()`](src/irq.c#L52)

也就是说，`el1_irq` 是统一入口，`handle_irq()` 负责分发具体设备中断。

exp2 没有这个分发函数。

## 13. `handle_local_timer_irq()`：处理 timer，并设置下一次中断

timer IRQ 最后进入 [`src/timer.c:handle_local_timer_irq`](src/timer.c#L29)：

- [打印 timer interrupt 信息](src/timer.c#L31)
- [重新写 `CNTP_TVAL_EL0`](src/timer.c#L31)

重新写 `CNTP_TVAL_EL0` 是通过 [`local_timer_reset(interval)`](src/timer.S#L23) 完成的。

这一步很重要：

```text
timer 到期后，如果不重新设置 CNTP_TVAL_EL0，就不会按当前代码的方式继续产生下一次周期性 timer IRQ。
```

所以 exp3 的 timer 是这样周期性工作的：

```text
设置 CNTP_TVAL_EL0
  -> 硬件倒计时到 0
  -> 产生 IRQ
  -> handle_local_timer_irq()
  -> 再次设置 CNTP_TVAL_EL0
  -> 下一轮倒计时
```

## 14. 按启动流程看，exp3 到底比 exp2 增加了什么

| 启动阶段 | exp2 | exp3 新增 |
| --- | --- | --- |
| `_start` 选 core 0 | 有 | 基本无变化 |
| 配置 `sctlr_el1` / `hcr_el2` | 有 | 基本无变化 |
| `eret` 进入 EL1 | 有 | 基本无变化 |
| 清 BSS、设置栈 | 有 | 基本无变化 |
| UART / printf | 有 | 保留 |
| 异常向量表 | 无 | 新增 [`vectors`](src/entry.S#L90) |
| 注册异常向量表 | 无 | 新增 [`vbar_el1 = vectors`](src/irq.S#L7) |
| local/generic timer | 无 | 新增 [`local_timer_init()`](src/timer.c#L18) |
| timer 控制寄存器 | 无 | 新增 [`CNTP_CTL_EL0`](src/timer.S#L20) |
| timer 到期值 | 无 | 新增 [`CNTP_TVAL_EL0`](src/timer.S#L29) |
| RPi Core 0 timer IRQ | 无 | 新增 [`TIMER_INT_CTRL_0`](src/irq.c#L36) |
| CPU IRQ 开关 | 无 | 新增 [`enable_irq()`](src/irq.S#L17) |
| IRQ 汇编入口 | 无 | 新增 [`el1_irq`](src/entry.S#L177) |
| IRQ C 层分发 | 无 | 新增 [`handle_irq()`](src/irq.c#L44) |
| timer IRQ handler | 无 | 新增 [`handle_local_timer_irq()`](src/timer.c#L29) |

## 15. 最终记忆版

如果只记一条线，可以这样记：

```text
exp2:
启动到 EL1 -> 跑 C -> UART echo

exp3:
启动到 EL1 -> 跑 C
  -> 注册异常向量表
  -> 启动 generic timer
  -> 打开 timer interrupt controller
  -> 打开 CPU IRQ
  -> UART echo
  -> timer 到期后硬件触发 IRQ
  -> vectors/el1_irq
  -> handle_irq
  -> handle_local_timer_irq
  -> 重设 timer
  -> eret 回到 echo
```

所以，exp3 的变化不是“早期启动怎么进入 EL1”变了，而是：

```text
进入 kernel_main() 以后，内核开始具备处理中断的能力。
```
