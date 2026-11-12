# Build the asset archive bin/assets.tar (Code::Blocks pre-build step; the
# CMake "assets" target does the same).
#
# Nothing is compiled into the kernel and nothing is converted: programs,
# drivers, maps, fonts (.ttf) and images (.png) go into the archive as they
# are, which build-bootloader.sh writes behind the kernel.
#
#   build-assets.sh    assets.txt  -> bin/assets.tar

bash ./build-assets.sh assets.txt bin/assets.tar
