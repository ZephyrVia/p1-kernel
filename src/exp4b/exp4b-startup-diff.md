# exp4b 启动流程介绍：对比 exp4a 新增了什么

这份文档按代码真正运行的顺序来读：先从 `_start` 进入 EL1，再进入 `kernel_main()`，创建任务，打开 timer interrupt，最后说明 timer 中断如何触发 `timer_tick()`，并把 exp4a 的协作式调度推进到 exp4b 的抢占式调度。

重点结论：

```text
exp4a: 任务自己调用 schedule() 主动让出 CPU。
exp4b: timer interrupt 周期性打断当前任务，counter 用完后由内核主动切换任务。

所以 exp4b 比 exp4a 多了：
1. enable_irq()
2. timer_tick()
3. preempt_count / preempt_disable() / preempt_enable()
4. 中断入口保存和恢复 elr_el1 / spsr_el1
```

## 0. 总体对比

exp4a 的主流程：

```text
_start
  -> 进入 EL1
  -> 清 BSS
  -> 设置 sp
  -> kernel_main()
       -> uart_init()
       -> irq_vector_init()
       -> generic_timer_init()
       -> enable_interrupt_controller()
       -> disable_irq()
       -> copy_process(process, "12345")
       -> copy_process(process, "abcde")
       -> while (1) schedule()

process()
  -> 打印 5 个字符
  -> 主动 schedule()
  -> 切到另一个任务
```

exp4b 的主流程：

```text
_start
  -> 进入 EL1
  -> 清 BSS
  -> 设置 sp
  -> kernel_main()
       -> uart_init()
       -> irq_vector_init()
       -> generic_timer_init()
       -> enable_interrupt_controller()
       -> enable_irq()
       -> copy_process(process, "12345")
       -> copy_process(process2, "abcde")
       -> while (1) schedule()

process()
  -> 一直打印字符，不主动 schedule()

timer interrupt 到期：
  -> CPU 进入 el1_irq
  -> handle_irq()
  -> handle_generic_timer_irq()
  -> timer_tick()
  -> current->counter--
  -> counter 到 0 后 _schedule()
  -> switch_to(next)
  -> cpu_switch_to(prev, next)
  -> eret 返回到新任务或旧任务的执行位置
```

可以这样记：

```text
exp4a 是 cooperative scheduling：任务主动 yield。
exp4b 是 preemptive scheduling：timer 到点后内核抢占。
```

## 1. `_start`：早期启动流程基本沿用 exp4a

exp4b 仍然从 [`src/boot.S:_start`](src/boot.S#L8) 开始。

第一步仍然只让 core 0 继续运行：

- [`mrs x0, mpidr_el1`](src/boot.S#L9)：读取当前 CPU core id。
- [`and x0, x0,#0xFF`](src/boot.S#L10)：取出低 8 位。
- [`cbz x0, master`](src/boot.S#L11)：如果是 core 0，就进入 `master`。
- [其他 core 进入 `proc_hang`](src/boot.S#L14)。

所以 exp4b 的抢占式调度不是多核并行。它仍然是在一个 CPU core 上，让多个任务轮流使用 CPU。

## 2. `master`：进入 EL1 的流程也基本不变

在 [`src/boot.S:master`](src/boot.S#L17)，exp4b 仍然配置：

- [`sctlr_el1`](src/boot.S#L18)：使用 `SCTLR_VALUE_MMU_DISABLED`，暂时关闭 MMU/cache。
- [`hcr_el2`](src/boot.S#L21)：使用 `HCR_VALUE`，让 EL1 使用 AArch64。

QEMU 下从 EL2 进入 EL1：

- [写 `spsr_el2`](src/boot.S#L25)
- [写 `elr_el2 = el1_entry`](src/boot.S#L28)
- [`eret`](src/boot.S#L41)

真 RPi3 硬件下从 EL3 进入 EL1：

- [写 `scr_el3`](src/boot.S#L31)
- [写 `spsr_el3`](src/boot.S#L34)
- [写 `elr_el3 = el1_entry`](src/boot.S#L37)
- [`eret`](src/boot.S#L41)

这部分和 exp4a 没有本质变化。调度相关变化发生在后面的中断和 scheduler 代码里。

## 3. `el1_entry`：清 BSS，设置栈，进入 `kernel_main()`

进入 EL1 后，exp4b 在 [`src/boot.S:el1_entry`](src/boot.S#L43) 做三件事：

- [清 BSS](src/boot.S#L44)：调用 `memzero()`。
- [设置 `sp = LOW_MEMORY`](src/boot.S#L49)：设置初始内核栈。
- [调用 `kernel_main`](src/boot.S#L50)：进入 C 代码。

到这里为止，exp4b 和 exp4a 的启动骨架基本一致。

## 4. `kernel_main()`：exp4b 开始打开 IRQ

exp4b 的 C 入口在 [`src/kernel.c:kernel_main`](src/kernel.c#L27)。

前面仍然是 UART 和 printf 初始化：

- [初始化 UART](src/kernel.c#L29)
- [初始化 printf](src/kernel.c#L30)
- [打印 `kernel boots`](src/kernel.c#L32)

然后初始化异常向量和 timer：

- [`irq_vector_init()`](src/kernel.c#L34)
- [`generic_timer_init()`](src/kernel.c#L35)
- [`enable_interrupt_controller()`](src/kernel.c#L36)
- [`enable_irq()`](src/kernel.c#L37)

这里是 exp4b 和 exp4a 的第一个关键差异：

```text
exp4a: disable_irq()
exp4b: enable_irq()
```

因为 exp4b 要靠 timer interrupt 打断当前任务，所以必须打开 IRQ。

## 5. `generic_timer_init()`：打开 timer，并设置第一次到期时间

`generic_timer_init()` 在 [`src/timer.c`](src/timer.c#L17)。

exp4b 里它做了三件事：

```c
printf("interval is set to: %u\r\n", interval);
gen_timer_init();
gen_timer_reset(interval);
```

其中：

- [`gen_timer_init()`](src/timer.S#L13)：写 `CNTP_CTL_EL0 = 1`，打开 EL1 physical timer。
- [`gen_timer_reset(interval)`](src/timer.S#L24)：写 `CNTP_TVAL_EL0 = interval`，设置下一次 timer 到期时间。

这比 exp4a 更完整：exp4a 只打开 timer，exp4b 还设置了第一次倒计时。

## 6. `enable_interrupt_controller()`：允许 core 0 接收 timer interrupt

`enable_interrupt_controller()` 在 [`src/irq.c`](src/irq.c#L25)。

它写：

```c
put32(TIMER_INT_CTRL_0, TIMER_INT_CTRL_0_VALUE);
```

意思是打开 core 0 的 generic timer interrupt 来源。

注意这里分两层：

```text
generic timer 自己要打开；
interrupt controller 也要允许这个 interrupt 送到 core 0；
CPU 本身还要 enable_irq() 清掉 DAIF 里的 I bit。
```

这三层都准备好后，timer 到期才会真的进入 IRQ handler。

## 7. `copy_process()`：创建两个可被抢占的任务

exp4b 在 [`kernel_main()`](src/kernel.c#L39) 创建两个任务：

```c
copy_process((unsigned long)&process, (unsigned long)"12345");
copy_process((unsigned long)&process2, (unsigned long)"abcde");
```

`copy_process()` 位于 [`src/fork.c`](src/fork.c#L5)。

主要流程：

1. [先调用 `preempt_disable()`](src/fork.c#L7)，避免创建任务过程中被抢占。
2. [调用 `get_free_page()` 分配一页内存](src/fork.c#L10)。
3. [把这一页底部当作 `task_struct`](src/fork.c#L10)。
4. [设置 `state = TASK_RUNNING`](src/fork.c#L14)。
5. [设置 `counter = priority`](src/fork.c#L15)。
6. [设置 `preempt_count = 1`](src/fork.c#L16)。
7. [设置 `cpu_context.x19 = fn`](src/fork.c#L18)。
8. [设置 `cpu_context.x20 = arg`](src/fork.c#L19)。
9. [设置 `cpu_context.pc = ret_from_fork`](src/fork.c#L20)。
10. [设置新任务栈顶](src/fork.c#L21)。
11. [把新任务放入 `task[]`](src/fork.c#L23)。
12. [调用 `preempt_enable()`](src/fork.c#L24)。

这里比 exp4a 多了两个和抢占相关的保护：

```text
copy_process() 自己用 preempt_disable()/preempt_enable() 保护创建过程；
新任务的 preempt_count 初始为 1，直到 schedule_tail() 里再打开抢占。
```

## 8. `task_struct`：counter 现在真的会被 timer tick 消耗

任务结构定义在 [`include/sched.h`](include/sched.h#L31)：

```c
struct task_struct {
    struct cpu_context cpu_context;
    long state;
    long counter;
    long priority;
    long preempt_count;
};
```

在 exp4a 中，`counter` 主要被 `schedule()` 设置为 0，用于主动让出 CPU。

在 exp4b 中，`counter` 的含义更接近时间片：

```text
timer interrupt 每来一次：
  current->counter--

counter 仍大于 0：
  当前任务继续运行

counter 减到 0：
  触发 _schedule()，换下一个任务
```

对应代码在 [`src/sched.c:timer_tick`](src/sched.c#L94)。

## 9. `preempt_count`：防止关键代码被抢占

exp4b 新增了两个函数：

- [`preempt_disable()`](src/sched.c#L9)
- [`preempt_enable()`](src/sched.c#L14)

它们操作的是：

```c
current->preempt_count
```

含义是：

```text
preempt_count == 0:
  当前任务可以被抢占

preempt_count > 0:
  当前任务正在临界区，timer_tick() 不能切走它
```

`timer_tick()` 里有这个判断：

```c
if (current->counter > 0 || current->preempt_count > 0)
    return;
```

位置在 [`src/sched.c`](src/sched.c#L97)。

这就是 exp4b 比 exp4a 更复杂的地方之一：一旦允许 timer 在任意位置打断任务，就必须避免在更新全局调度结构时被切走。

## 10. `process()`：任务本身不再主动调用 `schedule()`

exp4b 的 [`process()`](src/kernel.c#L9)：

```c
while (1) {
    for (int i = 0; i < 5; i++) {
        uart_send(array[i]);
        delay(5000000);
    }
}
```

`process2()` 在 [`src/kernel.c`](src/kernel.c#L19)，逻辑类似。

和 exp4a 对比，最重要的区别是：

```text
exp4a process() 末尾主动 schedule()
exp4b process() 里没有 schedule()
```

所以 exp4b 如果能看到两个任务轮流输出，说明切换不是任务自己让出的，而是 timer interrupt 抢占出来的。

## 11. 主循环里的 `schedule()`：启动第一批任务

创建任务之后，`kernel_main()` 进入：

```c
while (1) {
    schedule();
}
```

位置在 [`src/kernel.c`](src/kernel.c#L51)。

这个 `schedule()` 的作用主要是把 CPU 从初始 `init_task` 切到创建出来的新任务。

一旦切到 `process()` 或 `process2()`，任务自己不会再主动 `schedule()`。后面的切换就主要靠 timer interrupt。

## 12. `_schedule()`：选择下一个任务，但现在要防止被重入抢占

调度器主体是 [`src/sched.c:_schedule`](src/sched.c#L20)。

exp4b 一进入 `_schedule()` 就调用：

```c
preempt_disable();
```

位置在 [`src/sched.c`](src/sched.c#L25)。

原因是 `_schedule()` 会遍历和修改全局调度结构，比如：

- `task[]`
- `current`
- 每个任务的 `counter`

这些操作过程中不能被 timer interrupt 再次打断并重入调度。

然后 `_schedule()` 仍然沿用 exp4a 的选择逻辑：

```text
从 task[] 中找到 TASK_RUNNING 且 counter 最大的任务。
如果所有 counter 都是 0，就按 priority 给它们重新充值。
```

选择到任务后调用：

```c
switch_to(task[next]);
```

位置在 [`src/sched.c`](src/sched.c#L57)。

最后调用：

```c
preempt_enable();
```

位置在 [`src/sched.c`](src/sched.c#L58)。

## 13. `switch_to()` 和 `cpu_switch_to()`：上下文切换机制仍然类似 exp4a

[`switch_to()`](src/sched.c#L67) 做的事情仍然是：

```text
如果 next 就是 current，直接返回；
否则：
  prev = current
  current = next
  cpu_switch_to(prev, next)
```

真正切换寄存器的代码在 [`src/sched.S:cpu_switch_to`](src/sched.S#L5)。

它保存旧任务：

- 保存 `x19` 到 `x28`
- 保存 `x29`
- 保存旧任务的 `sp`
- 保存 `x30` 到旧任务的 `cpu_context.pc`

然后恢复新任务：

- 恢复新任务的 `x19` 到 `x28`
- 恢复新任务的 `x29`
- 恢复新任务的 `sp`
- 恢复新任务的 `x30`

最后执行：

```asm
ret
```

位置在 [`src/sched.S`](src/sched.S#L24)。

如果是第一次运行的新任务，`x30` 会是 `ret_from_fork`；如果是之前被切走的任务，`x30` 会是它上次离开 `cpu_switch_to()` 后应该继续执行的位置。

## 14. `ret_from_fork` 和 `schedule_tail()`：新任务第一次运行

新任务第一次被切换过去时，会从 [`ret_from_fork`](src/entry.S#L145) 开始。

流程：

```text
ret_from_fork
  -> schedule_tail()
  -> x0 = x20
  -> blr x19
  -> process(arg)
```

其中：

- `x19` 是 `copy_process()` 里保存的任务函数地址。
- `x20` 是 `copy_process()` 里保存的任务参数。

`schedule_tail()` 在 [`src/sched.c`](src/sched.c#L90)：

```c
void schedule_tail(void) {
    preempt_enable();
}
```

这和 `copy_process()` 里设置 `p->preempt_count = 1` 配合使用：

```text
新任务创建时先禁止抢占；
等它第一次真的开始运行，经过 schedule_tail() 后再允许抢占。
```

## 15. timer interrupt 到期后的完整路径

这是 exp4b 最核心的流程。

timer 到期后，硬件产生 IRQ。CPU 会根据 `VBAR_EL1` 找到异常向量表。

异常向量表在 [`src/entry.S:vectors`](src/entry.S#L74)。

EL1h IRQ 入口是：

- [`ventry el1_irq`](src/entry.S#L81)
- [`el1_irq`](src/entry.S#L140)

然后执行：

```asm
kernel_entry
bl handle_irq
kernel_exit
```

位置在 [`src/entry.S`](src/entry.S#L140)。

C 端的 [`handle_irq()`](src/irq.c#L34) 会读取：

```c
unsigned int irq = get32(INT_SOURCE_0);
```

如果是 generic timer interrupt，就调用：

```c
handle_generic_timer_irq();
```

位置在 [`src/irq.c`](src/irq.c#L38)。

`handle_generic_timer_irq()` 在 [`src/timer.c`](src/timer.c#L23)：

```c
gen_timer_reset(interval);
timer_tick();
```

也就是说：

```text
timer interrupt
  -> el1_irq
  -> handle_irq()
  -> handle_generic_timer_irq()
  -> gen_timer_reset(interval)
  -> timer_tick()
```

`gen_timer_reset(interval)` 负责设置下一次 timer 到期，`timer_tick()` 负责调度相关逻辑。

## 16. `timer_tick()`：抢占式调度的核心

[`timer_tick()`](src/sched.c#L94) 是 exp4b 最重要的新增逻辑。

代码逻辑可以翻译成：

```text
current->counter--

如果 current->counter > 0:
  时间片还没用完，继续运行当前任务

如果 current->preempt_count > 0:
  当前任务处于不可抢占区域，继续运行当前任务

否则：
  当前任务时间片用完，并且可以被抢占
  把 counter 设为 0
  打开 IRQ
  调用 _schedule()
  关 IRQ，等待 kernel_exit/eret 恢复中断状态
```

对应代码：

```c
--current->counter;
if (current->counter > 0 || current->preempt_count > 0)
    return;
current->counter = 0;

enable_irq();
_schedule();
disable_irq();
```

这个函数就是 exp4b 从“任务主动让出 CPU”变成“内核按 timer 抢占任务”的关键。

## 17. 为什么 `entry.S` 要保存 `elr_el1` 和 `spsr_el1`

exp4b 的 [`kernel_entry`](src/entry.S#L17) 比 exp4a 多保存了：

```asm
mrs x22, elr_el1
mrs x23, spsr_el1
stp x30, x22, [sp, #16 * 15]
str x23, [sp, #16 * 16]
```

返回时在 [`kernel_exit`](src/entry.S#L40) 恢复：

```asm
ldr x23, [sp, #16 * 16]
ldp x30, x22, [sp, #16 * 15]
msr elr_el1, x22
msr spsr_el1, x23
```

原因是抢占式调度下，中断可能发生在任意位置。

CPU 进入异常时：

```text
elr_el1 记录被打断时应该返回的地址；
spsr_el1 记录被打断前的处理器状态，包括中断屏蔽位等。
```

如果中断 handler 里发生了任务切换，之后再切回这个任务时，必须还能准确恢复它被打断的位置和状态。

所以 exp4b 必须保存 `elr_el1` / `spsr_el1`。这是它比 exp4a 更复杂的核心原因之一。

## 18. `kernel_exit` 和 `eret`：回到被中断或被切换后的任务

IRQ handler 结束时会走 [`kernel_exit`](src/entry.S#L40)，最后执行：

```asm
eret
```

`eret` 会根据恢复后的：

- `elr_el1`
- `spsr_el1`

返回到正确的位置。

如果 timer interrupt 里没有发生调度：

```text
eret 回到同一个任务被中断的位置
```

如果 timer interrupt 里发生了调度：

```text
_schedule()
  -> switch_to(next)
  -> cpu_switch_to(prev, next)

之后 kernel_exit/eret 会在当前恢复出来的任务上下文中返回。
```

这就是抢占式调度的微妙之处：中断 handler 看起来还是同一个 handler，但它中途可能已经把 `current` 和栈切成了另一个任务。

## 19. exp4b 中一次完整抢占的链路

把所有东西串起来：

```text
process("12345") 正在运行
  -> timer 到期
  -> CPU 自动进入 IRQ
  -> entry.S: el1_irq
       -> kernel_entry 保存通用寄存器、elr_el1、spsr_el1
       -> handle_irq()
       -> handle_generic_timer_irq()
       -> timer_tick()
            -> current->counter--
            -> counter 到 0
            -> enable_irq()
            -> _schedule()
                 -> preempt_disable()
                 -> 选择 task[] 中 counter 最大的任务
                 -> switch_to(next)
                 -> cpu_switch_to(prev, next)
                 -> preempt_enable()
            -> disable_irq()
       -> kernel_exit 恢复寄存器、elr_el1、spsr_el1
       -> eret
  -> 返回到被选中的任务继续运行
```

如果被选中的任务是第一次运行：

```text
cpu_switch_to()
  -> ret
  -> ret_from_fork
  -> schedule_tail()
  -> process2("abcde")
```

如果被选中的任务之前运行过：

```text
cpu_switch_to()
  -> ret
  -> 回到它上次被切走后保存的 pc 附近继续执行
```

## 20. exp4b 新增或重点变化的文件

抢占式调度核心：

- [`src/sched.c`](src/sched.c)：新增 `timer_tick()`、`preempt_disable()`、`preempt_enable()`。
- [`include/sched.h`](include/sched.h)：声明 timer/preempt 相关函数。

timer 驱动调度：

- [`src/timer.c`](src/timer.c)：timer IRQ 中调用 `timer_tick()`。
- [`src/timer.S`](src/timer.S)：打开和重置 generic timer。
- [`src/irq.c`](src/irq.c)：分发 generic timer interrupt。

异常入口增强：

- [`src/entry.S`](src/entry.S)：中断入口额外保存/恢复 `elr_el1` 和 `spsr_el1`。
- [`include/entry.h`](include/entry.h)：异常帧大小定义。

任务创建增强：

- [`src/fork.c`](src/fork.c)：创建任务时使用 preempt 保护，并初始化新任务 `preempt_count`。

测试任务：

- [`src/kernel.c`](src/kernel.c)：打开 IRQ，创建两个不会主动 yield 的任务。

## 21. 最核心的一条执行链

如果只记一条线，记这条：

```text
kernel_main()
  -> irq_vector_init()
  -> generic_timer_init()
       -> gen_timer_init()
       -> gen_timer_reset(interval)
  -> enable_interrupt_controller()
  -> enable_irq()
  -> copy_process(process, "12345")
  -> copy_process(process2, "abcde")
  -> schedule()
       -> switch_to(new task)
       -> ret_from_fork
       -> process(arg)

process(arg)
  -> 不主动 schedule()
  -> timer interrupt 打断它

timer interrupt
  -> el1_irq
  -> handle_irq()
  -> handle_generic_timer_irq()
  -> timer_tick()
       -> current->counter--
       -> counter 到 0 且 preempt_count == 0
       -> _schedule()
       -> switch_to(next)
       -> cpu_switch_to(prev, next)
  -> kernel_exit
  -> eret
  -> 回到被选中的任务
```

所以 exp4b 新增内容的本质是：

```text
用 timer interrupt 定期夺回 CPU；
用 counter 表示任务剩余时间片；
用 preempt_count 防止关键代码被抢占；
用 timer_tick() 把中断和 scheduler 接起来；
用 elr_el1/spsr_el1 保证被中断的位置和状态可以恢复。
```

