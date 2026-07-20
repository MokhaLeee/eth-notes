# 1. 寄存器讨论

`MAC_Packet_Filter`

- `RA`(BIT_31): receive all.
- `VUCC`(BIT_22): VxLAN UDP/IPv6 Checksum Control.
- `DNTU`(BIT_21): Drop Non-TCP/UDP over IP Packets. 这是一个 L4 级别的 filter, 自动监测并过滤所有的非 tcp/udp 包.
- `IPFE`(BIT_20): Layer 3 and Layer 4 Filter Enable.
- `VTFE`(BIT_16): VLAN Tag Filter Enable.
- `DHLFRS`(BIT_12:11): DA Hash Index or L3/L4 Filter Number in Receive Status.
- `HPF`(BIT_10): Hash or Perfect Filter: xgmac 提供了两种包过滤机制: 通过 DA address 做 perfect filter 或者通过 hash table 做 unperfect filter. *如果 HPF=0 则强制按照 hash 的模式做 filter, 配置为 1 则按照 HMU/HUC 做决定.*
- `SAF`(BIT_9): Source Address Filter Enable. 校验 SA field 并与 MAC_Address registers 做比对, 不 match 的包会被丢弃.
- `SAIF`(BIT_8): SA Inverse Filtering. 在该种情况下 MAC_Address 的功能反过来, 只有匹配的包会被丢弃, 不匹配的包反而会被接收.
- `PCF`(BIT_7:6): Pass Control Packets. These bits control the forwarding of all control packets (including unicast and multicast Pause packets).
	- 00: The MAC filters all control packets from reaching the application.
	- 01: The MAC forwards all control packets except Pause packets to the application even if they fail the Address filter.
	- 10: The MAC forwards all control packets to the application even if they fail the Address filter.
	- 11: The MAC forwards the control packets that pass the Address filter.
- `DBF`(BIT_5): Disable Broadcast Packets.
- `PM`(BIT_4): Pass All Multicast. 如果该 bit 置 0, 就会对 multicast 做 perfect or hash filtering.
- `DAIF`(BIT_3): DA Inverse Filtering.
- `HMC`(BIT_2): Hash Multicast.
- `HUC`(BIT_1): Hash Unicast.
- `PR`(BIT_0): Promiscuous Mode. When this bit is set, the Address Filtering module passes all incoming packets irrespective of the destination or source address. The MAC clears the SA or DA Filter Fail status bits of the Rx Status Word when PR is set.

# 2. filter 机制 (Source Address or Destination Address Filtering)

xgmac 提供了两种包过滤机制:
- perfect filtering: 将全部 48 bits DA field 与内部预设的 MAC address 寄存器列表做比对.
- hash filtering: imperfect filtering, 只对 upper 6 bits (depend on hash table size) 做 CRC 之后从 hash table 中做查询.

## 2.1. Unicast Destination Address Filtering

Perfect filtering: 通过 `MAC_Packet_Filter.HUC=0` 使能, xgmac 支持配置 32 MAC addresses for unicast filtering. MacAddr0 is always enabled 但是其他的 addr 需要用 mask 使能.

Hask filtering: 通过 `MAC_Packet_Filter.HUC=1` 使能, 其对 upper 6bits 做 crc 后判断 hash table 中的 bit 位, 例如 crc=6'b000000 对应 hash table bit_0, crc=6'b111111 对应 bit_63. 如果预设的 bit 被置 1, 则 filter 通过, 反之 drop

## 2.2. Multicast Destination Address Filtering

1. 可以通过配置 `MAC_Packet_Filter.PM=1` to pass all multicast packets.
2. 在配置 `PM=0` 的情况下 multicast `HMC=1` 的情况做 hash filter, 否则做 perfect filter.

## 2.3. Hash or Perfect Address Filtering

1. `HPF` 配 0 则全都强制走 hash
2. `HPF=1` 的情况, `HUC` 控制 unicast, `HMU` 控制 multicast 的走向

## 2.4. Broadcast Address Filtering

xgmac 不对 broadcast 做过滤, 可以通过 `DBF` 拒绝所有 broadcast.

## 2.5. Unicast Source Address Filtering

xgmac 可以通过特定 bit 来过滤 source-address 而非 destination-address.

## 2.6. Inverse Filtering

# 3. linux 流程讨论
```c
static void dwxgmac2_set_mchash(void __iomem *ioaddr, u32 *mcfilterbits,
				int mcbitslog2)
{
	int numhashregs, regs;

	switch (mcbitslog2) {
	case 6:
		numhashregs = 2;
		break;
	case 7:
		numhashregs = 4;
		break;
	case 8:
		numhashregs = 8;
		break;
	default:
		return;
	}

	for (regs = 0; regs < numhashregs; regs++)
		writel(mcfilterbits[regs], ioaddr + XGMAC_HASH_TABLE(regs));
}

static void dwxgmac2_set_filter(struct mac_device_info *hw,
				struct net_device *dev)
{
	void __iomem *ioaddr = (void __iomem *)dev->base_addr;
	u32 value = readl(ioaddr + XGMAC_PACKET_FILTER);
	int mcbitslog2 = hw->mcast_bits_log2;
	u32 mc_filter[8];
	int i;

	value &= ~(XGMAC_FILTER_PR | XGMAC_FILTER_HMC | XGMAC_FILTER_PM);
	value |= XGMAC_FILTER_HPF;

	memset(mc_filter, 0, sizeof(mc_filter));

	if (dev->flags & IFF_PROMISC) {
		value |= XGMAC_FILTER_PR;
		value |= XGMAC_FILTER_PCF;
	} else if ((dev->flags & IFF_ALLMULTI) ||
		   (netdev_mc_count(dev) > hw->multicast_filter_bins)) {
		value |= XGMAC_FILTER_PM;

		for (i = 0; i < XGMAC_MAX_HASH_TABLE; i++)
			writel(~0x0, ioaddr + XGMAC_HASH_TABLE(i));
	} else if (!netdev_mc_empty(dev) && (dev->flags & IFF_MULTICAST)) {
		struct netdev_hw_addr *ha;

		value |= XGMAC_FILTER_HMC;

		netdev_for_each_mc_addr(ha, dev) {
			u32 nr = (bitrev32(~crc32_le(~0, ha->addr, 6)) >>
					(32 - mcbitslog2));
			mc_filter[nr >> 5] |= (1 << (nr & 0x1F));
		}
	}

	dwxgmac2_set_mchash(ioaddr, mc_filter, mcbitslog2);

	/* Handle multiple unicast addresses */
	if (netdev_uc_count(dev) > hw->unicast_filter_entries) {
		value |= XGMAC_FILTER_PR;
	} else {
		struct netdev_hw_addr *ha;
		int reg = 1;

		netdev_for_each_uc_addr(ha, dev) {
			dwxgmac2_set_umac_addr(hw, ha->addr, reg);
			reg++;
		}

		for ( ; reg < XGMAC_ADDR_MAX; reg++) {
			writel(0, ioaddr + XGMAC_ADDRx_HIGH(reg));
			writel(0, ioaddr + XGMAC_ADDRx_LOW(reg));
		}
	}

	writel(value, ioaddr + XGMAC_PACKET_FILTER);
}
```

## 3.1. 默认配置:

- `PR=0`: 该 bit 会让所有的包都被接收而不做过滤, 置0以使能过滤机制
- `PM=0, HMC=0`: 对 multicast 做包过滤, 配置为 perfect filter
- `HPF=1`: 不强制做 hash filter, 交予 `HMU/HUC` 决定

## 3.2. multicast 配置:
- `IFF_PROMISC`(0x100): receive all packets
	- `PR=1`: 接收所有的包: broadcast/unicast/multicast
	- `PCF=0'b10`: 此处配置为 0b10, 通过所有 packets

- `IFF_ALLMULTI`(0x200): receive all multicast packets
	- `PM=1`: 接收所有 multicast
	- 同步清空 hash table

- `IFF_MULTICAST`(0x8000): supports multicast
	在这种情况下就需要选择 filter, stmmac 选择使用 hash filter 机制: `HMC=1` 的情况下根据软件配置的 multicast address 配置 hash table, 其中 hash table 需要根据 hash table size 来决定计算方式.

## 3.3. unicast 配置:

1. 如果软件希望配置的 unicast 超过了 DA address list size, 那就直接拉 `PR=1`, 直接不做 filter 给所有的包开绿灯(真是个简单粗暴的方法...)
2. 正常情况下则配置正常配置 DA 并作 perfect filter

# 4. hash table 计算规则

The 64-bit, 128-bit, or 256-bit hash table is used for group address filtering. For hash filtering, the content of the destination address in the incoming packet is passed through the CRC logic and the upper six (seven or eight in 128- or 256-bit Hash) bits of the CRC are used to index the content of the Hash table. The most significant bits determines the register to be used (Hash Table Register X), and the least significant five bits determine the bit within the register For example, a hash value of 7b'1100000 (in 128-bit Hash) selects Bit 0 of the Hash Table Register 3 and a value of 8b'10111111 (in 256-bit Hash) selects Bit 31 of the Hash Table Register 5.
The hash value of the destination address is calculated in the following way:

- Calculate the 32-bit CRC for the DA (See IEEE 802.3-2018, Section 3.2.8 for the steps to calculate
CRC32).
- Perform bit-wise reversal for the value obtained in Step 1.
- Take the upper 6 (or 7 or 8) bits from the value obtained in Step 2.

If the corresponding bit value of the MAC_Hash_Table_Reg0 register is 1'b1, the packet is accepted. Otherwise, it is rejected. If the PM bit is set in MAC_Packet_Filter, all multicast packets are accepted regardless of the multicast hash values.

If the Hash Table register is configured to be double-synchronized to the (X)GMII clock domain, the synchronization is triggered only when Bits[31:24] (in little-endian mode) or Bits[7:0] (in big-endian mode) of the Hash Table Register X registers are written.

If double-synchronization is enabled, consecutive writes to this register must be performed after at least four clock cycles in the destination clock domain.

翻译成人话:
1. 对 6 字节 mac 地址计算 crc 后取反
2. 取 crc 高侧 6bit (for 64bit hash, 7bit for 128bit hash, and 8bit for 256bit hash)
3. 将对应 hash table bit 置位:

对应 freebsd 源代码:
```c
uint32_t
ether_crc32_le(const uint8_t *buf, size_t len)
{
	static const uint32_t crctab[] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
		0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
		0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
		0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
	};
	size_t i;
	uint32_t crc;

	crc = 0xffffffff;	/* initial value */

	for (i = 0; i < len; i++) {
		crc ^= buf[i];
		crc = (crc >> 4) ^ crctab[crc & 0xf];
		crc = (crc >> 4) ^ crctab[crc & 0xf];
	}

	return (crc);
}

static uint32_t bitreverse(uint32_t value)
{
	uint32_t result = 0;

	for (int i = 0; i < 32; i++) {
		uint32_t bit = value & 1;

		result = (result << 1) | bit;
		value >>= 1;
	}

	return result;
}

#define DWCXG_HASH_TABLE 128
#define DWCXG_HASH_TABLE_LOG2 7

static u_int stmmac_filter_hash_maddr(void *arg, struct sockaddr_dl *sdl, u_int cnt)
{
	struct stmmac_hash_maddr_ctx *ctx = arg;
	uint32_t crc, hashbit, hashreg;
	uint8_t val;

	crc = ether_crc32_le(LLADDR(sdl), ETHER_ADDR_LEN);

	/* Take lower 7 bits for 128bit hash table and reverse it */
	val = (bitreverse(~crc) >> (32 - DWCXG_HASH_TABLE_LOG2));

	hashreg = (val >> 5);
	hashbit = (val & 31);
	ctx->hash[hashreg] |= (1 << hashbit);

	return 1;
}
```
