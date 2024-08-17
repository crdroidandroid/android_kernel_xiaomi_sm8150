#!/bin/bash
# Completly Kernelsu Updater
dir="${PWD}"
# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

echo -e "${green}"
echo "--------------------------------"
echo "Pulling main branch of KernelSU:"
echo "--------------------------------"
echo -e "${restore}"

git subtree pull --prefix=drivers/kernelsu https://github.com/tiann/KernelSU.git v0.9.5 --squash -m "drivers: kernelsu: update"

# Lets tricky calculating for the reall version here
rm -rf ksu
git clone https://github.com/tiann/KernelSU ksu
cd ksu
KSU_VERSION=10200
KSU_GIT_VERSION=$(git rev-list --count HEAD)
let CURRENT=KSU_VERSION+KSU_GIT_VERSION

cd $dir
echo ""
curver=$(cat drivers/kernelsu/kernel/dksu)
echo "Old DKSU_VERSION is $curver"
echo""
OLD_VER="-DKSU_VERSION=$curver"
NEW_VER="-DKSU_VERSION=$CURRENT"

echo -e "${green}"
echo "----------------------"
echo "Updating dKSU version :"
echo "----------------------"
echo -e "${restore}"

sed -i "s/$OLD_VER/$NEW_VER/g" drivers/kernelsu/kernel/Makefile

echo ""
echo -e "${green}KernelSU has been Updated"
echo -e "${restore}Curent version is ${blink_red}$CURRENT"
echo "$CURRENT" > drivers/kernelsu/kernel/dksu
echo ""
echo "Adding commit message:"

git add drivers/kernelsu/kernel/Makefile drivers/kernelsu/kernel/dksu
git commit -s -m "drivers: kernelsu: Hardcode DKSU_VERSION to tiann/KernelSU@c62b89f
A little bit hacky way. Run (git rev-list --count HEAD) on a separate KernelSU repo"

rm -rf ksu
