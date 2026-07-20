## 1. 多通道中断问题
xgmac 默认提供的方案是在 RX desc 中配置一个 IOC(Interrupt on Completion) bit，从而在当前 desc 被释放的时候主动触发 mac 中断，然后软件再查询 interrupt status 确定来了 RX，最后去做 RX 收包动作。

软件层面对这种场景下有两个需求：
    1. 中断合并：在大流量数据的情况下，大量的中断会导致 OS 损失性能
    2. 多通道中断做 CPU 绑核。

对于第一个问题，软件的设计是来到中断之后 disable irq 后做 polling。硬件做的是额外做一个 watchdog，在来到数据之后不立刻触发中断，而是触发 watchdog 后由 watchdog 来触发中断。
对于第二个问题，每个通道各自配置一个 watchdog，收包后各个通道的 watchdog 各自触发成独立的中断信号。软件可以为每个中断信号配置 CPU 亲和性。

这样一来，软件收发便存在三种方案：
    1. 为每个 rx desc 都配置 IOC，从而在每个包接收的时候都触发一次 mac interrupt
    2. desc.IOC 不做配置，在完成传输的时候触发 rx interrupt watchdog timer，等到 timer 时间耗尽则触发 RI
    3. 在不触发终端的情况下，记录发送字节数，并在发送到特定字节数量之后触发中断。

针对 RX 中断的触发逻辑，xgmac 提供了一个总的 mac interrupt(sbd_intr_o)，与八个 rx/tx 通道中断(sbd_perch_tx_intr_o, sbd_perch_rx_intr_o)，并通过寄存器 DMA_Mode.INTM 去配置这两种中断信号的行为：

- 00: 不分通道, 配置 IOC 之后直接触发汇总的 mac interrupt: sbd_perch_* are pulse signals for each completion events. sbd_intr_o is also asserted and cleared only when software clears the corresponding RI/TI status bits.
- 01: 屏蔽汇总后的 mac interrupt, 将分通道中断改成 level signals: sbd_perch_* are level signals asserted on the corresponding event and de-asserted when the software clears the corresponding RI/TI status bits. The sbd_intr_o is not asserted for these packet transfer completion events.
- 10: 未使用: sbd_perch_* are level signals asserted on the corresponding event and de-asserted when the software clears the corresponding RI/TI status bits. However, the signal is asserted again if the same event occurred again before it was cleared. The sbd_intr_o is not asserted for these packet transfer completion events.
- 11: Reserved

其核心问题是 sbd_intr 和 sbd_perch_intr 的中断触发条件:
    - 00: sbd_intr 直接拉高, sbd_perch 是 pulse 信号
    - 01: sbd_intr 不拉, sbd_perch 变成 level signal.

在该种情况下, RX 中断便存在两种触发方法:
    - 直接触发方案: DMA mode 配置为 0, RX 收包后由 IOC 触发 sbd_intr, 随后 cpu 通过 common 的 sbd_intr 获得中断
    - 分通道触发方案: DMA mode 配置为 1, 此时 sbd_intr 不再由 IOC 触发. RX 收包后触发 dma watchdog timer 并在 timer 触发之后拉 per-channel intr.

## primus rx-dma counter
