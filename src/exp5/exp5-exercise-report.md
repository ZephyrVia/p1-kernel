# exp5 用户态、异常与系统调用实验报告

## 目标

本次在 `exp5` 中完成用户态和系统调用相关练习：

1. 从 kernel thread 切换到 EL0 用户态任务。
2. 通过 `svc` 实现基本系统调用路径。
3. 在用户态访问 EL1 system register，触发 synchronous exception。
4. 使用 `ESR_EL1` 区分 syscall 和非法 system register 访问。
5. 新增 `set_priority` 系统调用，允许运行中的任务动态修改自身 priority。

最终效果：

```text
用户任务可以通过 syscall 请求内核服务；
EL0 访问 sctlr_el1 会触发同步异常，并被识别为 SYSREG_ERROR；
用户任务运行中可以调用 set_priority 修改 current->priority；
priority 修改后，调度输出比例会动态变化。
```

## 1. 用户态任务与 syscall 基础

修改位置：

- [src/kernel.c](src/kernel.c)
- [src/fork.c](src/fork.c)
- [src/entry.S](src/entry.S)
- [include/fork.h](include/fork.h)
- [include/sys.h](include/sys.h)
- [src/sys.S](src/sys.S)
- [src/sys.c](src/sys.c)

`kernel_main()` 创建一个 kernel thread：

```c
copy_process(PF_KTHREAD, (unsigned long)&kernel_process, 0, 0);
```

`kernel_process()` 运行在 EL1，随后调用：

```c
move_to_user_mode((unsigned long)&user_process);
```

`move_to_user_mode()` 会为当前 task 准备 `pt_regs`：

```text
regs->pc = user_process
regs->pstate = PSR_MODE_EL0t
regs->sp = user_stack + PAGE_SIZE
```

之后从 `ret_from_fork` 走到 `kernel_exit 0`，最终通过 `eret` 进入 EL0 执行 `user_process()`。

用户态通过 [src/sys.S](src/sys.S) 中的 wrapper 发起 syscall。例如：

```asm
call_sys_write:
	mov w8, #SYS_WRITE_NUMBER
	svc #0
	ret
```

`svc #0` 进入 [src/entry.S](src/entry.S) 的 `el0_sync`，内核读取 `ESR_EL1`，确认是 `SVC64` 后进入 `el0_svc`，再根据 `w8` 作为 syscall number 查 `sys_call_table`。

## 2. 用户态访问 system register 异常

修改位置：

- [include/arm/sysregs.h](include/arm/sysregs.h)
- [include/entry.h](include/entry.h)
- [src/entry.S](src/entry.S)
- [src/irq.c](src/irq.c)
- [src/kernel.c](src/kernel.c)

新增 ESR exception class 定义：

```c
#define ESR_ELx_EC_UNKNOWN 0x00
#define ESR_ELx_EC_SYS64   0x18
```

新增异常类型：

```c
#define SYSREG_ERROR 18
```

测试函数位于 [src/kernel.c](src/kernel.c)：

```c
void user_sysreg_test(void)
{
	call_sys_write("Try reading sctlr_el1 from EL0\n\r");
	asm volatile("mrs x0, sctlr_el1");
	call_sys_write("Should not reach here\n\r");
	call_sys_exit();
}
```

`sctlr_el1` 是 EL1 system register，EL0 用户态不能访问。用户态执行：

```asm
mrs x0, sctlr_el1
```

CPU 会产生 synchronous exception，进入 `el0_sync`。

`el0_sync` 当前按 `ESR_EL1.EC` 分流：

```asm
cmp x24, #ESR_ELx_EC_SVC64
b.eq el0_svc

cmp x24, #ESR_ELx_EC_SYS64
b.eq el0_sys

cmp x24, #ESR_ELx_EC_UNKNOWN
b.eq el0_sys
```

因此 syscall 和非法 system register 访问可以区分：

```text
svc #0:
  ESR_EL1.EC = SVC64
  -> el0_svc

mrs x0, sctlr_el1:
  ESR_EL1.EC = SYS64 或 UNKNOWN
  -> el0_sys
```

`el0_sys` 会打印异常信息并停住：

```asm
el0_sys:
	mov x0, #SYSREG_ERROR
	mov x1, x25
	mrs x2, elr_el1
	bl show_invalid_entry_message
	b err_hang
```

当前 QEMU 运行中观察到：

```text
SYSREG_ERROR, ESR: 2000000, address: 80b6c
```

这里 `ESR_EL1.EC = 0`，因此代码同时处理 `ESR_ELx_EC_UNKNOWN`。

## 3. 新增动态 priority 系统调用

修改位置：

- [include/sys.h](include/sys.h)
- [src/sys.S](src/sys.S)
- [src/sys.c](src/sys.c)
- [src/kernel.c](src/kernel.c)
- [src/sched.c](src/sched.c)

在 [include/sys.h](include/sys.h) 中将 syscall 数量从 4 扩展到 5：

```c
#define __NR_syscalls             5
#define SYS_SET_PRIORITY_NUMBER   4
```

新增用户态 syscall wrapper：

```asm
call_sys_set_priority:
	mov w8, #SYS_SET_PRIORITY_NUMBER
	svc #0
	ret
```

内核实现位于 [src/sys.c](src/sys.c)：

```c
int sys_set_priority(long priority)
{
	if (priority < 1) {
		return -1;
	}
	preempt_disable();
	printf("[prio] task=%x priority %d -> %d\r\n",
	       current, current->priority, priority);
	current->priority = priority;
	current->counter = priority;
	preempt_enable();
	return 0;
}
```

这里同时修改：

```text
current->priority
current->counter
```

原因是：

```text
priority 决定后续时间片充值；
counter 决定当前剩余时间片。
```

如果只修改 `priority`，效果可能要等所有任务 counter 用完并重新充值后才明显。同步修改 `counter` 可以让变化更快体现在运行结果中。

syscall table 增加最后一项：

```c
void * const sys_call_table[] = {
	sys_write,
	sys_malloc,
	sys_clone,
	sys_exit,
	sys_set_priority
};
```

## 4. 动态 priority demo

修改位置：

- [src/kernel.c](src/kernel.c)

新增用户态参数结构：

```c
struct priority_task_arg {
	char *label;
	long initial_priority;
	long later_priority;
	int switch_after_rounds;
};
```

当前创建两个用户线程：

```c
static struct priority_task_arg priority_task_a = {"A", 1, 1, 0};
static struct priority_task_arg priority_task_b = {"B", 1, 5, 12};
```

`A` 一直保持 priority 1。`B` 初始 priority 也是 1，运行 12 轮后调用：

```c
call_sys_set_priority(5);
```

用户线程主体：

```c
void user_priority_process(struct priority_task_arg *arg)
{
	call_sys_set_priority(arg->initial_priority);

	while (1) {
		call_sys_write(arg->label);
		delay(DELAYS);
		if (需要切换 priority) {
			call_sys_set_priority(arg->later_priority);
		}
	}
}
```

调度器仍然使用 [src/sched.c](src/sched.c) 中原有逻辑：

```c
p->counter = (p->counter >> 1) + p->priority;
```

所以 `B` 的 priority 从 1 改为 5 后，后续会得到更多 `counter`，输出中 `B` 的连续运行块会变长。

## 5. 验证结果

编译：

```bash
make
```

运行：

```bash
timeout 10s make run
```

观察到：

```text
kernel boots...
Kernel process started. EL 1
User process started
Priority demo: B raises priority from 1 to 5 while running
[prio] task=403000 priority 1 -> 1
A priority=1
...
[prio] task=405000 priority 1 -> 1
B priority=1
...
[prio] task=405000 priority 1 -> 5
B priority=5
```

随后输出中 `B` 的连续字符块明显比 `A` 更长，例如：

```text
BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB...
AAAAAA...
BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB...
```

这说明：

```text
用户态任务运行期间调用 set_priority；
内核修改 current->priority 和 current->counter；
调度器后续按新的 priority 分配时间片；
priority 动态生效。
```

system register 异常验证可通过打开 [src/kernel.c](src/kernel.c) 中的：

```c
#define RUN_SYSREG_TEST 1
```

预期输出：

```text
Try reading sctlr_el1 from EL0
SYSREG_ERROR, ESR: 2000000, address: ...
```

## 结论

本次实验在 exp5 的用户态和 syscall 基础上完成了两个扩展：

```text
1. 使用 ESR_EL1 区分 syscall 和非法 system register 访问。
2. 新增 set_priority syscall，让用户任务可以动态改变自身调度权重。
```

通过这个实验可以看到：

```text
svc 和非法 system register 访问都会进入 el0_sync；
真正的分流依据是 ESR_EL1.EC；
syscall handler 可以安全地修改 current task 的内核状态；
调度器会在后续 tick 中体现这些动态状态变化。
```
