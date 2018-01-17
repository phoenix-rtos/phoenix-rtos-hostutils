#!/bin/bash

echo '
ATTRS{idVendor}=="15a2", ATTRS{idProduct}=="0080", MODE="666"
ATTRS{idVendor}=="15a2", ATTRS{idProduct}=="007d", MODE="666"
ATTRS{idVendor}=="15a2", ATTRS{idProduct}=="006a", MODE="666"
' > 99-nxpimx.rules

sudo mv 99-nxpimx.rules /etc/udev/rules.d
sudo udevadm control --reload
