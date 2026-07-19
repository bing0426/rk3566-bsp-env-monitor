# RK3566 BME280 + SSD1315 Linux Driver Demo

这是一个用于求职展示的 Linux 驱动小项目，目标板是泰山派 RK3566，内核版本按
`4.19.232` 设计。项目采用混合方案：内核态负责硬件驱动，用户态负责业务调度和
显示内容组织。

> 说明：你原先提到 BMP280，但 BMP280 只有温度和气压；这里按你确认的 BME280
> 实现，可以采集温度、湿度和气压。

## 功能

- BME280 通过 I2C 采集温度、湿度、气压，导出 `/dev/bme280_env`
- SSD1315 128x64 OLED 通过 SPI 刷屏，导出 `/dev/ssd1315_oled`
- 按键通过 GPIO 中断触发，导出 `/dev/env_key`
- 用户态程序启动后先采集并显示一次
- 默认每 10 分钟采集一次并刷新显示
- 按键按下后立即采集并刷新显示

## 项目结构

```text
.
├── Kbuild
├── Makefile
├── app/
│   ├── Makefile
│   └── env_monitor.c
├── drivers/
│   ├── bme280_i2c/bme280_i2c.c
│   ├── env_key_irq/env_key_irq.c
│   └── ssd1315_spi/ssd1315_spi.c
├── dts/
│   └── rk3566-taishanpi-env-demo.dtsi
└── include/
    └── env_monitor_uapi.h
```

## 驱动设计

### BME280 I2C

`bme280_i2c.ko` 是一个标准 I2C client driver。

它在 `probe` 中读取芯片 ID、解析校准参数，并注册 misc 字符设备
`/dev/bme280_env`。用户态每次 `read` 都会触发一次 forced measurement，然后返回：

```text
temp_mC=25340 pressure_Pa=101325 humidity_mpermille=45678
```

单位含义：

- `temp_mC`：毫摄氏度，`25340` 表示 `25.34 C`
- `pressure_Pa`：帕
- `humidity_mpermille`：千分之一百分比，`45678` 表示 `45.678 %RH`

### SSD1315 SPI

`ssd1315_spi.ko` 是一个 SPI driver。

它通过 `dc-gpios` 区分命令和数据，通过 `reset-gpios` 复位 OLED，注册
`/dev/ssd1315_oled`。用户态向该设备一次写入 `1024` 字节帧缓冲即可刷新屏幕：

```text
128 * 64 / 8 = 1024 bytes
```

同时支持几个 ioctl：

- `SSD1315_IOC_CLEAR`
- `SSD1315_IOC_DISPLAY_ON`
- `SSD1315_IOC_DISPLAY_OFF`
- `SSD1315_IOC_INVERT_ON`
- `SSD1315_IOC_INVERT_OFF`

### GPIO 按键中断

`env_key_irq.ko` 是一个 platform driver，通过 `key-gpios` 获取按键 GPIO。

示例中按键为低电平有效，驱动使用下降沿中断、threaded IRQ、wait queue 和
`poll(2)`。用户态程序 `poll /dev/env_key`，按键触发后立即采集并刷新屏幕。

## 设备树

示例文件：`dts/rk3566-taishanpi-env-demo.dtsi`

默认示例：

- BME280：`i2c3`，地址 `0x76`
- SSD1315：`spi3`，片选 `CS0`
- OLED DC：`GPIO3_A5`
- OLED RES：`GPIO3_A6`，低电平复位
- 按键：`GPIO3_C0`，低电平有效

把这个 dtsi 合入你的 RK3566 板级 DTS，或者在板级 DTS 中 `#include` 它，然后重新编译
并替换 DTB。实际使用时重点检查 I2C/SPI 控制器编号、复用状态和 GPIO 是否与你接线一致。

## 编译

在板子上编译：

```bash
make KERNEL_DIR=/lib/modules/$(uname -r)/build ARCH=arm64
```

交叉编译示例：

```bash
make KERNEL_DIR=/path/to/rk3566/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

只编译用户态程序：

```bash
make -C app
```

## 运行

加载模块。使用当前目录结构直接 out-of-tree 编译时，模块会生成在各自源码目录：

```bash
sudo insmod drivers/bme280_i2c/bme280_i2c.ko
sudo insmod drivers/ssd1315_spi/ssd1315_spi.ko
sudo insmod drivers/env_key_irq/env_key_irq.ko
```

确认设备节点：

```bash
ls -l /dev/bme280_env /dev/ssd1315_oled /dev/env_key
cat /dev/bme280_env
```

启动程序：

```bash
sudo ./app/env_monitor
```

调试时可以把周期改短，例如 3 秒刷新一次：

```bash
sudo ./app/env_monitor -i 3000
```

## 面试讲解重点

- I2C：设备树匹配、`i2c_client`、SMBus 寄存器读写、芯片 ID 校验、校准参数解析
- 传感器算法：BME280 原始 ADC 值转换为温度、湿度和气压
- SPI：`spi_driver`、命令/数据 GPIO、OLED 初始化序列、GDDRAM 帧缓冲刷新
- GPIO/IRQ：GPIO descriptor、消抖、`gpiod_to_irq`、threaded IRQ、wait queue、`poll`
- 内核/用户边界：驱动只暴露硬件能力，10 分钟调度和显示排版在用户态完成
- 可扩展方向：BME280 可改成 IIO 驱动，SSD1315 可改成 fbdev/DRM tiny driver

## 参考

- Bosch Sensortec BME280 datasheet:
  <https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf>
