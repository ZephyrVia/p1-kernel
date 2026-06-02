# Exp6 启动流程新增内容梳理

本文从启动流程角度，对比 exp5，梳理 exp6 在内核启动、地址空间建立、进入用户态以及用户进程 fork 方面新增的内容。

## 1. 总体变化

exp5 的启动流程大致是：

```text
_start
  -> 从 EL2/EL3 切到 EL1
  -> 清空 bss
  -> 设置栈
  -> kernel_main
  -> 初始化 UART/异常向量/定时器/中断
  -> 创建内核线程
  -> 内核线程切到 EL0
  -> 用户态通过 syscall/clone/exit 运行
```

exp6 在这个基础上增加了 MMU 和进程虚拟地址空间，因此启动流程变成：

```text
_start
  -> 从 EL2/EL3 切到 EL1
  -> 清空 bss
  -> 创建临时 identity mapping, 仅 QEMU
  -> 创建内核高地址页表
  -> 设置虚拟地址下的内核栈
  -> 设置 TTBR1_EL1/TCR_EL1/MAIR_EL1
  -> 打开 MMU
  -> kernel_main, 此时内核已经运行在高虚拟地址
  -> 初始化 UART/异常向量/定时器/中断
  -> 创建内核线程 kernel_process
  -> kernel_process 创建用户地址空间
  -> 把 .text.user 用户程序复制到用户页
  -> 设置 TTBR0_EL1 为当前进程页表
  -> 通过 eret 进入 EL0
  -> 用户态执行 fork
  -> fork 复制用户虚拟内存并切换独立页表
```

核心区别是：exp5 主要是在物理地址/简单用户态 syscall 上运行；exp6 在启动过程中就建立页表并开启 MMU，随后每个用户进程都有自己的 TTBR0 用户地址空间。

## 2. 早期启动：仍然先完成 EL 切换

入口仍然在 [src/boot.S::_start](src/boot.S#L8)。

新增流程没有改变最开始的 CPU 选择和 EL 切换逻辑：

1. 读取 `mpidr_el1`，只让主核继续执行。
2. 非主核进入死循环。
3. 主核先关闭 MMU。
4. 根据当前 EL 判断是从 EL2 还是 EL3 启动。
5. 设置 `hcr_el2`、`scr_el3`、`spsr_el2/spsr_el3`、`elr_el2/elr_el3`。
6. 通过 `eret` 进入 EL1 的 `el1_entry`。

这一部分与 exp5 的作用基本一致：保证内核主逻辑运行在 EL1。

exp6 的新增点在于：进入 EL1 后，不再直接设置物理栈并调用 `kernel_main`，而是先准备虚拟内存环境。

## 3. 清空 bss 后，开始准备 MMU

进入 [src/boot.S::el1_entry](src/boot.S#L55) 后，exp6 仍然先清空 `.bss`：

```text
adr x0, bss_begin
adr x1, bss_end
sub x1, x1, x0
bl  memzero
```

从这里开始，exp6 相比 exp5 增加了 MMU 相关启动步骤。

## 4. QEMU 下新增 identity mapping

在 `USE_QEMU` 情况下，exp6 会先调用 [src/boot.S::__create_idmap](src/boot.S#L160)：

```text
bl __create_idmap
adrp x0, idmap_dir
msr ttbr0_el1, x0
```

这一步是 exp6 新增的 QEMU 兼容处理。

原因是：打开 MMU 的瞬间，QEMU 可能仍然用打开 MMU 前的物理地址继续取后续几条指令。如果只建立高地址内核映射，物理地址形式的取指会找不到映射，从而触发 prefetch abort。

因此 exp6 在 QEMU 下额外建立一套 identity mapping：

```text
虚拟地址 0x00000000... -> 物理地址 0x00000000...
```

从设计意图看，这套映射想覆盖 `0x00000000` 到 `PHYS_MEMORY_SIZE`，其中 `PHYS_MEMORY_SIZE = 0x40000000`，见 [include/mm.h](include/mm.h#L8)。也就是希望把 QEMU 下的 1GB 低端物理地址做成同值虚拟地址映射。

不过按当前代码实际计算，`__create_idmap` 传给 [create_block_map](src/boot.S#L144) 的结束地址是 `PHYS_MEMORY_SIZE`，见 [src/boot.S::__create_idmap](src/boot.S#L171)。而 `create_block_map` 会先把 `start/end` 右移 `SECTION_SHIFT`，再用 `PTRS_PER_TABLE - 1` 取低 9 位：

```text
start = 0x00000000 >> 21 = 0
end   = 0x40000000 >> 21 = 512
end   = 512 & 511 = 0
```

所以这段代码实际只会填 PMD 的第 0 项，映射范围是：

```text
VA 0x00000000 - 0x001fffff  ->  PA 0x00000000 - 0x001fffff
```

也就是低端 2MB identity mapping。由于 QEMU 的 kernel 默认加载地址在 `0x80000`，这个 2MB 范围已经覆盖了打开 MMU 瞬间继续取指所需的地址，所以这个补丁映射仍然能起到过渡作用。如果要严格映射完整 1GB，`__create_idmap` 这里的结束地址应该和内核映射类似，使用 `PHYS_MEMORY_SIZE - SECTION_SIZE`。

这套页表放在 [src/linker-qemu.ld::idmap_dir](src/linker-qemu.ld#L20)，临时写入 `ttbr0_el1`。它的作用只是帮助 CPU 平稳跨过“开启 MMU”这个瞬间。

这个补丁映射没有被显式销毁。它的结束方式是：后续创建第一个用户进程时，[move_to_user_mode](src/fork.c#L56) 会调用 [set_pgd(current->mm.pgd)](src/fork.c#L70)，把 `TTBR0_EL1` 从 `idmap_dir` 改成当前用户进程的页表；调度切换时，[switch_to](src/sched.c#L59) 也会再次调用 [set_pgd(next->mm.pgd)](src/sched.c#L65)。因此 idmap 页表内存仍保留在镜像里，但 CPU 不再通过 `TTBR0_EL1` 使用它。

## 5. 新增内核高地址页表

exp6 新增 [src/boot.S::__create_page_tables](src/boot.S#L180)，用于建立内核页表。

主要做了两类映射：

```text
VA_START + 0x00000000      -> 物理内存 0x00000000
VA_START + DEVICE_BASE     -> 外设物理地址 DEVICE_BASE
```

其中这些常量定义在 [include/mm.h](include/mm.h#L6) 和 [include/peripherals/base.h](include/peripherals/base.h#L6)：

```c
#define VA_START 0xffff000000000000
#define DEVICE_BASE 0x3F000000
```

也就是说，开启 MMU 后，内核不再直接使用低物理地址，而是运行在高虚拟地址：

```text
0xffff000000000000 + physical_offset
```

外设基地址也随之变成，见 [include/peripherals/base.h](include/peripherals/base.h#L6)：

```c
#define PBASE (VA_START + DEVICE_BASE)
#define LPBASE (VA_START + 0x40000000)
```

所以 UART、timer、IRQ 等驱动代码虽然仍然使用 `PBASE + offset`，但这个 `PBASE` 已经是虚拟地址。

## 6. 新增 MMU 控制寄存器初始化

页表建好后，exp6 在 [src/boot.S](src/boot.S#L91) 中新增以下关键寄存器配置：

```text
msr ttbr1_el1, x0
msr tcr_el1, x0
msr mair_el1, x0
msr sctlr_el1, x0
```

含义如下：

| 寄存器 | exp6 中的作用 | 源码位置 |
| --- | --- | --- |
| `TTBR1_EL1` | 保存内核高地址页表 `pg_dir` | [src/boot.S](src/boot.S#L91) |
| `TTBR0_EL1` | 保存用户进程页表，QEMU 早期也临时放 identity mapping | [src/boot.S](src/boot.S#L81), [src/utils.S](src/utils.S#L22) |
| `TCR_EL1` | 配置地址翻译参数，例如 48-bit VA、4KB 页 | [src/boot.S](src/boot.S#L95), [include/arm/mmu.h](include/arm/mmu.h#L28) |
| `MAIR_EL1` | 配置 normal memory 和 device memory 的属性 | [src/boot.S](src/boot.S#L98), [include/arm/mmu.h](include/arm/mmu.h#L10) |
| `SCTLR_EL1` | 最后打开 MMU | [src/boot.S](src/boot.S#L103), [include/arm/sysregs.h](include/arm/sysregs.h#L4) |

exp5 没有这个阶段；它设置完栈后就直接进入 `kernel_main`。

## 7. 内核栈改为虚拟地址

exp5 中栈大致直接设置到低地址：

```text
mov sp, #LOW_MEMORY
```

exp6 中改为，见 [src/boot.S](src/boot.S#L88)：

```text
mov x0, #VA_START
add sp, x0, #LOW_MEMORY
```

也就是栈地址变成：

```text
VA_START + LOW_MEMORY
```

这说明在进入 `kernel_main` 前，exp6 已经准备让内核完全运行在高虚拟地址环境中。

## 8. 链接脚本新增高地址布局和用户段

exp6 的链接脚本也随启动流程一起变化，QEMU 版本见 [src/linker-qemu.ld](src/linker-qemu.ld#L1)，树莓派版本见 [src/linker.ld](src/linker.ld#L1)。

QEMU 链接脚本起始地址变为，见 [src/linker-qemu.ld](src/linker-qemu.ld#L3)：

```ld
. = 0xffff000000080000;
```

同时新增用户程序段，见 [src/linker-qemu.ld](src/linker-qemu.ld#L6)：

```ld
user_begin = .;
.text.user : { build/user* (.text) }
.rodata.user : { build/user* (.rodata) }
.data.user : { build/user* (.data) }
.bss.user : { build/user* (.bss) }
user_end = .;
```

这说明 exp6 把用户程序代码单独收集起来。启动用户进程时，内核可以通过 `user_begin` 和 `user_end` 找到这段用户程序，并复制到用户地址空间。

链接脚本还新增页表空间，见 [src/linker-qemu.ld](src/linker-qemu.ld#L19)：

```ld
idmap_dir = .;
.data.idmapd : { . += (3 * (1 << 12)); }

pg_dir = .;
.data.pgd : { . += (3 * (1 << 12)); }
```

其中：

- `idmap_dir`：QEMU 早期开 MMU 用的 identity mapping 页表。
- `pg_dir`：内核高地址页表。

## 9. kernel_main 运行时，MMU 已经打开

进入 `kernel_main` 时，exp6 已经完成：

1. 内核页表创建。
2. 设备内存映射。
3. 内核栈切换到高虚拟地址。
4. `TTBR1_EL1/TCR_EL1/MAIR_EL1` 配置。
5. `SCTLR_EL1.M` 打开 MMU。

因此 [src/kernel.c::kernel_main](src/kernel.c#L27) 中的初始化代码已经运行在虚拟地址环境下：

```c
uart_init();
irq_vector_init();
timer_init();
enable_interrupt_controller();
enable_irq();
copy_process(PF_KTHREAD, (unsigned long)&kernel_process, 0);
```

这部分看起来和 exp5 类似，但底层地址含义已经不同：所有内核指针和外设访问都经过 MMU 映射。

## 10. 创建内核线程后，准备第一个用户进程

exp6 的 [kernel_main](src/kernel.c#L27) 创建一个内核线程 [kernel_process](src/kernel.c#L15)。

[kernel_process](src/kernel.c#L15) 新增的关键逻辑是：

```c
unsigned long begin = (unsigned long)&user_begin;
unsigned long end = (unsigned long)&user_end;
unsigned long process = (unsigned long)&user_process;
move_to_user_mode(begin, end - begin, process - begin);
```

这与 exp5 不同。

exp5 中用户函数通常仍然以内核地址中的函数指针形式传给 `move_to_user_mode`。exp6 则把用户程序当作一段独立的用户镜像：

```text
user_begin ... user_end
```

然后将这段代码复制到用户虚拟地址空间。

## 11. move_to_user_mode 新增用户页表创建

exp6 的 [move_to_user_mode(start, size, pc)](src/fork.c#L56) 做了更多事情：

1. 设置当前任务的 `pt_regs`。
2. 设置返回 EL0 后的 `pstate = PSR_MODE_EL0t`。
3. 设置用户态入口 `pc`。
4. 设置用户栈 `sp = 2 * PAGE_SIZE`。
5. 通过 [allocate_user_page](src/mm.c#L17) 为用户虚拟地址 0 分配代码页。
6. 把 `.text.user` 复制到这页，复制入口见 [src/fork.c](src/fork.c#L69)。
7. 通过 [set_pgd](src/utils.S#L22) 把当前任务页表写入 `TTBR0_EL1`。

也就是说，exp6 进入用户态前，已经为该任务建立了自己的用户页表：

```text
用户 VA 0x0000 -> 用户代码页
用户 VA 0x1000 或附近 -> 后续可能作为用户栈页，按需缺页分配
```

这里 `TTBR1_EL1` 负责内核高地址空间，`TTBR0_EL1` 负责当前用户进程低地址空间。

## 12. 异常向量新增 EL0 data abort 处理

exp5 的 EL0 同步异常主要处理 `svc` 系统调用。exp6 的 EL0 同步异常入口见 [src/entry.S::el0_sync](src/entry.S#L158)。

exp6 中 `el0_sync` 新增 data abort 分支：

```text
cmp x24, #ESR_ELx_EC_SVC64
b.eq el0_svc
cmp x24, #ESR_ELx_EC_DABT_LOW
b.eq el0_da
```

当用户态访问未映射地址时，会进入 [src/entry.S::el0_da](src/entry.S#L190)：

```text
el0_da:
  mrs x0, far_el1
  mrs x1, esr_el1
  bl do_mem_abort
```

[do_mem_abort](src/mm.c#L131) 会判断异常是否是 translation fault。如果是，就给当前进程分配一个物理页，并通过 [map_page](src/mm.c#L87) 映射到触发异常的用户虚拟地址。

这个机制让 exp6 支持简单的按需分配。典型用途是用户栈：启动时只映射代码页，用户栈页可以等第一次访问时再分配。

## 13. 用户态启动后新增 fork 流程

exp6 的用户程序在 [src/user.c::user_process](src/user.c#L17) 中：

```c
call_sys_write("User process\n\r");
int pid = call_sys_fork();
if (pid == 0) {
    loop("abcde");
} else {
    loop("12345");
}
```

对应 syscall 表见 [src/sys.c](src/sys.c#L20)：

```c
void * const sys_call_table[] = {
    sys_write,
    sys_fork,
    sys_exit
};
```

[sys_fork()](src/sys.c#L12) 调用：

```c
copy_process(0, 0, 0);
```

相比 exp5 的 clone，exp6 的 fork 不再要求用户手动传入新栈页。它会复制当前进程已有的用户虚拟内存。

## 14. fork 时复制用户地址空间

exp6 的 [task_struct](include/sched.h#L57) 新增 [mm_struct](include/sched.h#L48)：

```c
struct mm_struct {
    unsigned long pgd;
    int user_pages_count;
    struct user_page user_pages[MAX_PROCESS_PAGES];
    int kernel_pages_count;
    unsigned long kernel_pages[MAX_PROCESS_PAGES];
};
```

fork 时，主要逻辑在 [copy_process](src/fork.c#L7) 和 [copy_virt_memory](src/mm.c#L117)：

1. 为子进程分配新的 task page。
2. 复制当前进程的寄存器上下文。
3. 将子进程 syscall 返回值设置为 0。
4. 调用 `copy_virt_memory(p)`。
5. 为子进程建立自己的页表。
6. 把父进程用户页内容复制到子进程用户页。
7. 子进程返回用户态后，从 fork 返回处继续执行。

因此 exp6 的 fork 已经具有“父子进程拥有各自用户页表和用户内存副本”的雏形。

## 15. 调度切换时新增 TTBR0 切换

exp5 的 `switch_to` 主要只切换 CPU 上下文。exp6 的实现见 [src/sched.c::switch_to](src/sched.c#L59)。

exp6 在 `switch_to` 中新增：

```c
set_pgd(next->mm.pgd);
cpu_switch_to(prev, next);
```

[set_pgd](src/utils.S#L22) 会把下一个任务的页表写入 `TTBR0_EL1` 并刷新 TLB：

```text
msr ttbr0_el1, x0
tlbi vmalle1is
dsb ish
isb
```

这一步非常关键：它让不同用户进程在调度切换时看到不同的低地址用户空间。

内核空间仍然由 `TTBR1_EL1` 提供，所以所有进程共享同一个高地址内核映射。

## 16. 从启动角度总结新增内容

从启动流程看，exp6 相比 exp5 新增了以下内容：

1. 新增 MMU 常量和页表描述符定义。
2. 启动早期新增 QEMU identity mapping。
3. 启动早期新增内核高地址页表创建。
4. 链接地址从低地址改为高虚拟地址。
5. 内核栈从物理地址改为高虚拟地址。
6. 新增 `TTBR1_EL1/TCR_EL1/MAIR_EL1/SCTLR_EL1` 配置并打开 MMU。
7. 外设基地址改为高虚拟地址。
8. 链接脚本新增 `.text.user` 等用户程序段。
9. 内核启动用户进程时，先创建用户页表，再复制用户程序。
10. 新增 `TTBR0_EL1` 表示当前用户进程地址空间。
11. EL0 同步异常新增 data abort/缺页处理。
12. syscall 从 clone 风格调整为 fork 风格。
13. fork 时复制用户虚拟内存。
14. 调度切换时同步切换当前任务的用户页表。

一句话概括：exp6 的启动流程不再只是“初始化硬件后进入用户态”，而是先把内核放进高地址虚拟空间，再为用户程序建立独立低地址空间，最后通过异常返回进入 EL0，并在后续 fork 和调度中维护每个进程自己的页表。
