# FP/SIMD 与 CPACR_EL1 实验报告

## 目标

验证去掉 `-mgeneral-regs-only` 后，GCC 会在 AArch64 代码中生成 FP/SIMD 指令；如果 EL1 未开启 FP/SIMD 访问权限，`printf` 会停止工作。最后通过配置 `CPACR_EL1` 修复该问题。

## 1. 移除 `-mgeneral-regs-only`

修改位置：

- `Makefile.qemu`
- `Makefile.rpi3`
- `compile_flags.txt`

将 C 编译参数中的 `-mgeneral-regs-only` 删除。

重新编译时观察到 GCC 命令中已经不再包含该参数：

```text
aarch64-linux-gnu-gcc -Wall -Werror -nostdlib -nostartfiles -ffreestanding -Iinclude -g -O0 -DUSE_QEMU ...
```

## 2. 复现问题

执行：

```bash
make clean
make
timeout 5s make run
```

观察到 QEMU 启动后没有打印 `Exception level: 1`：

```text
qemu-system-aarch64 -M raspi3b -kernel ./kernel8.img -serial null -serial mon:stdio -nographic
qemu-system-aarch64: terminating on signal 15 from pid 2 (timeout)
```

作为对照，临时加回 `-mgeneral-regs-only` 编译后，QEMU 可以正常输出：

```text
Exception level: 1
```

因此可以确认：问题由移除 `-mgeneral-regs-only` 后生成的代码触发。

## 3. 反汇编观察

使用：

```bash
aarch64-linux-gnu-objdump -d build/kernel8.elf > kernel8_disasm.txt
```

在 `kernel8_disasm.txt` 中观察到 `tfp_printf` 使用了 FP/SIMD 寄存器：

```asm
0000000000080aac <tfp_printf>:
   80ad4: str q0, [sp, #96]
   80ad8: str q1, [sp, #112]
   80adc: str q2, [sp, #128]
   80ae0: str q3, [sp, #144]
   80ae4: str q4, [sp, #160]
   80ae8: str q5, [sp, #176]
   80aec: str q6, [sp, #192]
   80af0: str q7, [sp, #208]
```

`tfp_sprintf` 中也有类似指令：

```asm
0000000000080b9c <tfp_sprintf>:
   80bc4: str q0, [sp, #96]
   80bc8: str q1, [sp, #112]
   ...
   80be0: str q7, [sp, #208]
```

这说明 GCC 在编译变参函数时保存了 `q0` 到 `q7`。这些是 FP/SIMD 寄存器。EL1 未开启访问权限时，执行这些指令会触发异常；当前实验内核没有异常处理，因此表现为 `printf` 无输出。

## 4. 使用 `CPACR_EL1` 修复

在 `include/arm/sysregs.h` 中增加：

```c
#define CPACR_EL1_FPEN                  (3 << 20)
```

在 `src/boot.S` 的 `el1_entry` 中，进入 `kernel_main` 前增加：

```asm
ldr	x0, =CPACR_EL1_FPEN
msr	cpacr_el1, x0
isb
```

其中 `CPACR_EL1.FPEN[21:20] = 0b11` 表示允许 EL0 和 EL1 访问 FP/SIMD。

## 5. 修复后验证

保持 `-mgeneral-regs-only` 已删除，重新编译运行：

```bash
make clean
make
timeout 5s make run
```

观察到输出恢复：

```text
Exception level: 1
```

反汇编也确认 `cpacr_el1` 在进入 `kernel_main` 之前配置：

```asm
0000000000080048 <el1_entry>:
   80048: ldr x0, ...
   8004c: msr cpacr_el1, x0
   80050: isb
   80054: mov sp, #0x400000
   80058: bl kernel_main
```

## 结论

`-mgeneral-regs-only` 可以避免 GCC 生成 FP/SIMD 指令，因此能绕过 EL1 未开启 FP/SIMD 的问题。移除该参数后，`printf` 相关变参函数会使用 `q0` 到 `q7`，导致 EL1 触发异常。通过在进入 C 代码前配置 `CPACR_EL1.FPEN`，可以允许 EL1 使用 FP/SIMD，从而恢复 `printf` 输出。
