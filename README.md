## Build Instructions ##

1. Configure Kconfig options

```shell
#Use default selections
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig
```

or

```shell
#Select options manually
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- nconfig
```

Make sure SOC_STARFIVE, CLK_STARFIVE_JH7110_SYS, RESET_STARFIVE_JH7110,
PINCTRL_STARFIVE_JH7110, SERIAL_8250_DW is selected.
> If you need SD card driver, you should also select MMC_DW_STARFIVE.

2. Build
```shell
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu-
```

## How to Run on VisionFive 2 Board via Network ##

1. Power on, enter u-boot and set enviroment parameters
```
setenv fdt_high 0xffffffffffffffff; setenv initrd_high 0xffffffffffffffff;
setenv scriptaddr 0x88100000; setenv script_offset_f 0x1fff000; setenv script_size_f 0x1000;
setenv kernel_addr_r 0x84000000; setenv kernel_comp_addr_r 0x90000000; setenv kernel_comp_size 0x10000000;
setenv fdt_addr_r 0x88000000; setenv ramdisk_addr_r 0x88300000;
```
2. Set IP addresses for the board and your tftp server
```
setenv serverip 192.168.x.x; setenv gatewayip 192.168.x.x; setenv ipaddr 192.168.x.x; setenv hostname starfive; setenv netdev eth0;
```
3. Upload dtb, image and file system to ddr from your tftp server
```
tftpboot ${fdt_addr_r} jh7110-starfive-visionfive-v2.dtb; tftpboot ${kernel_addr_r} Image.gz; tftpboot ${ramdisk_addr_r} initramfs.cpio.gz;
```
4. Load and boot the kernel
```
booti ${kernel_addr_r} ${ramdisk_addr_r}:${filesize} ${fdt_addr_r};
```
When you see the message "buildroot login:", the launch was successful.
You can just input the following accout and password, then continue.
```
buildroot login: root
Password: starfive
```
