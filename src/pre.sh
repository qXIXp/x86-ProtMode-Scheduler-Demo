#!/bin/bash
nasm ./main.asm -o main.com
sudo umount /mnt/floppyB
sudo mount -o loop pmtest.img /mnt/floppyB
sudo rm /mnt/floppyB/main.com
sudo cp ./main.com /mnt/floppyB
bochs -f bochsrc.txt
