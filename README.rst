pico-clip
=========

Zephyr application for Raspberry Pi Pico 2 W
(``rpi_pico2/rp2350a/m33/w``).

Prerequisites
-------------

* Git
* Python 3.12 or newer with ``venv`` support
* CMake 3.20 or newer
* Ninja

Configure Wi-Fi
---------------

Before building, set the Wi-Fi SSID and password in ``prj.conf``::

   CONFIG_WIFI_SSID="YOUR_WIFI_SSID"
   CONFIG_WIFI_PSK="YOUR_WIFI_PASSWORD"

Build
-----

Create a new workspace and download the dependencies::

   mkdir -p pico-clip-workspace
   cd pico-clip-workspace

   git clone https://github.com/sepfy/pico-clip.git
   python3 -m venv pico-clip/.venv
   pico-clip/.venv/bin/python -m pip install --upgrade pip west

   pico-clip/.venv/bin/python -m west init -l pico-clip
   pico-clip/.venv/bin/python -m west update
   pico-clip/.venv/bin/python -m pip install \
     -r zephyr/scripts/requirements-base.txt
   pico-clip/.venv/bin/python -m west sdk install \
     --gnu-toolchains arm-zephyr-eabi

Download the CYW43439 Wi-Fi firmware::

   pico-clip/.venv/bin/python -m west blobs \
     -l 'img/whd/resources/(firmware/COMPONENT_43439/43439A0\.bin|clm/COMPONENT_43439/COMPONENT_MURATA-1YN/43439A0\.clm_blob)' \
     fetch hal_infineon

Configure ``pico-clip/prj.conf`` as shown above, then build::

   cd pico-clip
   ./scripts/build.sh make

The generated firmware is located at::

   build/pico2w_pico_clip/zephyr/zephyr.uf2
