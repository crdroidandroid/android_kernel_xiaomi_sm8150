#!/bin/bash
rm .version
rm build.log
# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

clear

# Resources
export CLANG_PATH="$HOME/toolchains/boolx-clang/bin"
export PATH=${CLANG_PATH}:${PATH}
export CLANG_TRIPLE=${CLANG_PATH}/aarch64-linux-gnu-
export CROSS_COMPILE=${CLANG_PATH}/aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=${CLANG_PATH}/arm-linux-gnueabi-
export SUBARCH=arm64
# export DTC_EXT=dtc # we don needed it again
export ARCH=arm64
export CC=$HOME/toolchains/boolx-clang/bin/clang

VER="V1.0-Reinfinite+"
KERNEL_DIR=`pwd`
REPACK_DIR=$HOME/AnyKernel3
ZIP_MOVE=$HOME/Boolx-releases
BASE_AK_VER="Bool-X-Raphael-"
DATE=`date +"%Y%m%d-%H%M"`
AK_VER="$BASE_AK_VER$VER"
ZIP_NAME="$AK_VER"-"$DATE"
TOOLCHAINS=$HOME/toolchains/boolx-clang
CONFIG=out/.config
KERNEL=out/arch/arm64/boot/Image.gz-dtb
DTBO=out/arch/arm64/boot/dtbo.img

#functions
function create_out {
		echo
		git clone https://github.com/onettboots/boolx_anykernel.git $REPACK_DIR && mkdir $ZIP_MOVE
}
function clean_all {
		echo
		make clean && make mrproper && rm -rf out
}
function make_config {
		echo
		make CC=$HOME/toolchains/boolx-clang/bin/clang O=out raphael_defconfig
}
function make_menuconfig {
		echo
		make CC=$HOME/toolchains/boolx-clang/bin/clang O=out menuconfig
}
function building {
		echo
		make O=out CC=$HOME/toolchains/boolx-clang/bin/clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip KBUILD_BUILD_USER=OnettBoots KBUILD_BUILD_HOST=SuperTermux -j$(grep -c ^processor /proc/cpuinfo)
}
function make_boot {
		cp $KERNEL $REPACK_DIR && cp $DTBO $REPACK_DIR/oc
}
function make_zip {
		cd $REPACK_DIR
		zip -r9 `echo $ZIP_NAME`.zip *
		mv  `echo $ZIP_NAME`*.zip $ZIP_MOVE
		cd $KERNEL_DIR
}

DATE_START=$(date +"%s")

echo -e "${green}"
echo "----------------------"
echo "Checking Toolchains:"
echo "----------------------"
echo -e "${restore}"

if [ -d $TOOLCHAINS ]; then
   echo -e "${green}"
   echo "Bool-x clang is ready..!!"
   echo -e "${restore}"
else
   echo -e "${red}"
   echo "Downloading toolchains.!!"
   while read -p "what your architecture (aarch64/x86)? " cchoice
do
case "$cchoice" in
	aarch64|Aarch64 )
		echo
		echo "Downloading Boolx-clang for Aarch64 host."
		git clone https://gitlab.com/onettboots/boolx-clang.git -b Clang-15.0 $TOOLCHAINS
		break
		;;
	x86|X86 )
		echo
		echo "Downloading Boolx-clang for Aarch64 host."
		git clone https://gitlab.com/onettboots/boolx-clang.git -b Clang-16.0_x86 $TOOLCHAINS
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done
   echo -e "${restore}"
fi

echo -e "${green}"
echo "----------------------------------"
echo "Checking for Anykernel flashable:"
echo "----------------------------------"
echo -e "${restore}"


if [ -d $REPACK_DIR ] && [ -d $ZIP_MOVE ]; then
   echo -e "${green}"
   echo "Anykernel is ready skipping..!!"
   echo -e "${restore}"
else
   echo -e "${red}"
   echo "Adding Anykernel flashable.!!"
   create_out
   echo -e "${restore}"
fi

echo -e "${green}"
echo "------------------"
echo "CLEAN OPTIONS:"
echo "------------------"
while read -p "Do you want to clean stuffs (y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
	    echo -e "${red}"
		clean_all
		echo -e "${restore}"
		echo -e "${green}"
		echo "All Cleaned now."
		echo -e "${restore}"
		break
		;;
	n|N )
		break
		;;
	* )
		echo -e "${red}"
		echo "Invalid try again!"
		echo -e "${restore}"
		;;
esac
done
echo -e "${restore}"

if [ -f $CONFIG ]; then
   echo -e "${green}"
   while read -p "Old config is exist do you want replace with new config ? (y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
		make_config
		echo
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done
   echo -e "${restore}"
else
   echo -e "${green}"
   echo -e "${restore}"
fi

if [ -f $CONFIG ]; then
   echo -e "${green}"
   echo -e "${restore}"
else
   echo -e "${green}"
   echo "-------------------------------------"
   echo "Making Configs:"
   echo "-------------------------------------"
   echo -e "${restore}"
   echo -e "${red}"
   make_config
   echo -e "${restore}"
fi

if [ -f $CONFIG ]; then
   echo -e "${green}"
   while read -p "Do you want edit config via menuconfig ? (y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
		make_menuconfig
		echo
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done
   echo -e "${restore}"
else
   echo -e "${green}"
   echo -e "${restore}"
fi

echo -e "${green}"
echo "-----------------"
echo "Building Kernel:"
echo "-----------------"
echo -e "${restore}"

building

echo -e "${green}"
echo "----------------------"
echo "Checking output files"
echo "----------------------"
echo -e "${restore}"
sleep 3
    
if [ -f $KERNEL ]; then
   echo -e "${green}"
   echo "------------------------------------------"
   echo "Succesed Build, make flashable zip"
   echo "------------------------------------------"
   echo -e "${restore}"
   make_boot
   make_zip
   cd $ZIP_MOVE
   echo -e "${blink_red}"
   echo $ZIP_MOVE
   echo "------------------------------------------"
   echo $ZIP_NAME*.zip
   echo "------------------------------------------"
   echo -e "${restore}"
else
   echo -e "${red}"
   echo "-------------------------------------"
   echo "Building failed, Fix it and rebuild...!!!"
   echo "-------------------------------------"
   echo -e "${restore}"
fi

DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))
echo "Time: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo
