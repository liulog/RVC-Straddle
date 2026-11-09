# RVC-Straddle

RVC-Straddle 是一个运行在 RISC-V S-mode 的最小实验内核，验证同一组代码半字如何因**页映射变化**或**执行入口变化**，被解码为写入 satp 的指令：

- 固定入口，只替换下页：`csrw sscratch,a0` 变为 `csrw satp,a0`。
- 固定代码和映射，只将入口偏移 2 字节：普通 ADDI 的高半字与后续压缩指令拼成 `csrw satp,a0`。

两条路径均已在 QEMU 上实测通过，完整输出见 [qemu-serial.log](qemu-serial.log)。项目验证的是指令重组及实际 CSR 写入效果，执行始终位于允许访问 satp 的 S-mode。

## 使用方式

需要 RISC-V GCC 和带 OpenSBI 的 QEMU，在项目根目录使用以下 Makefile 目标：

| 目标 | 功能 |
| --- | --- |
| `make` | 编译 `straddle.elf` |
| `make run` | 编译并运行，保存日志并检查结果 |
| `make qemu` | 仅运行已有 ELF，不触发编译 |

QEMU 命令、超时和结果检查统一放在 [Makefile](Makefile)。可设置 `CROSS_COMPILE`（工具链前缀）、`QEMU`（模拟器）、`QEMU_TIMEOUT`（默认 30s）和 `LOG`（默认 `serial.log`）。本机工具链前缀为 `/opt/spacemit-toolchain-linux-glibc-x86_64-v1.2.4/bin/riscv64-unknown-linux-gnu-`。

串口输出地址、编码、寄存器和 PASS/FAIL；异常输出 `scause/sepc/stval`。成功标记为 `ALL PASS: cross-page, remap-only CSR, and middle-entry satp`，缺少该标记或出现 FAIL 均判为失败。

## 解码规则

RISC-V 从当前 PC 的第一个 16 位半字判断指令长度。启用 C 扩展后，指令起点只需 2 字节对齐，32 位指令可以跨页，也可以从原指令的中间重新开始解码。

| 起始半字的位 | 长度 |
| --- | --- |
| `bits[1:0] != 11` | 16 位压缩编码空间 |
| `bits[1:0] = 11` 且 `bits[4:2] != 111` | 32 位 |
| `bits[4:0] = 11111` | 更长编码，本实验不使用 |

长度匹配不代表编码合法。对于 CSR 访问指令，**opcode 决定大类，funct3 决定操作，CSR 编号决定访问目标**：

```text
bits:  31             20 19      15 14     12 11       7 6             0
       +----------------+----------+---------+----------+---------------+
       | CSR number     | rs1/uimm | funct3  | rd       | opcode        |
       +----------------+----------+---------+----------+---------------+
satp:  | 0x180          | 01010=a0 | 001     | 00000=x0 | 1110011=0x73  |
       +----------------+----------+---------+----------+---------------+
```

**仅有 opcode=0x73 不足以确定是 CSR 访问**，它表示 SYSTEM 类，还包含 ECALL、SRET 等。CSR 操作的 funct3 为 `001/010/011/101/110/111`；上图的 `001` 表示 CSRRW，配合 rd=x0，即 `csrw satp,a0`（编码 `0x18051073`），将 a0 写入 satp，不读取旧值。

## 验证内容与实测结果

内核由 OpenSBI 进入 S-mode，入口为 `0x80200000`。建立两个 4 KiB RX 测试页，通过 RAM 恒等映射准备代码；修改 PTE 后执行 `SFENCE.VMA`，指令同步使用 `FENCE.I`。根页表 A/B 的物理地址不同，但共享相同下级映射，保证切换后代码和栈仍有效。

本次根页表 A/B 的 PA 为 `0x8020f000 / 0x8020e000`，对应 satp 值分别为 `0x800000000008020f / 0x800000000008020e`。下文的页 A/B 指候选代码页，与根页表 A/B 分开命名。

### 固定入口，替换下页

选用 sscratch 作为原始目标，是因为它是供 S-mode 软件使用的暂存 CSR，写入不会直接改变地址翻译，适合做对照；实验保存并恢复其原值。satp 则控制地址翻译模式和根页表，写入效果可以读回核对。两条指令低半字都是 `1073`，只有下页提供的 CSR 编号不同。

入口固定为 `0x40000ffe`，高半字来自下一页 `0x40001000`。下图按内存地址顺序展示，`||` 表示页边界：

```text
原映射: [前页 1073: opcode=SYSTEM, funct3=CSRRW, rd=x0] || [页A 1405: CSR=sscratch, rs1高位] => 14051073: csrw sscratch,a0
换页后: [前页 1073: opcode=SYSTEM, funct3=CSRRW, rd=x0] || [页B 1805: CSR=satp,     rs1高位] => 18051073: csrw satp,a0
```

**opcode、funct3、rd 均在前页，CSR 编号全部位于下页。** 两个高半字的 rs1 高位相同，与前页的一位共同组成 a0。候选代码页预先准备好，测试时只改下页 PTE，不改前页或候选页字节。

实测原映射只将 `0x55` 写入 sscratch；换成页 B 后，a0 中的目标 satp 被写入，根页表从 B 切换为 A，读回一致。页 B 从自身页首进入时，`1805` 独立执行为 `c.addi a6,-31`，不写 CSR。

保持入口不变时，普通 ADDI 不能仅靠换下页变成 CSRW，因为 opcode 和 funct3 不会变化；负对照也验证了这一点。

### 固定代码和映射，改变入口

若允许跳进一条 4 字节指令的后半部分，原代码中的普通指令也能提供 CSR 编码。下面三个半字完全不变：

```text
地址:       0x40000ffc  0x40000ffe || 0x40001000
正常入口:  [  0513        1073  ] || [ 1805 ] => addi a0,t1,263 ; c.addi a6,-31
偏移入口:     0513     [  1073    ||   1805 ] => csrw satp,a0
```

原 ADDI 编码为 `0x10730513`，高半字恰好是 `1073`。从 `0x40000ffe` 进入后，该半字成为新指令的低半字，其长度位要求继续取 32 位指令，于是与 `1805` 组成 `0x18051073`。CSR opcode 来自原 ADDI 的高半字，后面的压缩指令被作为高半字消费，不再独立执行。

两次调用前均设置 a0 为目标 satp B、t1=0、a6=64，实测为：

| 入口 | a0 返回 | a6 返回 | satp 前 → 后 |
| --- | --- | --- | --- |
| 正常入口 `0x40000ffc` | `263` | `33` | A → A |
| 偏移入口 `0x40000ffe` | `0x800000000008020e` | `64` | A → B |

两次调用之间没有改写代码或叶子 PTE。前页 PA 为 `0x8020b000`，下页 PA 为 `0x80207000`，代码半字 `0513 / 1073 / 1805` 保持一致。该路径不需要替换页映射，仅改变入口即可产生并执行 satp 写指令。

## 验证范围

2026-11-09 在 QEMU 11.1.0、OpenSBI 1.8.1、GCC 15.2.0 上完成验证：RV64GC、Sv39、4 KiB 页、单 hart、128 MiB RAM。20 项检查全部通过，无 FAIL，构建与运行命令退出码为 0。除上述两条路径，还覆盖跨页 ADDI、候选页独立执行、直接放置的跨页 satp 写及 stvec 对照。

实验要求 S-mode 可访问 satp（TVM=0），且 a0 提供有效的目标值。它证明上述构造在此 QEMU 配置中成立，不证明任意现有代码都能组成目标指令，也不代表权限绕过或真实硬件验证。物理地址随编译布局变化，以实际日志为准。

规范参考：[指令长度与对齐](https://docs.riscv.org/reference/isa/v20240411/unpriv/intro.html)、[C 扩展](https://docs.riscv.org/reference/isa/unpriv/c-st-ext.html)、[Zicsr](https://docs.riscv.org/reference/isa/unpriv/zicsr.html)、[satp/SFENCE.VMA](https://docs.riscv.org/reference/isa/priv/supervisor.html)、[FENCE.I](https://docs.riscv.org/reference/isa/unpriv/zifencei.html)。
