# exp4 进程与调度实验报告

## 目标

本次在 `exp4` 中完成内核任务和调度相关练习：

1. 实现内核任务创建和上下文切换。
2. 从协作式调度扩展到 timer 驱动的抢占式调度。
3. 为任务引入 priority，使高优先级任务获得更多 CPU 时间。
4. 保存和恢复 FP/SIMD 寄存器上下文。
5. 改造页分配器，从固定扫描改为基于空闲链表和懒分配的 page allocator。
6. 增加有限 debug 输出，用于观察内存、栈、任务和调度状态。

最终效果：

```text
两个任务可以被创建并轮流运行；
timer interrupt 会触发抢占式调度；
priority 更高的任务会连续运行更多 timer tick；
FP/SIMD 状态会随任务切换保存和恢复；
task page 从 kernel_end 之后的空闲物理页懒分配。
```

## 1. 任务创建与上下文切换

修改位置：

- [include/sched.h](include/sched.h)
- [src/sched.c](src/sched.c)
- [src/sched.S](src/sched.S)
- [include/fork.h](include/fork.h)
- [src/fork.c](src/fork.c)
- [src/entry.S](src/entry.S)
- [src/kernel.c](src/kernel.c)

每个任务使用一页内存保存：

```text
task_struct + kernel stack
```

`copy_process_with_priority()` 在 [src/fork.c](src/fork.c) 中完成任务创建：

```c
p = (struct task_struct *) get_free_page();
p->priority = priority;
p->state = TASK_RUNNING;
p->counter = p->priority;
p->cpu_context.x19 = fn;
p->cpu_context.x20 = arg;
p->cpu_context.pc = (unsigned long)ret_from_fork;
p->cpu_context.sp = (unsigned long)p + THREAD_SIZE;
```

第一次切换到新任务时，`cpu_switch_to()` 会恢复 `pc = ret_from_fork`，然后进入：

```text
ret_from_fork
  -> schedule_tail()
  -> process(arg)
```

真正保存和恢复通用寄存器的代码在 [src/sched.S](src/sched.S)，主要保存 `x19-x30` 和 `sp`。

## 2. 抢占式调度

修改位置：

- [src/kernel.c](src/kernel.c)
- [src/timer.c](src/timer.c)
- [src/irq.c](src/irq.c)
- [src/sched.c](src/sched.c)
- [src/entry.S](src/entry.S)

初始化流程位于 [src/kernel.c](src/kernel.c)：

```c
irq_vector_init();
generic_timer_init();
enable_interrupt_controller();
enable_irq();
```

timer 到期后，整体流程为：

```text
timer interrupt
  -> el1_irq
  -> handle_irq()
  -> handle_generic_timer_irq()
  -> timer_tick()
```

`timer_tick()` 位于 [src/sched.c](src/sched.c)，负责扣减当前任务的 `counter`：

```c
--current->counter;
if (current->counter > 0 || current->preempt_count > 0)
	return;

enable_irq();
_schedule();
disable_irq();
```

因此任务不需要主动调用 `schedule()`，timer interrupt 会在时间片耗尽时触发调度。

## 3. Priority 调度

修改位置：

- [include/fork.h](include/fork.h)
- [src/fork.c](src/fork.c)
- [src/kernel.c](src/kernel.c)

新增接口：

```c
int copy_process_with_priority(unsigned long fn, unsigned long arg, long priority);
```

当前测试中设置：

```c
#define PROCESS1_PRIORITY 1
#define PROCESS2_PRIORITY 3
```

调度器在所有任务 `counter` 用完后按 priority 充值：

```c
p->counter = (p->counter >> 1) + p->priority;
```

因此 priority 为 3 的任务会获得约 3 个 timer tick，priority 为 1 的任务获得约 1 个 timer tick。

## 4. FP/SIMD 上下文保存

修改位置：

- [include/arm/sysregs.h](include/arm/sysregs.h)
- [src/boot.S](src/boot.S)
- [include/sched.h](include/sched.h)
- [src/sched.S](src/sched.S)
- [include/utils.h](include/utils.h)
- [src/utils.S](src/utils.S)
- [src/kernel.c](src/kernel.c)

启动时在 [src/boot.S](src/boot.S) 中打开 FP/SIMD：

```asm
ldr x0, =CPACR_EL1_FPEN
msr cpacr_el1, x0
isb
```

`task_struct` 中新增 `fpsimd_context`，用于保存：

```text
q0-q31
fpcr
fpsr
```

`cpu_switch_to()` 在切出任务时保存 FP/SIMD 寄存器，在切入任务时恢复对应寄存器。

为了验证 FP/SIMD 上下文不会互相污染，两个测试任务分别写入不同 marker：

```text
process 1: 0x11111111
process 2: 0x22222222
```

运行过程中没有出现 `marker corrupted`，说明 FP/SIMD context 能随任务正确切换。

## 5. 页分配器改造

修改位置：

- [src/linker-qemu.ld](src/linker-qemu.ld)
- [src/linker.ld](src/linker.ld)
- [include/mm.h](include/mm.h)
- [src/mm.c](src/mm.c)
- [src/kernel.c](src/kernel.c)

linker script 新增：

```ld
kernel_end = .;
```

页分配器初始化时使用：

```c
alloc_start = align_up((unsigned long)&kernel_end, PAGE_SIZE);
```

当前 allocator 采用：

```text
未分配过的页：next_unallocated_page 懒分配
释放过的页：侵入式 free_list 复用
```

`get_free_page()` 优先从 `free_list` 取回收页；如果没有回收页，则从 `next_unallocated_page` 获取下一页。`free_page()` 会把释放页插回 `free_list`。

由于启动栈从 `LOW_MEMORY` 向下增长，allocator 会跳过：

```text
[LOW_MEMORY - 4 * PAGE_SIZE, LOW_MEMORY)
```

这段保留给 boot stack，避免 task page 覆盖启动栈。

## 6. Debug 输出

修改位置：

- [include/debug.h](include/debug.h)
- [src/debug.c](src/debug.c)
- [src/kernel.c](src/kernel.c)
- [src/fork.c](src/fork.c)
- [src/sched.c](src/sched.c)
- [src/irq.c](src/irq.c)

为了避免串口刷屏，debug 输出按类别限流，只在关键事件打印：

```text
boot 初始化
task page 分配
schedule 选择
switch_to 切换
timer_tick 抢占
IRQ pending
task 第一次启动
```

典型输出：

```text
[mm] alloc_start=85000 free_pages=257911
[alloc] pid=1 task=85000 fpsimd=85070 stack_top=86000 priority=1
[alloc] pid=2 task=86000 fpsimd=86070 stack_top=87000 priority=3
[timer] cur=86000 priority=3 counter=3->2 schedule=0
[timer] cur=86000 priority=3 counter=1->0 schedule=1
[switch] prev=86000 next=85000 ...
```

这些输出用于确认任务地址、栈地址、FP/SIMD context 地址、调度决策和 timer 抢占行为。

## 7. 验证结果

编译：

```bash
make
```

运行：

```bash
timeout 5s make run
```

观察到：

```text
kernel boots
[mm] alloc_start=85000 free_pages=257911
[alloc] pid=1 task=85000 ... priority=1
[alloc] pid=2 task=86000 ... priority=3
[fpsimd] process 2 marker=22222222
[fpsimd] process 1 marker=11111111
```

priority 验证：

```text
[timer] cur=86000 priority=3 counter=3->2 schedule=0
[timer] cur=86000 priority=3 counter=2->1 schedule=0
[timer] cur=86000 priority=3 counter=1->0 schedule=1

[timer] cur=85000 priority=1 counter=1->0 schedule=1
```

说明高优先级任务获得了更多 timer tick。

FP/SIMD 验证：

```text
[fpsimd] process 2 marker=22222222
[fpsimd] process 1 marker=11111111
```

运行过程中没有出现：

```text
marker corrupted
```

说明 FP/SIMD 寄存器在任务切换时被正确保存和恢复。

## 结论

本次实验完成了 exp4 的进程和调度扩展：

- 实现了任务创建、内核栈分配和上下文切换。
- 使用 timer interrupt 实现抢占式调度。
- 引入 priority，使高优先级任务获得更多 CPU 时间。
- 支持 FP/SIMD 寄存器随任务保存和恢复。
- 将页分配器改为基于 `kernel_end` 的懒分配和侵入式 free list。
- 增加限流 debug 输出，便于观察当前内存、栈和调度状态。

