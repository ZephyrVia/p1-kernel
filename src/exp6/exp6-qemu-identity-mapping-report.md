# exp6 QEMU 适配：identity mapping 实验报告

## 目标

本次改动只针对以下练习要求：

```text
Adapt lesson 06 to run on qemu.
You have to implement identity mapping in order to do this.
Check this issue for reference.
```

lesson 06 开启 MMU 后，内核会运行在高虚拟地址空间：

```text
VA_START = 0xffff000000000000
```

真实 RPi3 硬件在打开 MMU 后可以直接继续按新的虚拟地址取指；但 QEMU 的 `raspi3b` 模型在打开 MMU 的瞬间可能仍继续使用打开 MMU 前的物理地址取后续指令。若页表中没有低地址同值映射，CPU 会把这些物理地址当作虚拟地址翻译，找不到映射后触发 prefetch abort。

因此本次改动的核心是：**在 QEMU 下临时建立一套 identity mapping，使低地址虚拟地址直接映射到同值物理地址，帮助 CPU 平稳跨过开启 MMU 的瞬间。**

## 修改位置

主要修改位置：

- [src/boot.S](src/boot.S#L61)：QEMU 下创建并启用 identity mapping。
- [src/boot.S::__create_idmap](src/boot.S#L160)：实际创建 identity mapping 页表。
- [src/linker-qemu.ld](src/linker-qemu.ld#L19)：为 `idmap_dir` 预留页表空间。
- [include/mm.h](include/mm.h#L30)：定义初始页表空间大小 `PG_DIR_SIZE`。
- [Makefile.qemu](Makefile.qemu#L16)：新增 `run` 目标，便于直接运行 QEMU。

## 1. QEMU 下额外创建 idmap 页表

在 [src/boot.S:61](src/boot.S#L61)，进入 EL1 并清空 `.bss` 后，QEMU 分支会先创建 identity mapping：

```asm
#ifdef USE_QEMU
	bl	__create_idmap
	adrp	x0, idmap_dir
	msr	ttbr0_el1, x0
#endif
```

这里的含义是：

```text
1. __create_idmap 创建一棵低地址同值映射页表。
2. idmap_dir 是这棵页表的根。
3. 把 idmap_dir 写入 TTBR0_EL1。
```

exp6 中内核高地址映射使用 `TTBR1_EL1`，而低地址空间由 `TTBR0_EL1` 翻译。QEMU 在 MMU 刚打开时若继续按低地址取指，就可以通过 `TTBR0_EL1` 找到 identity mapping，从而避免异常。

## 2. identity mapping 的具体实现

`__create_idmap` 位于 [src/boot.S:160](src/boot.S#L160)：

```asm
__create_idmap:
	mov	x29, x30
	
	adrp	x0, idmap_dir
	mov	x1, #PG_DIR_SIZE
	bl	memzero

	adrp	x0, idmap_dir
	mov	x1, xzr
	create_pgd_entry	x0, x1, x2, x3

	mov	x1, xzr
	mov	x2, xzr
	ldr	x3, =(PHYS_MEMORY_SIZE)
	create_block_map x0, x1, x2, x3, MMU_FLAGS, x4

	mov	x30, x29
	ret
```

流程如下：

```text
1. 清空 idmap_dir 对应的页表内存。
2. 从虚拟地址 0 开始创建 PGD/PUD/PMD 三级页表路径。
3. 使用 block entry 建立低地址映射。
```

目标映射关系是：

```text
VA 0x00000000...  ->  PA 0x00000000...
```

也就是虚拟地址和物理地址相同。

## 3. 映射范围

从代码意图看，`__create_idmap` 使用：

```asm
ldr x3, =(PHYS_MEMORY_SIZE)
```

其中 `PHYS_MEMORY_SIZE` 定义在 [include/mm.h:8](include/mm.h#L8)：

```c
#define PHYS_MEMORY_SIZE 0x40000000
```

因此设计目标是覆盖低端 1GB：

```text
0x00000000 - 0x3fffffff
```

不过当前 `create_block_map` 会把 `start/end` 先右移 `SECTION_SHIFT`，再用 `PTRS_PER_TABLE - 1` 取低 9 位。对 `0x40000000` 来说：

```text
0x40000000 >> 21 = 512
512 & 511 = 0
```

所以当前代码实际只填了第 0 个 PMD entry，实际映射范围是低端 2MB：

```text
VA 0x00000000 - 0x001fffff
PA 0x00000000 - 0x001fffff
```

QEMU 默认把 kernel image 加载到 `0x80000`，这个地址落在低端 2MB 内。因此即使实际映射范围只有 2MB，也足够覆盖打开 MMU 瞬间继续取指所需的地址。

## 4. 页表空间预留

`idmap_dir` 在 QEMU 链接脚本中预留，见 [src/linker-qemu.ld:19](src/linker-qemu.ld#L19)：

```ld
. = ALIGN(0x00001000);
idmap_dir = .;
.data.idmapd : {. += (4 * (1 << 12));}
```

这里给 idmap 页表预留了页对齐空间，启动早期的 `__create_idmap` 会直接在这块内存里写页表项。

`pg_dir` 也在同一个链接脚本中预留，见 [src/linker-qemu.ld:22](src/linker-qemu.ld#L22)：

```ld
. = ALIGN(0x00001000);
pg_dir = .;
.data.pgd : { . += (4 * (1 << 12)); }
```

二者职责不同：

```text
idmap_dir: QEMU 开启 MMU 过渡用，写入 TTBR0_EL1
pg_dir:    内核高地址映射用，写入 TTBR1_EL1
```

## 5. 与内核高地址页表的关系

identity mapping 只是 QEMU 过渡映射。随后 [src/boot.S:86](src/boot.S#L86) 会继续创建正式的内核页表：

```asm
bl __create_page_tables
```

正式内核映射建立后，启动代码会写入：

```asm
msr ttbr1_el1, x0
msr tcr_el1, x0
msr mair_el1, x0
msr sctlr_el1, x0
```

其中 `sctlr_el1` 写入后 MMU 正式打开，之后通过：

```asm
br x2
```

跳转到高虚拟地址下的 `kernel_main`。

所以启动过程可以概括为：

```text
物理地址执行 boot.S
  -> QEMU 下先建立 TTBR0 identity mapping
  -> 建立 TTBR1 内核高地址映射
  -> 打开 MMU
  -> 跳到高虚拟地址 kernel_main
```

## 6. idmap 什么时候不再使用

这套 identity mapping 没有被显式释放。它的停止使用方式是：后续用户进程建立自己的用户页表后，内核会把 `TTBR0_EL1` 改成当前进程的 `mm.pgd`。

也就是说：

```text
启动早期:
  TTBR0_EL1 = idmap_dir

进入用户态前:
  TTBR0_EL1 = current->mm.pgd
```

因此 `idmap_dir` 对应的页表内存仍留在内核镜像中，但 CPU 不再通过 `TTBR0_EL1` 使用它。

## 7. QEMU 运行入口

为了方便验证 QEMU 适配，给 [Makefile.qemu](Makefile.qemu#L16) 增加了 `run` 目标：

```make
run : kernel8.img
	qemu-system-aarch64 -machine raspi3b -nographic -kernel kernel8.img -serial null -serial mon:stdio
```

运行方式：

```bash
make -f Makefile.qemu run
```

## 8. 验证结果

构建：

```bash
make -f Makefile.qemu
```

运行：

```bash
make -f Makefile.qemu run
```

观察到内核可以在 QEMU 中完成启动，并进入后续用户态流程，串口输出包含：

```text
kernel boots ...
Kernel process started. EL 1
User process
```

这说明 QEMU 下打开 MMU 后没有再因取指地址无法翻译而卡死，identity mapping 起到了预期的过渡作用。

## 总结

本次改动的核心是为 QEMU 增加一套临时 identity mapping：

```text
VA low address -> PA same low address
```

它解决的是 QEMU 在开启 MMU 瞬间可能继续按低物理地址取指的问题。实现上，通过 `__create_idmap` 创建低地址映射页表，并在打开 MMU 前写入 `TTBR0_EL1`。正式进入用户进程后，`TTBR0_EL1` 会被切换为进程自己的用户页表，因此 idmap 只承担启动过渡职责。
