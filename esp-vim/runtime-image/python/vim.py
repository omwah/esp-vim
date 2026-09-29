# vim: Vim, from MicroPython on esp-vim (:help esp-python).
#
# The core is C (the _vim module): command(), eval(), call() and the error
# they raise. The rest is built on Vim's own functions here, so that buffer
# changes go through getbufline()/setbufline() and friends, with undo and
# redraw as Vim does them. The names follow Vim's Python interface
# (:help python-vim) where it has them.

from _vim import command, eval, call, error

__all__ = ("command", "eval", "call", "error", "current", "buffers", "windows",
           "vars", "options", "Buffer", "Window")


def _let(name, value):
    call("esp#py#Let", name, value)


class Buffer:
    """A buffer, by number. Lines are str; indexes are 0-based, as in Python."""

    def __init__(self, nr):
        self._nr = nr

    @property
    def number(self):
        return self._nr

    @property
    def name(self):
        n = call("bufname", self._nr)
        return call("fnamemodify", n, ":p") if n else ""

    @property
    def valid(self):
        return bool(call("bufexists", self._nr))

    def __len__(self):
        info = call("getbufinfo", self._nr)
        if not info:
            raise error("buffer %d does not exist" % self._nr)
        return info[0]["linecount"]

    def _span(self, key):
        n = len(self)
        if isinstance(key, slice):
            start, stop, step = key.indices(n)
            if step != 1:
                raise ValueError("buffer slices have no step")
            return start, max(start, stop)
        if key < 0:
            key += n
        if not 0 <= key < n:
            raise IndexError("line %d: the buffer has %d" % (key, n))
        return key, key + 1

    def __getitem__(self, key):
        start, stop = self._span(key)
        lines = call("getbufline", self._nr, start + 1, stop) if stop > start else []
        return lines if isinstance(key, slice) else lines[0]

    def __setitem__(self, key, value):
        start, stop = self._span(key)
        lines = [value] if isinstance(key, int) else list(value)
        common = min(stop - start, len(lines))
        if common:
            call("setbufline", self._nr, start + 1, lines[:common])
        if len(lines) > common:
            call("appendbufline", self._nr, start + common, lines[common:])
        elif stop - start > common:
            call("deletebufline", self._nr, start + common + 1, stop)

    def __delitem__(self, key):
        start, stop = self._span(key)
        if stop > start:
            call("deletebufline", self._nr, start + 1, stop)

    def __iter__(self):
        return iter(call("getbufline", self._nr, 1, "$"))

    def append(self, lines, nr=None):
        """Add a line (or a list of them) at the end, or before line nr."""
        if nr is None:
            nr = len(self)
        call("appendbufline", self._nr, nr, lines)

    def __eq__(self, other):
        return isinstance(other, Buffer) and other._nr == self._nr

    def __repr__(self):
        return "<buffer %d %r>" % (self._nr, self.name)


class Window:
    """A window, by window ID."""

    def __init__(self, winid):
        self._id = winid

    @property
    def id(self):
        return self._id

    @property
    def number(self):
        return call("win_id2win", self._id)

    @property
    def buffer(self):
        return Buffer(call("winbufnr", self._id))

    @property
    def cursor(self):
        """(line, column): the line from 1 and the byte column from 0."""
        pos = call("getcurpos", self._id)
        return (pos[1], pos[2] - 1)

    @cursor.setter
    def cursor(self, pos):
        call("win_execute", self._id,
             "call cursor(%d, %d)" % (int(pos[0]), int(pos[1]) + 1))

    @property
    def height(self):
        return call("winheight", self._id)

    @property
    def width(self):
        return call("winwidth", self._id)

    def __eq__(self, other):
        return isinstance(other, Window) and other._id == self._id

    def __repr__(self):
        return "<window %d>" % self._id


class _Current:
    @property
    def buffer(self):
        return Buffer(call("bufnr", "%"))

    @property
    def window(self):
        return Window(call("win_getid"))

    @property
    def line(self):
        return call("getline", ".")

    @line.setter
    def line(self, text):
        call("setline", ".", text)


class _Buffers:
    """The listed buffers, by number: vim.buffers[3]."""

    def __getitem__(self, nr):
        if not call("bufexists", nr):
            raise KeyError(nr)
        return Buffer(nr)

    def __iter__(self):
        return iter([Buffer(b["bufnr"]) for b in call("getbufinfo", {"buflisted": 1})])

    def __len__(self):
        return len(call("getbufinfo", {"buflisted": 1}))


class _Windows:
    """The current tab page's windows, by number from 1: vim.windows[1]."""

    def __getitem__(self, n):
        wid = call("win_getid", n)
        if not wid:
            raise IndexError(n)
        return Window(wid)

    def __iter__(self):
        return iter([Window(call("win_getid", n)) for n in range(1, len(self) + 1)])

    def __len__(self):
        return call("winnr", "$")


class _Scope:
    """Vim variables with a prefix: vim.vars["x"] is g:x."""

    def __init__(self, prefix):
        self._prefix = prefix

    def __getitem__(self, name):
        full = self._prefix + name
        if not call("exists", full):
            raise KeyError(name)
        return eval(full)

    def __setitem__(self, name, value):
        _let(self._prefix + name, value)

    def __delitem__(self, name):
        full = self._prefix + name
        if not call("exists", full):
            raise KeyError(name)
        command("unlet " + full)

    def __contains__(self, name):
        return bool(call("exists", self._prefix + name))

    def get(self, name, default=None):
        return self[name] if name in self else default


current = _Current()
buffers = _Buffers()
windows = _Windows()
vars = _Scope("g:")
vvars = _Scope("v:")
options = _Scope("&")
