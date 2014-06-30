if [ $# -gt 0 ]; then
echo $1 > .version
fi

make -j6

../ramdisk_one_plus_one/dtbToolCM -2 -o ../ramdisk_one_plus_one/split_img/boot.img-dtb -s 2048 -p ../one_plus_one/scripts/dtc/ ../one_plus_one/arch/arm/boot/

cp arch/arm/boot/zImage ../ramdisk_one_plus_one/split_img/boot.img-zImage

cd ../ramdisk_one_plus_one/

./repackimg.sh

cd ../one_plus_one/

zipfile="franco.Kernel-nightly.zip"
echo "making zip file"
cp ../ramdisk_one_plus_one/image-new.img zip/boot.img
cd zip/
rm -f *.zip
zip -r -9 $zipfile *
rm -f /tmp/*.zip
cp *.zip /tmp
