# exp4a 启动流程介绍：对比 exp3 新增了什么

这份文档按 CPU 和内核代码的实际执行顺序来读：先从 `_start` 进入 EL1，再进入 `kernel_main()`，然后创建任务，最后说明 `schedule()` 如何切换到不同任务运行。

重点结论：

```text
exp4a 没有大改早期 boot.S 启动骨架。
exp4a 真正新增的是 kernel_main() 之后的简单内存分配、任务创建、协作式调度和上下文切换。

另外，exp4a 还加入了可选的 framebuffer 图形输出支持。
```

## 0. 总体对比

exp3 的主流程：

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
       -> while (1) 空转

timer 到期后：
  -> 硬件产生 IRQ
  -> CPU 根据 VBAR_EL1 找到 vectors
  -> 进入 el1_irq
  -> handle_irq()
  -> handle_local_timer_irq()
  -> 重新设置下一次 timer
  -> eret 返回原来的位置
```

exp4a 的主流程：

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
       -> generic_timer_init()
       -> enable_interrupt_controller()
       -> disable_irq()
       -> copy_process(process, "12345")
       -> copy_process(process, "abcde")
       -> while (1) schedule()

第一次 schedule()：
  -> 从 task[] 里选出一个可运行任务
  -> switch_to(next)
  -> cpu_switch_to(prev, next)
  -> ret 跳到 ret_from_fork
  -> ret_from_fork 调用 process(arg)

任务运行中：
  -> process() 打印 5 个字符
  -> 主动调用 schedule()
  -> 切换到另一个任务
```

可以把 exp4a 理解成：

```text
exp3: 会处理中断和 timer 的单任务内核
exp4a: 在 exp3 基础上加入两个内核任务，并通过主动 yield 做协作式多任务
```

## 1. `_start`：早期启动骨架基本不变

exp4a 仍然从 [`src/boot.S:_start`](src/boot.S#L8) 开始。

第一步还是只让 core 0 继续运行：

- [`mrs x0, mpidr_el1`](src/boot.S#L9)：读取当前 CPU core id。
- [`and x0, x0,#0xFF`](src/boot.S#L10)：取出低 8 位。
- [`cbz x0, master`](src/boot.S#L11)：如果是 core 0，就进入 `master`。
- [其他 core 进入 `proc_hang`](src/boot.S#L14)。

这一段和 exp3 的思路一样。exp4a 的多任务不是多核并行，仍然只在 core 0 上运行。

## 2. `master`：进入 EL1 的流程基本不变

在 [`src/boot.S:master`](src/boot.S#L17) 中，exp4a 仍然先配置：

- [`sctlr_el1`](src/boot.S#L18)：使用 `SCTLR_VALUE_MMU_DISABLED`，MMU/cache 暂时关闭。
- [`hcr_el2`](src/boot.S#L21)：使用 `HCR_VALUE`，让 EL1 使用 AArch64。

如果是 QEMU，代码从 EL2 进入 EL1：

- [写 `spsr_el2`](src/boot.S#L25)
- [写 `elr_el2 = el1_entry`](src/boot.S#L28)
- [`eret`](src/boot.S#L41)

如果是真 RPi3 硬件，代码从 EL3 进入 EL1：

- [写 `scr_el3`](src/boot.S#L31)
- [写 `spsr_el3`](src/boot.S#L34)
- [写 `elr_el3 = el1_entry`](src/boot.S#L37)
- [`eret`](src/boot.S#L41)

这一阶段的关键理解仍然是：

```text
QEMU: 当前 EL2 -> eret -> EL1h
RPi3: 当前 EL3 -> eret -> Non-secure EL1h
```

## 3. `el1_entry`：清 BSS，设置内核栈，进入 C 代码

进入 EL1 后，exp4a 在 [`src/boot.S:el1_entry`](src/boot.S#L43) 做三件事：

- [清 BSS](src/boot.S#L44)：调用 `memzero(bss_begin, bss_end - bss_begin)`。
- [设置 `sp = LOW_MEMORY`](src/boot.S#L49)：把当前启动任务的栈放到低内存边界。
- [调用 `kernel_main`](src/boot.S#L50)：进入 C 代码。

这里和 exp3 仍然很像。真正新增的内容从 `kernel_main()` 开始明显出现。

## 4. `kernel_main()`：exp4a 的主线从这里变成多任务

exp4a 的 C 入口是 [`src/kernel.c:kernel_main`](src/kernel.c#L50)。

开头仍然是基础初始化：

- [初始化 UART](src/kernel.c#L52)
- [初始化 printf](src/kernel.c#L53)
- [打印 `kernel boots`](src/kernel.c#L55)

然后初始化异常和 timer：

- [`irq_vector_init()`](src/kernel.c#L60)：设置 `VBAR_EL1`，注册异常向量表。
- [`generic_timer_init()`](src/kernel.c#L61)：打开 ARM generic timer。
- [`enable_interrupt_controller()`](src/kernel.c#L62)：打开 core 0 的 timer interrupt 来源。
- [`disable_irq()`](src/kernel.c#L63)：注意这里关闭了 IRQ。

这里和 exp3 有一个重要差别：

```text
exp3: 初始化 timer/IRQ 后 enable_irq()，依赖 timer 中断演示 IRQ 流程。
exp4a: 初始化 timer/IRQ 后 disable_irq()，当前主线使用协作式调度，不依赖 timer 抢占。
```

也就是说，exp4a 的任务切换不是 timer 中断强行打断任务，而是任务自己主动调用 `schedule()`。

## 5. 可选流程：初始化 framebuffer 图形输出

如果在 Makefile 里打开 `USE_LFB`：

```make
# COPS += -DUSE_LFB
```

那么 `kernel_main()` 还会执行：

- [`lfb_init()`](src/kernel.c#L66)：通过 mailbox 向 GPU 申请 framebuffer。
- [`lfb_showpicture()`](src/kernel.c#L67)：显示内置图片。
- [`lfb_print(0, 240, "kernel boots")`](src/kernel.c#L68)：在屏幕上打印文字。

相关新增文件包括：

- [`src/lfb.c`](src/lfb.c)：framebuffer 初始化、画字符、显示图片。
- [`src/mbox.c`](src/mbox.c)：mailbox 调用。
- [`include/lfb.h`](include/lfb.h)：LFB 接口。
- [`include/mbox.h`](include/mbox.h)：mailbox 常量和接口。
- [`font.psf`](font.psf)：字体文件。
- [`include/rev_uva_logo_color3-resized.h`](include/rev_uva_logo_color3-resized.h)：内置图片数据。

这部分是可选图形输出，不是 exp4a 调度机制的核心。

## 6. 新增步骤一：`copy_process()` 创建两个任务

exp4a 在 [`kernel_main()`](src/kernel.c#L71) 里创建两个任务：

```c
copy_process((unsigned long)&process, (unsigned long)"12345");
copy_process((unsigned long)&process, (unsigned long)"abcde");
```

这两个任务运行同一个函数 [`process()`](src/kernel.c#L19)，但参数不同：

```text
任务 1: process("12345")
任务 2: process("abcde")
```

`copy_process()` 的声明在 [`include/fork.h`](include/fork.h#L4)，实现位于 [`src/fork.c`](src/fork.c#L7)。

创建任务的关键步骤：

1. [调用 `get_free_page()` 分配一页内存](src/fork.c#L13)。
2. [把这页底部当作 `task_struct`](src/fork.c#L13)。
3. [设置任务状态为 `TASK_RUNNING`](src/fork.c#L17)。
4. [设置任务时间片计数 `counter`](src/fork.c#L18)。
5. [把任务函数地址保存到 `cpu_context.x19`](src/fork.c#L21)。
6. [把任务参数保存到 `cpu_context.x20`](src/fork.c#L22)。
7. [把任务首次运行地址设置为 `ret_from_fork`](src/fork.c#L23)。
8. [把任务栈指针设置到这一页顶部](src/fork.c#L25)。
9. [把任务挂到全局 `task[]` 数组](src/fork.c#L27)。

这里有一个很重要的设计：

```text
新任务第一次被切换过去时，并不是直接跳到 process()。
它先跳到 ret_from_fork，再由 ret_from_fork 调用 process(arg)。
```

原因是 `cpu_switch_to()` 最后使用 `ret` 返回，而 `ret` 会跳到恢复出来的 `x30`。所以新任务的 `cpu_context.pc` 被提前设置成 `ret_from_fork`。

## 7. 新增步骤二：简单页分配器 `get_free_page()`

exp4a 新增了 [`src/mm.c`](src/mm.c)，提供一个很简单的页分配器。

相关内存范围在 [`include/mm.h`](include/mm.h#L13)：

- [`LOW_MEMORY`](include/mm.h#L13)：低内存边界，也是启动时 `sp` 的位置。
- [`HIGH_MEMORY`](include/mm.h#L14)：等于 `PBASE`，也就是外设物理基地址。
- [`PAGING_MEMORY`](include/mm.h#L16)：可用于分配的内存总量。
- [`PAGING_PAGES`](include/mm.h#L17)：可分配页数。

[`get_free_page()`](src/mm.c#L6) 的逻辑很直接：

```text
遍历 mem_map[]
  -> 找到一个值为 0 的页
  -> 标记为 1
  -> 返回 LOW_MEMORY + i * PAGE_SIZE
```

[`free_page()`](src/mm.c#L17) 则把对应页重新标记为空闲。

在 exp4a 里，这个页分配器主要用于给每个新任务分配：

```text
一页内存 = task_struct + 内核栈
```

## 8. 新增步骤三：`task_struct` 描述一个任务

任务元数据定义在 [`include/sched.h`](include/sched.h#L47)：

```c
struct task_struct {
    struct cpu_context cpu_context;
    long state;
    long counter;
    long priority;
    long preempt_count;
};
```

其中最关键的是 [`cpu_context`](include/sched.h#L31)：

```c
struct cpu_context {
    unsigned long x19;
    ...
    unsigned long fp;
    unsigned long sp;
    unsigned long pc;
};
```

它保存任务之间切换时需要恢复的寄存器。

exp4a 还定义了几个全局变量：

- [`current`](src/sched.c#L6)：当前正在运行的任务。
- [`task[NR_TASKS]`](src/sched.c#L7)：所有任务的数组。
- [`nr_tasks`](src/sched.c#L8)：当前任务数量。

启动时，系统先有一个 [`init_task`](src/sched.c#L5)。后面 `copy_process()` 创建的新任务会放进 `task[]`。

## 9. 新增步骤四：主循环反复调用 `schedule()`

两个任务创建好后，`kernel_main()` 进入：

```c
while (1) {
    schedule();
}
```

位置在 [`src/kernel.c`](src/kernel.c#L83)。

[`schedule()`](src/sched.c#L50) 做的第一件事是：

```c
current->counter = 0;
```

这表示当前任务主动放弃剩余时间片。然后它调用 [`_schedule()`](src/sched.c#L10) 选择下一个任务。

这就是协作式调度：

```text
任务不会被 timer 强行抢占。
任务运行到合适位置时，自己调用 schedule() 让出 CPU。
```

## 10. `_schedule()`：选择下一个任务

[`_schedule()`](src/sched.c#L10) 会在 `task[]` 中寻找：

```text
state == TASK_RUNNING 且 counter 最大的任务
```

关键代码在 [`src/sched.c`](src/sched.c#L23)：

```c
for (int i = 0; i < NR_TASKS; i++) {
    p = task[i];
    if (p && p->state == TASK_RUNNING && p->counter > c) {
        c = p->counter;
        next = i;
    }
}
```

如果所有可运行任务的 `counter` 都是 0，就重新给任务补充 counter：

```c
p->counter = (p->counter >> 1) + p->priority;
```

位置在 [`src/sched.c`](src/sched.c#L38)。

最后调用：

```c
switch_to(task[next]);
```

位置在 [`src/sched.c`](src/sched.c#L47)。

## 11. `switch_to()`：更新 current，然后进入汇编上下文切换

[`switch_to()`](src/sched.c#L57) 先判断要切换的任务是不是当前任务：

```c
if (current == next)
    return;
```

如果不是，就保存：

```text
prev = current
current = next
```

然后调用 [`cpu_switch_to(prev, next)`](src/sched.c#L63)。

这里开始进入汇编层面的上下文切换。

## 12. `cpu_switch_to()`：保存旧任务，恢复新任务

`cpu_switch_to()` 位于 [`src/sched.S`](src/sched.S#L5)。

它先保存旧任务的上下文：

- [保存 `x19` 到 `x28`](src/sched.S#L12)
- [保存 `x29` 和当前 `sp`](src/sched.S#L17)
- [保存 `x30` 到 `cpu_context.pc`](src/sched.S#L18)

然后恢复新任务的上下文：

- [恢复 `x19` 到 `x28`](src/sched.S#L23)
- [恢复 `x29` 和新任务 `sp`](src/sched.S#L28)
- [恢复 `x30`](src/sched.S#L29)
- [设置 CPU 的 `sp`](src/sched.S#L30)

最后执行：

```asm
ret
```

位置在 [`src/sched.S`](src/sched.S#L35)。

这句 `ret` 很关键：

```text
如果这个任务以前运行过：
  ret 回到它上次离开 cpu_switch_to() 后的位置。

如果这个任务是第一次运行：
  ret 跳到 copy_process() 预先放好的 ret_from_fork。
```

## 13. `ret_from_fork`：新任务第一次真正开始运行

`ret_from_fork` 定义在 [`src/entry.S`](src/entry.S#L134)。

第一次切换到新任务时，`cpu_switch_to()` 恢复出：

```text
x19 = 任务函数地址，也就是 process
x20 = 任务参数，也就是 "12345" 或 "abcde"
x30 = ret_from_fork
```

然后 `ret` 跳到 `ret_from_fork`。

`ret_from_fork` 做两件事：

- [调用 `schedule_tail`](src/entry.S#L139)，当前实现为空函数。
- [把 `x20` 放到 `x0`](src/entry.S#L145)，作为 C 函数第一个参数。
- [调用 `blr x19`](src/entry.S#L146)，跳进真正的任务函数。

所以最终效果是：

```text
ret_from_fork
  -> process("12345")

或者：

ret_from_fork
  -> process("abcde")
```

## 14. `process()`：任务主动 yield

任务函数是 [`src/kernel.c:process`](src/kernel.c#L19)。

它的逻辑是：

```text
while (1):
  打印参数里的 5 个字符
  delay()
  schedule()
```

对应代码：

- [循环打印 5 个字符](src/kernel.c#L31)
- [通过 UART 输出字符](src/kernel.c#L33)
- [延迟](src/kernel.c#L41)
- [调用 `schedule()` 主动让出 CPU](src/kernel.c#L43)

由于 `kernel_main()` 创建了两个任务，所以输出会在两组字符串之间切换：

```text
12345abcde12345abcde...
```

实际顺序会受到 delay、调度点和运行环境影响，但核心现象是两个任务轮流运行。

## 15. exp4a 中的 timer/IRQ：保留了基础，但不是当前主角

exp4a 仍然有异常向量表和 timer 初始化：

- [`irq_vector_init()`](src/kernel.c#L60)
- [`generic_timer_init()`](src/kernel.c#L61)
- [`enable_interrupt_controller()`](src/kernel.c#L62)

异常向量表在 [`src/entry.S`](src/entry.S#L63)。

EL1h IRQ 入口是：

- [`ventry el1_irq`](src/entry.S#L70)
- [`el1_irq`](src/entry.S#L129)
- [`bl handle_irq`](src/entry.S#L131)
- [`eret`](src/entry.S#L132)

`handle_irq()` 在 [`src/irq.c`](src/irq.c#L21)，目前主要检查 core 0 的 generic timer interrupt。

但因为 [`kernel_main()`](src/kernel.c#L63) 调用了 `disable_irq()`，所以 exp4a 当前默认并不靠 timer IRQ 来触发调度。

这也是 exp4a 和后续实验之间的伏笔：

```text
exp4a 当前是协作式调度：
  process() 主动 schedule()

后续如果打开 IRQ 并在 timer tick 中调用调度逻辑：
  就可以发展成抢占式调度
```

## 16. 新增文件按功能归类

多任务和调度：

- [`include/sched.h`](include/sched.h)：任务结构、CPU 上下文、调度接口。
- [`src/sched.c`](src/sched.c)：调度器、任务选择、`switch_to()`。
- [`src/sched.S`](src/sched.S)：真正保存/恢复寄存器的上下文切换。

任务创建：

- [`include/fork.h`](include/fork.h)：`copy_process()` 声明。
- [`src/fork.c`](src/fork.c)：创建任务，初始化任务上下文。

简单内存分配：

- [`src/mm.c`](src/mm.c)：`get_free_page()` / `free_page()`。
- [`include/mm.h`](include/mm.h)：新增 `HIGH_MEMORY`、`PAGING_MEMORY`、`PAGING_PAGES`。

新任务启动入口：

- [`src/entry.S`](src/entry.S)：新增 `ret_from_fork`。
- [`include/entry.h`](include/entry.h)：声明 `ret_from_fork()`。

可选图形输出：

- [`src/lfb.c`](src/lfb.c)
- [`src/mbox.c`](src/mbox.c)
- [`include/lfb.h`](include/lfb.h)
- [`include/mbox.h`](include/mbox.h)
- [`font.psf`](font.psf)
- [`include/rev_uva_logo_color3-resized.h`](include/rev_uva_logo_color3-resized.h)

## 17. 最核心的一条执行链

如果只记一条线，记这条：

```text
kernel_main()
  -> copy_process(process, "12345")
       -> get_free_page()
       -> 初始化 task_struct
       -> cpu_context.pc = ret_from_fork
       -> cpu_context.x19 = process
       -> cpu_context.x20 = "12345"
       -> task[pid] = p

  -> copy_process(process, "abcde")
       -> 同上

  -> schedule()
       -> _schedule()
       -> switch_to(next)
       -> cpu_switch_to(prev, next)
       -> ret
       -> ret_from_fork
       -> process(arg)
       -> 打印字符
       -> schedule()
       -> 切换到另一个任务
```

所以 exp4a 新增内容的本质是：

```text
用一页内存表示一个任务；
用 task_struct 保存任务元数据；
用 cpu_context 保存任务寄存器；
用 schedule() 选择下一个任务；
用 cpu_switch_to() 完成真正的上下文切换；
用 ret_from_fork 让新任务第一次进入自己的函数。
```
