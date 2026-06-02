# exp5 用户态访问系统寄存器异常说明

这份文档解释本次改动的目的、为什么要这样改，以及用户态访问系统寄存器时同步异常是如何一步步触发和处理的。

## 1. 需求是什么

实验要求：

```text
When a task is executed in user mode, try to access some of the system registers.
Make sure that a synchronous exception is generated in this case.
Handle this exception, use esr_el1 register to distinguish it from a system call.
```

翻译成当前 exp5 的语境，就是：

```text
1. 让 EL0 用户态代码尝试访问 EL1 system register。
2. 证明这会触发 synchronous exception。
3. 在 el0_sync 异常入口里读取 ESR_EL1。
4. 用 ESR_EL1.EC 区分：
   - 这是 syscall，也就是 svc
   - 还是非法 system register 访问
```

这件事很适合 exp5，因为 exp5 已经有了用户态、syscall 和异常返回路径。现在要补的是：**syscall 不是唯一的 EL0 synchronous exception**。

## 2. 为什么用户态访问 system register 会异常

ARMv8-A 有异常级别：

```text
EL0: 用户态
EL1: 内核态
EL2: Hypervisor
EL3: Secure monitor
```

像 `sctlr_el1` 这种寄存器属于 EL1 system register，控制的是内核级系统行为，例如 MMU/cache 等。用户态 EL0 不能直接读写它。

本次测试代码在 [src/kernel.c](src/kernel.c) 中：

```c
asm volatile("mrs x0, sctlr_el1");
```

这条指令的意思是：

```text
读取 SCTLR_EL1 到 x0
```

但当前代码运行在 EL0，所以这条指令是非法的。CPU 执行到这里时不会继续往下跑，而是立刻产生 synchronous exception。

同步异常的意思是：

```text
异常由当前这条指令直接触发；
异常地址就是这条 faulting instruction 的地址；
处理完后理论上可以返回这条指令，或者跳过这条指令。
```

这和 timer interrupt 不同。timer interrupt 是异步中断，不是由当前指令本身触发的。

## 3. 为什么要改 `sysregs.h`

原来的 [include/arm/sysregs.h](include/arm/sysregs.h) 只有：

```c
#define ESR_ELx_EC_SHIFT 26
#define ESR_ELx_EC_SVC64 0x15
```

这只够识别 syscall：

```text
ESR_EL1.EC == 0x15 -> SVC64
```

为了识别非法 system instruction / system register 访问，新增：

```c
#define ESR_ELx_EC_UNKNOWN 0x00
#define ESR_ELx_EC_SYS64   0x18
```

其中：

```text
0x18:
  trapped MSR/MRS/System instruction execution in AArch64 state

0x00:
  Unknown reason / uncategorized exception
```

按 ARM 文档，很多 system register trap 可以归类为 `SYS64`。但当前 QEMU 上，EL0 执行 `mrs x0, sctlr_el1` 得到的 `ESR_EL1` 是：

```text
ESR: 0x02000000
```

取 EC 字段：

```text
EC = ESR_EL1 >> 26
   = 0x02000000 >> 26
   = 0
```

也就是 `UNKNOWN`，不是 `SYS64`。所以代码同时处理 `SYS64` 和 `UNKNOWN`，保证这个实验在当前 QEMU 行为下能正确识别。

## 4. 为什么要改 `entry.h` 和 `irq.c`

原来异常错误类型只到：

```c
#define SYNC_ERROR    16
#define SYSCALL_ERROR 17
```

本次新增：

```c
#define SYSREG_ERROR 18
```

目的是给非法 system register 访问一个单独的错误类型，而不是继续打印泛泛的 `SYNC_ERROR`。

同时 [src/irq.c](src/irq.c) 里的 `entry_error_messages[]` 也要同步增加：

```c
"SYNC_ERROR",
"SYSCALL_ERROR",
"SYSREG_ERROR"
```

否则 `show_invalid_entry_message(SYSREG_ERROR, ...)` 会用 `18` 作为数组下标，但数组没有对应字符串，可能越界。

## 5. 异常具体是如何触发的

测试入口是：

```text
kernel_process()
  -> move_to_user_mode(user_sysreg_test)
  -> kernel_exit 0
  -> eret
  -> EL0 user_sysreg_test()
```

进入用户态后，测试函数先打印：

```c
tfp_sprintf(buf, "Try reading sctlr_el1 from EL0\n\r");
call_sys_write(buf);
```

然后执行：

```asm
mrs x0, sctlr_el1
```

CPU 发现：

```text
当前异常级别是 EL0；
指令试图读取 EL1 system register；
权限不允许；
产生 synchronous exception。
```

硬件自动做几件事：

```text
1. 把 faulting instruction 的地址保存到 ELR_EL1。
2. 把异常原因编码到 ESR_EL1。
3. 把当前 PSTATE 保存到 SPSR_EL1。
4. 从 EL0 切到 EL1。
5. 根据 VBAR_EL1 找到异常向量表。
6. 跳到 EL0 64-bit synchronous exception 入口。
```

在当前代码中，异常向量表对应入口是 [src/entry.S](src/entry.S)：

```asm
ventry el0_sync
```

所以 CPU 最终进入：

```asm
el0_sync:
```

## 6. `el0_sync` 如何区分 syscall 和 system register 异常

原来的 `el0_sync` 逻辑是：

```asm
el0_sync:
    kernel_entry 0
    mrs x25, esr_el1
    lsr x24, x25, #ESR_ELx_EC_SHIFT
    cmp x24, #ESR_ELx_EC_SVC64
    b.eq el0_svc
    handle_invalid_entry 0, SYNC_ERROR
```

这只认识 syscall。只要不是 `SVC64`，全部算 `SYNC_ERROR`。

现在改成：

```asm
el0_sync:
    kernel_entry 0
    mrs x25, esr_el1
    lsr x24, x25, #ESR_ELx_EC_SHIFT

    cmp x24, #ESR_ELx_EC_SVC64
    b.eq el0_svc

    cmp x24, #ESR_ELx_EC_SYS64
    b.eq el0_sys

    cmp x24, #ESR_ELx_EC_UNKNOWN
    b.eq el0_sys

    handle_invalid_entry 0, SYNC_ERROR
```

含义是：

```text
ESR_EL1.EC == 0x15:
  这是 svc syscall，进入 el0_svc。

ESR_EL1.EC == 0x18:
  这是 trapped system instruction，进入 el0_sys。

ESR_EL1.EC == 0x00:
  当前 QEMU 对 mrs sctlr_el1 给出的结果，也进入 el0_sys。

其他同步异常:
  仍然打印 SYNC_ERROR。
```

这正好满足实验要求：**用 `ESR_EL1` 把 system register fault 和 syscall 区分开**。

## 7. `el0_sys` 做了什么

新增的处理分支：

```asm
el0_sys:
    mov x0, #SYSREG_ERROR
    mov x1, x25
    mrs x2, elr_el1
    bl show_invalid_entry_message
    b err_hang
```

传给 `show_invalid_entry_message()` 的参数是：

```text
x0 = SYSREG_ERROR
x1 = ESR_EL1 的原始值
x2 = ELR_EL1，也就是触发异常的指令地址
```

所以最终会打印类似：

```text
SYSREG_ERROR, ESR: 2000000, address: 80b6c
```

这里选择 `b err_hang` 是为了教学验证：

```text
异常发生后直接停住；
输出 ESR 和 faulting PC；
不会继续运行，也不会误以为非法指令执行成功。
```

如果将来想让程序继续执行，可以在 `el0_sys` 中修改保存的 `ELR_EL1`，让它加 4 跳过 faulting instruction，然后 `kernel_exit 0` 返回用户态。但当前实验只要求证明异常生成并被识别，所以停住更直观。

## 8. 为什么输出是这样

运行结果：

```text
kernel boots...
Kernel process started. EL 1
Try reading sctlr_el1 from EL0
SYSREG_ERROR, ESR: 2000000, address: 80b6c
```

逐行解释：

```text
kernel boots...
  kernel_main() 启动。

Kernel process started. EL 1
  kernel_process() 仍在 EL1 运行。

Try reading sctlr_el1 from EL0
  已经进入 user_sysreg_test()，通过 syscall 打印提示。

SYSREG_ERROR, ESR: 2000000, address: 80b6c
  EL0 执行 mrs x0, sctlr_el1 后触发 synchronous exception；
  el0_sync 读取 ESR_EL1；
  发现它不是 SVC64；
  进入 el0_sys；
  打印异常类型、ESR 和 faulting instruction 地址。
```

没有出现：

```text
Should not reach here
```

这说明 `mrs x0, sctlr_el1` 没有在用户态成功执行，异常确实发生了。

## 9. 和 syscall 的区别

syscall 路径：

```text
EL0:
  svc #0

EL1:
  el0_sync
  ESR_EL1.EC == SVC64
  el0_svc
  sys_call_table[w8]
  sys_write/sys_malloc/sys_clone/sys_exit
  kernel_exit 0
  eret
```

非法 system register 访问路径：

```text
EL0:
  mrs x0, sctlr_el1

EL1:
  el0_sync
  ESR_EL1.EC == SYS64 or UNKNOWN
  el0_sys
  show_invalid_entry_message(SYSREG_ERROR, esr, elr)
  err_hang
```

两者共同点：

```text
都是 EL0 synchronous exception；
都会进入 el0_sync；
都会保存用户态寄存器现场；
都会通过 ESR_EL1 判断异常原因。
```

两者区别：

```text
svc 是用户态主动请求内核服务，是合法入口；
访问 sctlr_el1 是用户态越权访问，是非法操作。
```

所以 exp5 现在展示了一个很重要的内核概念：

```text
异常入口可以相同；
异常原因不同；
内核必须读取 ESR_EL1，把不同异常分发到不同处理路径。
```
