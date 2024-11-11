1. Download PX2 Linux kernel archive from OneDrive: https://kth-my.sharepoint.com/:u:/g/personal/naveenm_ug_kth_se/EZuMMM-W971Eiu1zgdhF9vEBj_dYpmiysLg6WhBTHm1Qmg?e=sidRA2
2. Unpack it to $HOME directory. After unpacking you should see $HOME/nvidia directory
3. Inside *socketcan_kvaser_drivers* directory run the following command::

    make ARCH=arm64 CC=$HOME/nvidia/nvidia_sdk/DRIVE_OS_5.0.10.3_SDK_with_DriveWorks_Linux_OS_PX2_AUTOCHAUFFEUR/DriveSDK/toolchains/tegra-4.9-nv/usr/libexec/aarch64-gnu-linux/gcc/aarch64-gnu-linux/4.9.4/gcc LD=$HOME/nvidia/nvidia_sdk/DRIVE_OS_5.0.10.3_SDK_with_DriveWorks_Linux_OS_PX2_AUTOCHAUFFEUR/DriveSDK/toolchains/tegra-4.9-nv/usr/libexec/aarch64-gnu-linux/gcc/aarch64-gnu-linux/4.9.4/ld

4. Copy *kernel/drivers/net/can/usb/kvaser_usb/kvaser_usb.ko* to PX2 */lib/modules/4.9.80-rt61-tegra/kernel/drivers/net/can/usb/kvaser_usb.ko*

5. Add (if not already) to PX2 file */etc/modules* line **kvaser_usb**

6. Run: ``sudo update-initramfs -u``

7. If after attaching the new Kvaser CAN/LIN device, you see **can2** and **can3** in the output of ``ip addr``, it works.

