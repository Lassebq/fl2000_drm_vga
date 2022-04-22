# Linux kernel FL2000DX dongle DRM driver

Added VGA support to this [driver](https://github.com/klogg/fl2000_drm). HDMI support has been removed.

### Building driver

Check out the code and type
```
make
```
Use
```
insmod fl2000.ko && insmod it66121.ko
```
with sudo or in root shell to start the driver. If you are running on a system with secure boot enabled, you may need to sign kernel modules. Try using provided script for this:
```
./scripts/sign.sh
```
ensure that DRM components are loaded in your system, if not - please use
```
modprobe drm
modprobe drm_kms_helper
```
**NOTE:** proper kernel headers and build tools (e.g. "build-essential" package) must be installed on the system. Driver is developed and tested on linux 5.17.


## Not Implemented (or removed)
 * HDMI detection
 * VGA compression
