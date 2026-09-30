# A 30-line ESP32-S3 app that shows esp-emu's zero-overhead-loop defect
# (docs/PHASE5.md): `pixi run emu-s3-loop` builds it and runs it under the
# vendored esp-emu. PASS means the emulator can be trusted with the S3 gate.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emu-s3-loop)
