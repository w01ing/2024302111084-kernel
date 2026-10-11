# Lab1 图纸

这两张图对应实验设计笔记要求：

- [启动时序图：上电到 main()](./lab1-startup-sequence.svg)
- [内核物理内存空间布局图](./lab1-memory-layout.svg)

可编辑源文件：

- [启动时序图 DOT 源码](./lab1-startup-sequence.dot)
- [内存布局图 DOT 源码](./lab1-memory-layout.dot)

图中的内核符号地址来自当前编译产物，若新增代码导致 `.bss` 布局变化，应重新运行：

```bash
riscv64-unknown-elf-nm -n kernel/kernel
```
