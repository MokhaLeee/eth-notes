![alt text](rdma-test.assets/image-1.png)

一个完整的 RoCEv2 数据包 包含以下层级结构:

```
以太网帧头 (Ethernet II)
├── IP 头 (IPv4 / IPv6)
├── UDP 头 (Dst Port: 4791)
├── InfiniBand BTH (Base Transport Header)
├── InfiniBand Extended Transport Header (RETH / AETH / DETH 等)
├── Payload (数据载荷)
└── ICRC / Variant CRC (校验码)
```

- [Makefile](rdma-test.assets/demo/Makefile)
- [rdma_demo.c](rdma-test.assets/demo/rdma_demo.c)

```bash
# 配置 rxe 网卡
sudo apt install -y rdma-core libibverbs1 librdmacm1 ibverbs-utils perftest
sudo rdma link add rxe0 type rxe netdev eno1

# 确认 rxe 状态
rdma link show
ibv_devices

# 编译 demo 文件
sudo apt install -y build-essential libibverbs-dev librdmacm-dev
make

# srv: 10.116.89.94
sudo ./rdma_demo server -a 10.116.89.94
# cli: 10.116.89.201
sudo ./rdma_demo client -a 10.116.89.94 -m "Hello RDMA!"
```

[demo.pcap](rdma-test.assets/demo.pcap)
![alt text](rdma-test.assets/image.png)
