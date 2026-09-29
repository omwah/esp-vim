# esp: the device, from MicroPython on esp-vim (:help esp-python).
#
# Every esp_*() Vim function is esp.<name>() here, with the same arguments and
# results: esp.heap(), esp.gpio_write(2, 1), esp.i2c_scan(). The :Esp*
# commands and Python use the one implementation. A few conveniences follow.

from _vim import call as _call


def __getattr__(name):
    fn = "esp_" + name
    if name.startswith("_") or not _call("exists", "*" + fn):
        raise AttributeError("no function %s()" % fn)

    def f(*args):
        return _call(fn, *args)

    return f


class Pin:
    """A GPIO pin: Pin(2, Pin.OUT).on(); Pin(0, Pin.IN_PULLUP).value()."""

    IN = "in"
    IN_PULLUP = "in_pullup"
    IN_PULLDOWN = "in_pulldown"
    OUT = "out"

    def __init__(self, pin, mode=None):
        self.pin = pin
        if mode is not None:
            _call("esp_gpio_mode", pin, mode)

    def value(self, v=None):
        if v is None:
            return _call("esp_gpio_read", self.pin)
        _call("esp_gpio_write", self.pin, 1 if v else 0)

    def on(self):
        self.value(1)

    def off(self):
        self.value(0)

    def __repr__(self):
        return "Pin(%d)" % self.pin
