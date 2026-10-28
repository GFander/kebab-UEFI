# kebab-UEFI

Mu-Silicium 的 OnePlus 8T（kebab，SM8250）适配：让这块板子从 UEFI 走到 XNU，并跑起 iOS 16.7.16 的内核。

这个仓库只放**我们的改动**，不包含 Mu-Silicium 的完整历史（3.3 GB）。上游代码请自行克隆，然后按下面的方法叠加。

## 基线

```
仓库     Project-Silicium/Mu-Silicium
提交     6c6755012728c9f7de748228758bad94d1e9a660
标题     Add Samsung Galaxy S8 and S5E8895 support (#2160)
子模块   Binaries   c88f2796c4664703e61da9bc7435f77a52a87621
         Mu_Basecore cfb93724a06ff86ff5ba971413368a7809ee42a5
```

我们的工作在 `experiment/kebab-xnu-ramdisk` 分支上，领先基线 38 个提交。

## 仓库内容

```
patches/          38 个提交的 format-patch，按顺序编号
                  9999-worktree.patch 是最后一层未提交改动（含二进制）
src/              改动过的源码目录的完整副本
  Platforms/OnePlus/kebabPkg/
  Silicon/Silicium/SiliciumPkg/{Application/BootToBl, Drivers/*, Library/*}
  experiments/logexport/
BASE.txt          基线提交、子模块哈希
```

`src/` 用来看代码，`patches/` 用来复现。

## 怎么用

```bash
git clone https://github.com/Project-Silicium/Mu-Silicium.git
cd Mu-Silicium
git checkout 6c675501
git submodule update --init --recursive

# 叠我们的提交
git checkout -b kebab
git am /path/to/patches/0*.patch

# 最后一层是未提交改动
git apply --binary /path/to/patches/9999-worktree.patch
```

或者直接看 `src/` 里对应文件，和上游 diff 对比。

## 构建

上游的构建方式是 EDK2：

```bash
python Platforms/OnePlus/kebabPkg/DeviceBuild.py
```

产物是 `Build/kebabPkg/DEBUG_CLANGPDB/AARCH64/SILICIUM_UEFI.fd`（约 3 MB），烧到 `boot_b` 的固件区。

## 改动概览

```
BootManager.c                 +1479   UniXNU 启动菜单与启动流程
RamManager.c                    +68   内存布局
KebabFwLogDxe                          固件日志环形缓冲（供内核读取）
KebabRpmhProbeDxe                      RPMh 探针
QcomUfsDxe                             UFS 初始化
SmmuBypassDxe / SmmuEarlyReadDxe       SMMU 绕过与早期读取
kebab.dsc / kebab.fdf                  驱动装配
APRIORI.inc / DXE.inc                  预加载顺序
XnuRamDiskDxe/                         启动参数 FAT12 镜像
FrameBufferSerialPortLib.c        +30  串口重定向到帧缓冲
UniXNU.efi                             启动器二进制
```

## 启动链

```
ABL → boot_b（本仓库构建的固件）→ UniXNU 菜单 → DarwinBoot → XNU 内核
```

内核不在这里，见 `kebab-dev-ios16` 那个仓库（142 个补丁）。

## 注意

```
Binaries 和 Mu_Basecore 是子模块，指向的提交记录在 BASE.txt
仓库里没有完整历史，所以不能直接 git log 追溯前 38 个提交之前的演变
```
