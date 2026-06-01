# exp5 demo 与真实 Linux 的联系和区别

这份文档总结 exp5 展示的核心逻辑，并说明它和真实 Linux 内核的关系。可以把 exp5 看成一个极简模型：它不是 Linux，但它用很少的代码演示了 Linux 里几个最重要的概念雏形。

## 1. exp5 demo 展示了什么

exp5 的主线是：

```text
内核启动
  -> 创建一个 kernel thread
  -> kernel thread 准备用户态现场
  -> eret 进入 EL0 用户态
  -> 用户态通过 svc 发起 syscall
  -> 内核根据 syscall number 分发到 sys_write/sys_malloc/sys_clone/sys_exit
  -> 用户态 clone 出两个用户线程
  -> timer interrupt 抢占调度这两个用户线程
```

所以 exp5 不只是“能打印字符”，它展示的是：

```text
1. 一个调度单位 task_struct 可以既有内核态执行路径，也有用户态执行路径。
2. 内核线程可以只需要 kernel stack。
3. 用户任务必须有 kernel stack，也必须有 user stack。
4. 用户态不能直接操作内核服务，需要通过 svc/syscall 进入内核。
5. syscall 返回时，内核通过 pt_regs 恢复用户态寄存器现场。
6. timer interrupt 可以打断用户态程序，并触发调度。
```

这个 demo 里的几个重要对象：

```text
task_struct:
  调度单位，保存 cpu_context、state、counter、priority、flags、stack 等。

kernel stack:
  每个 task 都有，位于 task page 中，用于 syscall、IRQ、调度和异常处理。

user stack:
  只有用户任务需要，用于 EL0 函数调用、局部变量和用户态执行。

pt_regs:
  放在内核栈顶部，保存异常返回到用户态所需的寄存器、sp、pc、pstate。

sys_call_table:
  syscall number 到内核函数的映射表。
```

## 2. 当前 demo 中实际有哪些任务

启动后首先有：

```text
task[0] init_task
```

`kernel_main()` 创建一个 kernel thread：

```text
task[1] kernel_process
```

这个任务开始时是内核线程，在 EL1 执行 `kernel_process()`。随后它调用 `move_to_user_mode(user_process)`，给自己分配 user stack，并准备 `pt_regs`：

```text
task[1]
  原来：kernel thread
  后来：进入 EL0 执行 user_process()
```

`user_process()` 再通过两次 `call_sys_clone()` 创建两个用户任务：

```text
task[2] user_process1("12345")
task[3] user_process1("abcd")
```

父用户进程 `task[1]` 创建完两个 child 后调用 `call_sys_exit()`，变成 `TASK_ZOMBIE`。之后主要运行的是：

```text
task[2]
task[3]
```

所以输出中看到很多 `12345` 和 `abcd` 连在一起，本质上是两个用户态任务被 timer interrupt 抢占调度。

## 3. 和 Linux 的联系

### 3.1 `task_struct` 的思想是相通的

Linux 也用 `task_struct` 表示调度单位。无论是进程还是线程，Linux 调度器真正调度的是 task。

exp5 中：

```text
task_struct
  -> cpu_context
  -> state
  -> counter
  -> priority
  -> stack
  -> flags
```

Linux 中的 `task_struct` 要复杂得多，还会包含或关联：

```text
调度信息
进程状态
内存地址空间 mm_struct
打开文件 files_struct
信号 signal_struct
凭据 cred
命名空间 namespace
内核栈
线程上下文 thread_struct
```

但核心思想一致：

```text
一个可调度实体，需要有自己的状态、上下文和内核栈。
```

### 3.2 用户态和内核态分离的思想是相通的

exp5 里：

```text
EL0: user_process/user_process1
EL1: syscall、IRQ、scheduler、sys_write
```

Linux 在 ARM64 上也是：

```text
EL0: 用户程序
EL1: Linux kernel
```

用户程序不能直接访问内核内部函数，也不能直接操作硬件。它必须通过 syscall、异常或中断进入内核。

exp5 中的 `svc #0` 就对应 Linux ARM64 上的系统调用入口机制。真实 Linux 也是用户态执行 `svc`，硬件进入 EL1，内核读取 syscall number，然后分发到对应的 syscall handler。

### 3.3 每个用户任务都有内核栈，这一点相通

exp5 中：

```text
kernel thread:
  task page + kernel stack

user task:
  task page + kernel stack
  user stack page
```

Linux 中用户进程也有用户栈和内核栈：

```text
用户栈:
  在用户虚拟地址空间中，运行用户代码时使用。

内核栈:
  每个 task 独立拥有，进入内核处理 syscall、page fault、interrupt 时使用。
```

这点非常关键：用户进程不能只拥有用户栈。只要它可能进入内核，就必须有可信的内核栈。

### 3.4 `pt_regs` 的思想是相通的

exp5 的 `pt_regs` 保存：

```text
x0-x30
sp
pc
pstate
```

Linux ARM64 也有 `struct pt_regs`，用于保存从用户态陷入内核时的寄存器现场。

用途也相似：

```text
syscall 参数读取
syscall 返回值写回
signal 处理
异常返回
调试和 ptrace
```

exp5 里 `childregs->regs[0] = 0` 模拟了 clone/fork 的一个经典语义：

```text
parent 返回 child pid
child 返回 0
```

Linux 的 fork/clone 也有类似返回值语义。

### 3.5 syscall table 的思想是相通的

exp5 中：

```c
void * const sys_call_table[] = {
    sys_write,
    sys_malloc,
    sys_clone,
    sys_exit
};
```

用户态把 syscall number 放进 `w8`，内核用它索引 `sys_call_table`。

Linux ARM64 也使用 syscall number 分发系统调用。真实 Linux 的 syscall 表更大，入口路径也更复杂，但抽象关系类似：

```text
syscall number
  -> syscall table
  -> sys_* handler
```

### 3.6 抢占和 timer interrupt 的思想是相通的

exp5 中 timer interrupt 到来后：

```text
el0_irq/el1_irq
  -> handle_irq()
  -> timer_tick()
  -> _schedule()
```

Linux 也依赖时钟中断、调度 tick 或高精度定时器来更新调度状态，并在合适时机触发重新调度。

联系在于：

```text
用户程序不是自己主动让出 CPU；
内核可以通过中断夺回控制权，并切换到其他任务。
```

## 4. 和 Linux 的主要区别

### 4.1 exp5 没有虚拟内存隔离

这是最大区别之一。

exp5 中 `get_free_page()` 返回的是简单物理地址范围里的页。用户任务拿到的 `stack` 也是一个直接地址：

```c
regs->sp = stack + PAGE_SIZE;
```

当前 demo 没有真正建立每个进程独立的页表，也没有用户地址空间隔离。

Linux 中每个用户进程通常有自己的虚拟地址空间：

```text
用户代码看到的是虚拟地址；
内核通过页表控制哪些地址可访问；
不同进程的相同虚拟地址可以映射到不同物理页；
用户态不能随便访问内核内存。
```

所以 exp5 只是演示 EL0/EL1 切换和 syscall 机制，还没有达到真实进程隔离。

### 4.2 exp5 的 clone 更像“创建用户线程”，不是完整 Linux clone

exp5 的 `sys_clone(stack)` 做得很少：

```text
分配 task_struct + kernel stack
设置 child 的用户栈
让 child 从 clone 返回 0
返回后在用户态 thread_start 调用 fn(arg)
```

真实 Linux `clone()` 可以通过 flags 精确控制资源共享关系，例如：

```text
是否共享地址空间
是否共享文件表
是否共享信号处理
是否共享 fs 信息
是否创建 thread group
TLS 设置
child tid / parent tid
退出信号
```

exp5 没有这些。它更像是为了教学，把用户线程创建过程拆成最小可理解版本。

### 4.3 exp5 没有 `mm_struct`、进程地址空间和 ELF 加载

exp5 的用户函数 `user_process()` 其实仍然是和内核一起编译进同一个镜像里的 C 函数。`move_to_user_mode()` 只是把 `pc` 设置成这个函数地址：

```c
regs->pc = (unsigned long)&user_process;
```

真实 Linux 创建用户进程通常会：

```text
加载 ELF 文件
创建用户虚拟地址空间
映射 text/data/bss/heap/stack
设置 argc/argv/envp
设置动态链接器
返回用户态入口
```

exp5 没有文件系统，也没有 ELF loader，所以它不是真正意义上的“运行一个用户程序文件”。

### 4.4 exp5 的调度器极简

exp5 调度器核心是：

```text
遍历 task[]
选择 counter 最大的 TASK_RUNNING 任务
counter 用完后按 priority 充值
```

真实 Linux 使用 CFS/EEVDF 等复杂调度机制，考虑：

```text
公平性
vruntime/deadline
多核负载均衡
NUMA
实时调度类
cgroup
CPU affinity
睡眠/唤醒
优先级继承
抢占模型
```

exp5 的调度器足够演示“timer 抢占 + context switch”，但不是生产级调度。

### 4.5 exp5 的 syscall 参数和安全检查很少

exp5 的 `sys_write(char *buf)` 直接：

```c
printf(buf);
```

真实 Linux 不会直接信任用户指针。它会使用类似 `copy_from_user()` 的机制安全地从用户地址空间拷贝数据，并检查权限和地址合法性。

exp5 没有：

```text
用户指针校验
地址权限检查
page fault 恢复
copy_from_user/copy_to_user
安全边界
```

所以它展示的是 syscall 控制流，不是完整安全模型。

### 4.6 exp5 的任务生命周期很简单

exp5 中 `exit_process()` 只做：

```text
state = TASK_ZOMBIE
free_page(user stack)
schedule()
```

真实 Linux 退出进程要复杂得多：

```text
关闭文件
释放地址空间
释放信号和线程组资源
通知父进程
保留 zombie 供 wait()
释放内核栈和 task_struct
处理 cgroup、namespace、ptrace、futex 等
```

exp5 中 zombie task 甚至还留在 `task[]` 里，没有完整回收 `task_struct` 所在页。

### 4.7 exp5 没有多核并发模型

exp5 启动时只让 core 0 继续运行，其他 core 停住。因此调度发生在单核上。

真实 Linux 是 SMP 内核，需要处理：

```text
多核同时调度
跨 CPU 唤醒
IPI
runqueue lock
per-cpu 数据
内存屏障
cache 一致性
负载均衡
```

exp5 避开了这些复杂性。

## 5. 一个对应关系表

```text
exp5                         Linux
------------------------------------------------------------
task_struct                  task_struct
cpu_context                  thread_struct / switch context
pt_regs                      struct pt_regs
PF_KTHREAD                   PF_KTHREAD / kthread
TASK_RUNNING                 TASK_RUNNING 等状态
TASK_ZOMBIE                  EXIT_ZOMBIE 等退出状态
kernel stack                 per-task kernel stack
user stack page              用户虚拟地址空间中的用户栈
svc #0                       syscall trap
w8 syscall number            x8 syscall number on ARM64
sys_call_table               syscall dispatch table
sys_write/sys_clone          Linux sys_* syscall handlers
timer_tick                   scheduler tick / timer interrupt
copy_process                 Linux copy_process() 的极简影子
move_to_user_mode            类似准备首次返回用户态的简化路径
```

## 6. 最重要的理解

exp5 最值得带走的理解是：

```text
进程/线程不是“纯用户态对象”。

一个用户任务平时在 EL0 跑用户代码；
一旦发生 syscall、interrupt、page fault，就进入 EL1；
进入 EL1 后仍然是在同一个 task 的上下文里，只是切换到了它自己的 kernel stack。
```

所以：

```text
kernel thread:
  只有内核态执行路径，可以没有 user stack。

user task:
  有用户态执行路径，也有内核态异常/syscall 路径；
  因此既需要 user stack，也需要 kernel stack。
```

真实 Linux 把这个模型扩展到了完整的虚拟内存、文件系统、权限、安全检查、多核调度、复杂进程生命周期和丰富 syscall 集合。exp5 则保留了最小骨架，让你能直接看见：

```text
task 如何被调度；
EL0 如何通过 svc 进入 EL1；
内核如何保存现场并分发 syscall；
内核如何通过 eret 返回用户态；
用户任务为什么同时需要 user stack 和 kernel stack。
```
