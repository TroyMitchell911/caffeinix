<h4 align="center">A Unix-like system that hopes to pursue efficiency.</h4>

<p align="center">
    <a href="https://github.com/TroyMitchell911/Caffeinix/commits/main/">
    <img src="https://img.shields.io/github/last-commit/TroyMitchell911/Caffeinix.svg?style=flat-square&logo=github&logoColor=white"
         alt="GitHub last commit">
    <a href="https://github.com/TroyMitchell911/Caffeinix/issues">
    <img src="https://img.shields.io/github/issues-raw/TroyMitchell911/Caffeinix.svg?style=flat-square&logo=github&logoColor=white"
         alt="GitHub issues">
    <a href="https://github.com/TroyMitchell911/Caffeinix/pulls">
    <img src="https://img.shields.io/github/issues-pr-raw/TroyMitchell911/Caffeinix.svg?style=flat-square&logo=github&logoColor=white"
         alt="GitHub pull requests">
</p>

<p align="center">
  <a href="#installation">Installation</a> •
  <a href="#updating">Updating</a> •
  <a href="#features">Features</a> •
  <a href="#wiki">Wiki</a> •
  <a href="#contributing">Contributing</a> •
  <a href="#credits">Credits</a> •
  <a href="#support">Support</a> •
  <a href="#license">License</a>
</p>

---

<table>
<tr>
<td>

**Caffeinix** is a **Unix-like system**. As the name suggests, **Caffeinix (Caffeine + Unix)**, this was written by me while drinking Americano. I hope that it can improve the efficiency of you and me like caffeine. **Of course, the efficiency may not be very high. I also said this is just a hope.**

</td>
</tr>
</table>

## Operating environment

This is not **absolute**, just what we **recommend**.

```bash
Distributor ID: Ubuntu
Description:    Ubuntu 22.04.4 LTS
Release:        22.04
Codename:       jammy
```

## Prerequisites

### Method 1: Using Docker

```bash
$ docker run -itd -p 10008:10008 -v /dev/caffeinix:/dev/caffeinix -w /root --name caffeinix --restart=always troymitchell/caffeinix:1.0 /bin/bash
$ docker restart caffeinix
$ docker exec -it caffeinix bash
```

### Method 2: Manual Installation 

#### Step 1

```bash
$ sudo apt update
$ sudo apt install build-essential gcc make perl dkms git gdb-multiarch qemu-system-misc bear
```

#### Step 2

You need a RISC-V "newlib" tool chain from https://github.com/riscv/riscv-gnu-toolchain

> [!IMPORTANT] 
> You need to install 'qemu-system-misc' if you are using Docker to deploy otherwise you can't use commands 'make qemu-gdb' and 'make qemu'.

> [!NOTE]  
> None now

## Getting the sources

You don't need this step if you are using **[Method 1: Using Docker](#Method-1-Using-Docker)** because we have put the sources into the directory `~/caffeinix`. But the only thing you need to do is entering the directory `~/caffeinix` then run `git pull origin main` command to avoiding the source is old.

```bash
$ git clone https://github.com/TroyMitchell911/caffeinix.git
```

## Building and usage

- `make`：Compile and build
- `make qemu`：Start qemu running after compiling and building
- `make qemu-gdb`：Start debugging

## Updating

## Features

## Wiki

Do you **need some help**? Check out the _articles_ on the [wiki](https://github.com/TroyMitchell911/Caffeinix/wiki/).

## Contributing

Got **something interesting** you'd like to **share**? Learn about [contributing](#).    
## Credits

## Support

Reach out to me via the **[profile addresses](https://github.com/TroyMitchell911)**.

## License

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL%203.0-green)](https://www.tldrlegal.com/license/gnu-general-public-license-v3-gpl-3)
