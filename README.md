# gstreamer-rockchip
This plugin depends on gstreamer freamwork  
The plugin depends on mpp, libvpu and libdrm2-rockchip  
Before build this, make sure you have installed them above
## build deps
sudo apt install build-essential automake autoconf autopoint libtool pkg-config  
sudo apt install libgtk-3-dev liborc-0.4-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev  
## build
./autogen.sh && make  
## install
sudo make install  
## build a debian package
sudo apt install devscripts debhelper dh-exec  
DEB_BUILD_OPTIONS=nocheck debuild -i -nc -us -uc -b -d -aarmhf
