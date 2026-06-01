# exp5 启动流程介绍：对比 exp4b 新增了什么

这份文档参照 exp4b 的启动流程说明，按代码真正运行的顺序来读：先从 `_start` 进入 EL1，再进入 `kernel_main()`，创建一个 kernel thread，随后把它切换到 EL0 用户态；用户态代码通过 `svc` 进入内核，使用 syscall 完成输出、分配用户栈、clone 用户线程和退出。

重点结论：

```text
exp4b: 内核中创建多个 kernel task，timer interrupt 抢占它们。
exp5: 先创建 kernel thread，再把它转换成 EL0 user process；
      用户态不能直接调用内核函数，而是通过 syscall 请求内核服务。

所以 exp5 比 exp4b 多了：
1. EL0 用户态运行路径：move_to_user_mode() + kernel_exit 0
2. pt_regs：保存和恢复用户态寄存器、pc、sp、pstate
3. syscall 框架：svc 入口、sys_call_table、sys_write/sys_malloc/sys_clone/sys_exit
4. 用户态 syscall wrapper：call_sys_write/call_sys_malloc/call_sys_clone/call_sys_exit
5. 用户线程 clone：为 child 准备独立 user stack 和内核栈上的 pt_regs
6. 进程退出与资源回收：TASK_ZOMBIE、exit_process()、free_page(user stack)
```

## 0. 总体对比

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
  -> 在 EL1 内核态运行
  -> 直接 uart_send()/printf()
  -> 被 timer interrupt 抢占
```

exp5 的主流程：

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
       -> copy_process(PF_KTHREAD, kernel_process, 0, 0)
       -> while (1) schedule()

kernel_process()
  -> 仍在 EL1 内核态运行
  -> move_to_user_mode(user_process)
  -> 返回 ret_from_fork
  -> kernel_exit 0
  -> eret 进入 EL0 user_process()

user_process()
  -> call_sys_write()
  -> call_sys_malloc()
  -> call_sys_clone(user_process1, arg, stack)
  -> call_sys_exit()

user_process1()
  -> 通过 call_sys_write() 输出字符
  -> timer interrupt 仍然可以抢占用户态任务
```

可以这样记：

```text
exp4b 重点是 preemptive scheduling。
exp5 在保留抢占式调度的基础上，引入 user/kernel 分离和 syscall 边界。
```

## 1. 早期启动流程基本沿用 exp4b

exp5 仍然从 [src/boot.S](src/boot.S) 的 `_start` 开始：

- 只让 core 0 继续运行，其他 core 停在 `proc_hang`。
- 按 QEMU 或 RPi3 的启动环境，从 EL2 或 EL3 切到 EL1。
- 清 BSS，设置初始内核栈。
- 调用 `kernel_main()` 进入 C 代码。

这部分和 exp4b 的启动骨架没有本质变化。exp5 的新增内容主要从任务创建、异常返回和 syscall 路径开始。

## 2. `kernel_main()`：不再直接创建两个普通内核任务

exp4b 中，`kernel_main()` 直接创建两个会打印字符的任务：

```text
copy_process(process, "12345")
copy_process(process2, "abcde")
```

exp5 中，[src/kernel.c](src/kernel.c) 改成先创建一个 kernel thread：

```c
copy_process(PF_KTHREAD, (unsigned long)&kernel_process, 0, 0);
```

这里的关键变化是 `copy_process()` 增加了 `clone_flags` 参数。`PF_KTHREAD` 表示创建的是内核线程，它第一次运行时仍从 EL1 执行 `kernel_process()`。

初始化 timer interrupt 的流程仍然存在：

```c
irq_vector_init();
generic_timer_init();
enable_interrupt_controller();
enable_irq();
```

因此 exp5 不是取消 exp4b 的抢占式调度，而是在它上面继续构造用户态任务模型。

## 3. `task_struct`：新增用户栈和任务类型信息

exp5 的任务结构在 [include/sched.h](include/sched.h) 中新增：

```c
unsigned long stack;
unsigned long flags;
```

其中：

- `flags` 用来区分 kernel thread 和普通用户任务，例如 `PF_KTHREAD`。
- `stack` 保存用户态栈页地址，任务退出时用于释放。

同时新增：

```c
#define TASK_ZOMBIE 1
#define PF_KTHREAD 0x00000002
```

`TASK_ZOMBIE` 用于标记已经退出的任务，调度器只选择 `TASK_RUNNING` 的任务继续运行。

## 4. `pt_regs`：为用户态返回准备完整寄存器现场

exp4b 的上下文切换主要保存 `cpu_context`，也就是 `x19-x30` 和 `sp` 这类内核调度所需寄存器。

exp5 在 [include/fork.h](include/fork.h) 中新增 `pt_regs`：

```c
struct pt_regs {
	unsigned long regs[31];
	unsigned long sp;
	unsigned long pc;
	unsigned long pstate;
};
```

它放在每个任务内核栈的顶部，由 [src/fork.c](src/fork.c) 中的 `task_pt_regs()` 计算：

```text
task page base + THREAD_SIZE - sizeof(struct pt_regs)
```

`pt_regs` 的作用是保存异常返回时需要恢复的用户态状态：

- `regs[0..30]`：通用寄存器。
- `sp`：用户态栈指针。
- `pc`：用户态下一条指令地址。
- `pstate`：返回到哪个异常级别，以及中断屏蔽状态。

这让内核可以通过 `kernel_exit 0` 和 `eret` 精确返回 EL0。

## 5. `move_to_user_mode()`：把 kernel thread 转成 user process

exp5 新增 [src/fork.c](src/fork.c) 中的 `move_to_user_mode()`。

它在当前任务的内核栈顶部准备一份 `pt_regs`：

```c
regs->pc = pc;
regs->pstate = PSR_MODE_EL0t;
regs->sp = stack + PAGE_SIZE;
current->stack = stack;
```

这里 `pc` 指向 `user_process()`，`pstate = PSR_MODE_EL0t` 表示稍后 `eret` 要回到 EL0。

运行路径是：

```text
kernel_process()
  -> move_to_user_mode(user_process)
  -> 返回 ret_from_fork
  -> ret_to_user
  -> kernel_exit 0
  -> eret
  -> EL0 user_process()
```

这一步是 exp5 的核心分界线：同一个 `task_struct` 原本作为 kernel thread 启动，随后拥有用户栈和用户态寄存器现场，最终开始在 EL0 执行。

## 6. `entry.S`：异常入口支持 EL0 syscall 和 EL0 IRQ

exp4b 的异常入口主要处理 EL1 IRQ，用于 timer interrupt。

exp5 的 [src/entry.S](src/entry.S) 对 `kernel_entry` 和 `kernel_exit` 增加了 `el` 参数：

```text
kernel_entry 1 / kernel_exit 1：异常来自 EL1
kernel_entry 0 / kernel_exit 0：异常来自 EL0
```

来自 EL0 时，CPU 自动切到 EL1 栈运行异常处理代码，而用户栈指针保存在 `sp_el0`。因此 exp5 在 `kernel_entry 0` 中保存 `sp_el0`，在 `kernel_exit 0` 中恢复它。

异常向量表中也新增了两条关键路径：

```text
el0_sync：处理 EL0 同步异常，主要用于 svc syscall
el0_irq ：处理 EL0 运行期间到来的 timer interrupt
```

所以 exp5 中即使用户程序正在 EL0 执行，timer 到期也能进入 `el0_irq -> handle_irq() -> timer_tick()`，继续沿用 exp4b 的抢占式调度。

## 7. syscall 框架：用户态通过 `svc` 进入内核

exp5 新增 [include/sys.h](include/sys.h)、[src/sys.c](src/sys.c)、[src/sys.S](src/sys.S)。

系统调用号定义在 [include/sys.h](include/sys.h)：

```c
#define SYS_WRITE_NUMBER  0
#define SYS_MALLOC_NUMBER 1
#define SYS_CLONE_NUMBER  2
#define SYS_EXIT_NUMBER   3
```

用户态 wrapper 在 [src/sys.S](src/sys.S) 中实现。以 `call_sys_write()` 为例：

```asm
mov w8, #SYS_WRITE_NUMBER
svc #0
ret
```

进入内核后，[src/entry.S](src/entry.S) 的 `el0_sync` 会读取 `esr_el1`，确认异常类型是 `SVC64`，然后进入 `el0_svc`：

```text
读取 w8 中的 syscall number
检查 syscall number 是否越界
从 sys_call_table 取出 sys_* 函数地址
调用对应内核函数
把返回值写回保存的 x0
kernel_exit 0 返回用户态
```

内核侧 syscall table 在 [src/sys.c](src/sys.c)：

```c
void * const sys_call_table[] = {
	sys_write,
	sys_malloc,
	sys_clone,
	sys_exit
};
```

这条路径建立了真正的 user/kernel 边界：用户态代码不再直接调用 `printf()`、`get_free_page()` 或 `copy_process()`，而是通过 `svc` 请求内核服务。

## 8. `sys_write()`：用户态输出字符

exp4b 的任务在内核态，可以直接调用 `uart_send()` 或 `printf()`。

exp5 的用户态任务改为：

```c
call_sys_write(buf);
```

对应内核实现是 [src/sys.c](src/sys.c) 中的：

```c
void sys_write(char *buf) {
	printf(buf);
}
```

所以输出路径变成：

```text
EL0 user_process1()
  -> call_sys_write()
  -> svc
  -> el0_svc
  -> sys_write()
  -> printf()
  -> kernel_exit 0
  -> 回到 EL0
```

这比 exp4b 多了一次异常进入和异常返回，但也因此隔离了用户态和内核态。

## 9. `sys_malloc()`：为用户进程分配页

exp5 新增：

```c
unsigned long call_sys_malloc();
unsigned long sys_malloc();
```

用户态在 [src/kernel.c](src/kernel.c) 的 `user_process()` 中调用：

```c
unsigned long stack = call_sys_malloc();
```

内核态 `sys_malloc()` 使用 [src/mm.c](src/mm.c) 的 `get_free_page()` 分配一页物理页，并把地址返回给用户态。

在当前实验中，这页主要用作新 clone 出来的用户线程栈：

```text
stack
  -> 传给 call_sys_clone()
  -> 内核写入 childregs->sp = stack + PAGE_SIZE
  -> child 线程从这页顶部作为用户栈开始运行
```

## 10. `sys_clone()`：创建新的用户线程

exp4b 的 `copy_process()` 创建的是内核态任务，任务入口由 `cpu_context.x19/x20` 指定。

exp5 中，用户态通过 [src/sys.S](src/sys.S) 的 `call_sys_clone(fn, arg, stack)` 创建用户线程。

`call_sys_clone()` 做了两件事：

1. 把用户态函数入口 `fn` 和参数 `arg` 暂存在 `x10/x11`。
2. 通过 `svc` 进入内核，请求 `sys_clone(stack)` 创建 child task。

内核侧 [src/sys.c](src/sys.c)：

```c
int sys_clone(unsigned long stack) {
	return copy_process(0, 0, 0, stack);
}
```

`copy_process()` 看到不是 `PF_KTHREAD` 后，会按用户任务路径准备 child 的 `pt_regs`：

```text
childregs->regs[0] = 0
childregs->sp = stack + PAGE_SIZE
p->stack = stack
```

返回用户态后，`call_sys_clone()` 根据 `x0` 判断当前是 parent 还是 child：

```text
x0 != 0：parent，clone 返回
x0 == 0：child，跳到 thread_start
```

child 在 `thread_start` 中取回 `x10/x11`：

```text
x0 = x11
blr x10
```

于是 child 最终进入：

```text
user_process1("12345")
user_process1("abcd")
```

这就是 exp5 相比 exp4b 最大的行为变化：任务入口不再只由内核线程创建时写死，而是可以由用户态通过 syscall 请求 clone。

## 11. `sys_exit()` 和 `exit_process()`：用户任务退出

exp5 新增 `call_sys_exit()` 和 `sys_exit()`。

用户态任务结束时调用：

```c
call_sys_exit();
```

或者 child 线程从用户函数返回后，[src/sys.S](src/sys.S) 的 `thread_start` 会自动发起 `SYS_EXIT_NUMBER`：

```asm
mov x8, #SYS_EXIT_NUMBER
svc 0x0
```

内核侧 [src/sys.c](src/sys.c) 调用 [src/sched.c](src/sched.c) 中的 `exit_process()`：

```text
找到 current 在 task[] 中的位置
把 state 设为 TASK_ZOMBIE
释放 current->stack 指向的用户栈页
调用 schedule() 切走
```

调度器 `_schedule()` 只选择 `TASK_RUNNING` 的任务，所以退出后的任务不会再被调度。

## 12. exp5 的最终运行图

综合起来，exp5 的关键路径可以画成：

```text
kernel_main()
  -> copy_process(PF_KTHREAD, kernel_process)
  -> schedule()
       -> ret_from_fork
       -> kernel_process()
            -> move_to_user_mode(user_process)
       -> ret_to_user
       -> kernel_exit 0
       -> eret

EL0 user_process()
  -> call_sys_write("User process started")
  -> call_sys_malloc()
  -> call_sys_clone(user_process1, "12345", stack)
  -> call_sys_malloc()
  -> call_sys_clone(user_process1, "abcd", stack)
  -> call_sys_exit()

EL0 user_process1()
  -> call_sys_write()
  -> delay()
  -> timer interrupt
       -> el0_irq
       -> handle_irq()
       -> timer_tick()
       -> _schedule()
       -> kernel_exit 0
```

因此，exp5 的实验重点可以概括为：

```text
在 exp4b 已有抢占式调度的基础上，
让任务真正从 EL1 内核态走到 EL0 用户态，
并通过 syscall 完成用户态和内核态之间的受控交互。
```

## 13. 与当前 exp4b 目录的差异提醒

当前仓库里的 exp4b 还包含一些实验性增强，例如 debug 输出、priority 测试和 FP/SIMD 上下文保存说明。exp5 当前代码的主线没有继续展开这些调试输出和 FP/SIMD 测试，而是把重点转向用户态、系统调用和用户线程 clone。

所以阅读时可以把两者分工理解为：

```text
exp4b：调度器和 timer 抢占机制。
exp5 ：用户态切换、syscall、clone、exit。
```
