# Third-party software

PICOH-101 is MIT (LICENSE). The plugin is built with, and its binaries include, the following, each under its own license. CMake downloads most of them, at versions pinned by the template version (v0.6.1, GIT_TAG in synth/CMakeLists.txt) and by synth/CMakeLists.txt; keep their notices with anything you distribute. Every build ships the downloaded ones' license texts in THIRD_PARTY_LICENSES/, and the rest in licenses/ (from synth/licenses/).

- char-clap-template (its libs/): MIT. https://github.com/charCulbert/char-clap-template
- CLAP SDK: MIT. https://github.com/free-audio/clap
- CHOC (the native webview, through char-clap-template): ISC. https://github.com/Tracktion/choc
- Signalsmith's elliptic BLEP (header only, in elliptic-blep/; its filter coefficients are compiled into the DSP, which runs its own version of the filter): MIT, Copyright 2024 Signalsmith Audio Ltd. / Geraint Luff. https://github.com/Signalsmith-Audio/elliptic-blep
- clap-wrapper, and the VST3 SDK it downloads: MIT (see the SDK's own license). https://github.com/free-audio/clap-wrapper
- AudioUnit SDK (clap-wrapper downloads it for the AU, on macOS): Apache-2.0. https://github.com/apple/AudioUnitSDK
- ghc::filesystem (clap-wrapper downloads it on macOS, unless CMAKE_OSX_DEPLOYMENT_TARGET is 10.15 or later): MIT. https://github.com/gulrak/filesystem
- Compost (web components, bundled into the GUI page; its license ships beside it): MIT. https://github.com/charCulbert/compost
- IBM Plex Mono and Schibsted Grotesk (fonts, in web/fonts, shipped with their licenses): SIL Open Font License 1.1. https://github.com/IBM/plex https://github.com/schibsted/schibsted-grotesk
- The WASI SDK's runtime (wasi-libc, libc++, libc++abi, the LLVM runtime and dlmalloc), in the WCLAP only: Apache-2.0 with LLVM Exceptions, MIT, or CC0 (dlmalloc). https://github.com/WebAssembly/wasi-sdk

## The firmware

The Pico's firmware is built with the following. CMake fetches the Pico SDK (2.2.0), which carries the rest.

- Raspberry Pi Pico SDK: BSD-3-Clause. https://github.com/raspberrypi/pico-sdk
- TinyUSB (USB serial and MIDI): MIT. https://github.com/hathach/tinyusb
- lwIP (the network stack, with its mDNS): BSD-3-Clause. https://savannah.nongnu.org/projects/lwip/
- cyw43-driver (the Wi-Fi chip): its own license, which allows use only with Raspberry Pi's RP-series chips. https://github.com/georgerobotics/cyw43-driver
- firmware/i2s.pio, the I2S program from the Pico SDK's examples: BSD-3-Clause, Copyright 2020 Raspberry Pi (Trading) Ltd. (its header is kept).
- The DHCP server in firmware/third_party/dhcpserver, from MicroPython: MIT, Copyright 2013-2022 Damien P. George (its LICENSE is beside it). https://github.com/micropython/micropython
