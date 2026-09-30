# The Python modules frozen into the firmware: compiled to bytecode at build
# time (mpy-cross) and run from flash, so an import reads no file and the
# bytecode takes none of the Python heap. See CMakeLists.txt.
freeze("$(PORT_DIR)/modules", ("vim.py", "esp.py"))
