# RK3566 BME280 + SSD1315 Linux 驱动示例

这是一个运行在泰山派 RK3566 开发板上的环境监测示例项目。项目通过 Linux 内核驱动采集 BME280 的温度、湿度和气压数据，使用 SSD1315 OLED 显示数据，并支持 GPIO 按键触发即时刷新。

项目采用内核态与用户态分离的方式：内核驱动负责硬件访问和设备接口，用户态程序负责采集调度与显示内容组织。

> BME280 可采集温度、湿度和气压；BMP280 仅支持温度和气压。本项目使用 BME280。

## 功能

- BME280 通过 I2C 采集温度、湿度和气压，提供 `/dev/bme280_env`
- SSD1315 128×64 OLED 通过 SPI 刷屏，提供 `/dev/ssd1315_oled`
- GPIO 按键通过中断触发，提供 `/dev/env_key`
- 用户态程序启动后立即采集并显示一次数据
- 默认每 10 分钟自动采集并刷新一次
- 按下按键后立即采集并刷新显示

## 项目结构

```text
.
├── Kbuild
├── Makefile
├── app/
│   ├── Makefile
│   └── env_monitor.c
├── bsp/
│   ├── uboot-2026.01_defconfig       
│   ├── linux-6.18_defconfig          
│   └── buildroot-2025.02_defconfig   
├── drivers/
│   ├── bme280_i2c/bme280_i2c.c
│   ├── env_key_irq/env_key_irq.c
│   └── ssd1315_spi/ssd1315_spi.c
├── dts/
│   └── rk3566-taishanpi-env-demo.dtsi
└── include/
    └── env_monitor_uapi.h
```

## BSP 配置

后续使用的三个 defconfig 统一放在 `bsp/` 目录：

| 组件 | 版本 | 配置文件 |
| --- | --- | --- |
| U-Boot | 2026.01 | `bsp/uboot-2026.01_defconfig` |
| Linux Kernel | 6.18 | `bsp/linux-6.18_defconfig` |
| Buildroot | 2025.02 | `bsp/buildroot-2025.02_defconfig` |

当前仓库尚未包含这些配置文件，后续添加时请保持上述文件名。

## 驱动设计

### BME280 I2C

`bme280_i2c.ko` 是标准 I2C client 驱动。驱动在 `probe` 中读取芯片 ID、解析校准参数，并注册 misc 字符设备 `/dev/bme280_env`。用户态每次调用 `read` 都会触发一次 forced measurement，返回格式如下：

```text
temp_mC=25340 pressure_Pa=101325 humidity_mpermille=45678
```

数据单位：

- `temp_mC`：毫摄氏度，`25340` 表示 `25.34 °C`
- `pressure_Pa`：帕
- `humidity_mpermille`：千分之一百分比，`45678` 表示 `45.678 %RH`

### SSD1315 SPI

`ssd1315_spi.ko` 是 SPI 驱动。驱动通过 `dc-gpios` 区分命令和数据，通过 `reset-gpios` 复位 OLED，并注册 `/dev/ssd1315_oled`。用户态每次写入 1024 字节帧缓冲即可刷新整个屏幕：

```text
128 × 64 ÷ 8 = 1024 bytes
```

支持以下 ioctl：

- `SSD1315_IOC_CLEAR`
- `SSD1315_IOC_DISPLAY_ON`
- `SSD1315_IOC_DISPLAY_OFF`
- `SSD1315_IOC_INVERT_ON`
- `SSD1315_IOC_INVERT_OFF`

### GPIO 按键中断

`env_key_irq.ko` 是 platform 驱动，通过 `key-gpios` 获取按键 GPIO。示例按键为低电平有效，驱动使用下降沿中断、threaded IRQ、wait queue 和 `poll(2)`。用户态程序轮询 `/dev/env_key`，按键触发后立即采集数据并刷新屏幕。

## 设备树

设备树示例文件：`dts/rk3566-taishanpi-env-demo.dtsi`

默认配置：

- BME280：`i2c3`，地址 `0x76`
- SSD1315：`spi3`，片选 `CS0`
- OLED DC：`GPIO3_A5`
- OLED RES：`GPIO3_A6`，低电平复位
- 按键：`GPIO3_C0`，低电平有效

将该 dtsi 合入 RK3566 板级 DTS，或在板级 DTS 中包含它，然后重新编译并替换 DTB。实际使用时，需要确认 I2C/SPI 控制器编号、引脚复用状态和 GPIO 与硬件接线一致。

## 编译

在开发板上编译：

```bash
make KERNEL_DIR=/lib/modules/$(uname -r)/build ARCH=arm64
```

交叉编译：

```bash
make KERNEL_DIR=/path/to/rk3566/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

只编译用户态程序：

```bash
make -C app
```

## 运行

使用当前目录结构进行 out-of-tree 编译后，内核模块位于各自的源码目录。依次加载模块：

```bash
sudo insmod drivers/bme280_i2c/bme280_i2c.ko
sudo insmod drivers/ssd1315_spi/ssd1315_spi.ko
sudo insmod drivers/env_key_irq/env_key_irq.ko
```

确认设备节点并读取传感器数据：

```bash
ls -l /dev/bme280_env /dev/ssd1315_oled /dev/env_key
cat /dev/bme280_env
```

启动用户态程序：

```bash
sudo ./app/env_monitor
```

调试时可以缩短刷新周期。例如每 3 秒刷新一次：

```bash
sudo ./app/env_monitor -i 3000
```

## 参考资料

- [Bosch Sensortec BME280 数据手册](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf)
