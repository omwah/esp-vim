" The :Esp* commands (plugin/esp.vim). The esp_*() builtins return plain
" data; everything about how it looks is here, so it can change without
" reflashing the firmware.

" ---------------------------------------------------------------- views --

" Show {lines} in the view window called {name}, reusing it if it is open.
" {header} is the line number of a table header to highlight (0: none).
" {Refresh} is what R calls to redraw it.
function! s:Show(name, lines, header, Refresh) abort
  let win = 0
  for w in range(1, winnr('$'))
    if getbufvar(winbufnr(w), 'esp_view', '') ==# a:name
      let win = w
      break
    endif
  endfor
  if win
    execute win . 'wincmd w'
  else
    execute 'botright ' . min([len(a:lines), &lines / 2]) . 'new'
    setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted
    setlocal nonumber norelativenumber nowrap nolist foldcolumn=0 nospell
    let &l:fillchars = 'eob: '
    let b:esp_view = a:name
    silent execute 'file Esp' . a:name
    nnoremap <buffer> <silent> q :close<CR>
    nnoremap <buffer> <silent> R :call b:esp_refresh()<CR>
    syntax match EspViewTitle /\%1l.*/
    highlight default link EspViewTitle Title
    highlight default link EspViewHeader Statement
    highlight default link EspViewLabel Identifier
  endif
  let b:esp_refresh = a:Refresh
  silent! syntax clear EspViewHeader EspViewLabel
  if a:header
    execute 'syntax match EspViewHeader /\%' . a:header . 'l.*/'
  else
    syntax match EspViewLabel /^\%>1l\S\+/
  endif
  setlocal modifiable
  silent %delete _
  call setline(1, a:lines)
  setlocal nomodifiable nomodified
  normal! gg
endfunction

" 1234567 -> "1.2 MB"
function! s:Size(n) abort
  if a:n < 1024
    return a:n . ' B'
  elseif a:n < 1024 * 1024
    return printf('%.1f KB', a:n / 1024.0)
  endif
  return printf('%.1f MB', a:n / 1048576.0)
endfunction

function! s:Row(fmt, ...) abort
  return substitute(call('printf', [a:fmt] + a:000), '\s\+$', '', '')
endfunction

" The width a view has: its window's, when it is already open, else the
" screen's (s:Show opens it the full width).
function! s:ViewWidth(name) abort
  for w in range(1, winnr('$'))
    if getbufvar(winbufnr(w), 'esp_view', '') ==# a:name
      return winwidth(w)
    endif
  endfor
  return &columns
endfunction

" A table for view {name}: each column as wide as its widest cell, 'l'eft or
" 'r'ight aligned as {align} says, with as much space between them as the
" window has room for -- 3 spaces, 2, or 1; past that, only the last column
" runs over the edge. {rows}: lists of cells (strings or numbers); [] for an
" empty line.
function! s:Table(name, rows, align) abort
  let n = len(a:align)
  let widths = repeat([0], n)
  for r in a:rows
    for i in range(min([n, len(r)]))
      let widths[i] = max([widths[i], strwidth(r[i])])
    endfor
  endfor
  let total = 0
  for w in widths
    let total += w
  endfor
  let room = s:ViewWidth(a:name)
  let gap = total + 3 * (n - 1) <= room ? 3 : total + 2 * (n - 1) <= room ? 2 : 1
  let lines = []
  for r in a:rows
    let cells = []
    for i in range(n)
      let c = i < len(r) ? string(r[i]) : ''
      let c = i < len(r) && type(r[i]) == v:t_string ? r[i] : c
      let pad = repeat(' ', widths[i] - strwidth(c))
      call add(cells, a:align[i] ==# 'r' ? pad . c : c . pad)
    endfor
    call add(lines, substitute(join(cells, repeat(' ', gap)), '\s\+$', '', ''))
  endfor
  return lines
endfunction

" The same, for the other autoload scripts (esp/git.vim).
function! esp#View(name, lines, header, Refresh) abort
  call s:Show(a:name, a:lines, a:header, a:Refresh)
endfunction

function! esp#Table(name, rows, align) abort
  return s:Table(a:name, a:rows, a:align)
endfunction

" ------------------------------------------------------------- :EspInfo --

function! esp#Info() abort
  let i = esp_info()
  let up = i.uptime_ms / 1000
  let lines = [i.chip . ' -- system information   (R refresh, q close)',
        \ s:Row('%-10s %s, revision %s', 'Chip', i.chip, i.revision),
        \ s:Row('%-10s %d cores at %d MHz', 'CPU', i.cores, i.cpu_mhz),
        \ s:Row('%-10s %s', 'Features', empty(i.features) ? 'none' : join(i.features, ', ')),
        \ s:Row('%-10s %s', 'Flash', s:Size(get(i, 'flash', 0))),
        \ s:Row('%-10s %s', 'PSRAM', i.psram ? s:Size(i.psram) : 'none'),
        \ ]
  if has_key(i, 'mac')
    call add(lines, s:Row('%-10s %s', 'MAC', i.mac))
  endif
  call extend(lines, [
        \ s:Row('%-10s %s', 'ESP-IDF', i.idf),
        \ s:Row('%-10s %s', 'Vim', i.vim),
        \ s:Row('%-10s %d:%02d:%02d', 'Uptime', up / 3600, up / 60 % 60, up % 60),
        \ s:Row('%-10s %s', 'Reset', i.reset),
        \ ])
  call s:Show('Info', lines, 0, function('esp#Info'))
endfunction

" ------------------------------------------------------------- :EspHeap --

function! esp#Heap() abort
  let h = esp_heap()
  let rows = [['Heap', 'free', 'largest', 'lowest', 'total']]
  for name in ['internal', 'psram', 'dma']
    let x = h[name]
    if x.total
      call add(rows, [name, s:Size(x.free), s:Size(x.largest), s:Size(x.min_free), s:Size(x.total)])
    endif
  endfor
  call extend(rows, [[], ['Vim', 'in use', 'peak', 'budget'],
        \ ['', s:Size(h.vim.used), s:Size(h.vim.peak), h.vim.budget ? s:Size(h.vim.budget) : 'none']])
  let py = esp_py_heap()
  if py.running
    call extend(rows, [[], ['Python', 'in use', 'free', 'total'],
          \ ['', s:Size(py.used), s:Size(py.free), s:Size(py.total)]])
  endif
  call s:Show('Heap', ['Memory   (R refresh, q close)'] + s:Table('Heap', rows, 'lrrrr'),
        \ 2, function('esp#Heap'))
endfunction

" ------------------------------------------------------------ :EspTasks --

function! esp#Tasks() abort
  let tasks = sort(esp_tasks(), {a, b -> a.priority != b.priority
        \ ? b.priority - a.priority : a.name < b.name ? -1 : a.name > b.name})
  let rows = [['Name', 'State', 'Prio', 'Core', 'Stack free']]
  for t in tasks
    call add(rows, [t.name, t.state, t.priority, t.core < 0 ? 'any' : t.core, s:Size(t.stack_free)])
  endfor
  call s:Show('Tasks', ['FreeRTOS tasks: ' . len(tasks) . '   (R refresh, q close)']
        \ + s:Table('Tasks', rows, 'llrrr'), 2, function('esp#Tasks'))
endfunction

" ----------------------------------------------------------- :EspReboot --

function! esp#Reboot(bang) abort
  let modified = getbufinfo({'bufmodified': 1})
  if !a:bang && !empty(modified)
    echohl ErrorMsg
    echomsg 'EspReboot: ' . len(modified) . ' buffer(s) with unsaved changes, e.g. "'
          \ . fnamemodify(modified[0].name, ':~:.') . '" (add ! to reboot anyway)'
    echohl None
    return
  endif
  echo 'Rebooting...'
  redraw
  call esp_reboot()
endfunction

" ------------------------------------------------ :EspSleep and :EspPower --

" :EspSleep[!] [deep] [{seconds}]: sleep now. On a board with a wake button,
" reader mode: light sleep, the screen as it was, and Vim carries on where it
" was when the button wakes it. "deep" (or no wake button): the chip stops,
" and waking is a boot, so unsaved changes would be lost -- refused without !.
" {seconds}: a timer wakes it as well.
function! esp#SleepComplete(lead, line, pos) abort
  return filter(['deep'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#Sleep(bang, ...) abort
  let args = copy(a:000)
  let deep = !empty(args) && args[0] ==# 'deep'
  if deep
    call remove(args, 0)
  endif
  let secs = empty(args) ? 0 : str2nr(args[0])
  if len(args) > 1 || (!empty(args) && args[0] !~# '^\d\+$')
    echoerr 'Usage: :EspSleep[!] [deep] [{seconds}]'
    return
  endif
  if !esp_power().reader && secs == 0
    let deep = 1                        " nothing but a reset would wake it
  endif
  let modified = getbufinfo({'bufmodified': 1})
  if deep && !a:bang && !empty(modified)
    echohl ErrorMsg
    echomsg 'EspSleep: ' . len(modified) . ' buffer(s) with unsaved changes, e.g. "'
          \ . fnamemodify(modified[0].name, ':~:.') . '": deep sleep loses them (add ! to sleep anyway)'
    echohl None
    return
  endif
  " Quietly, as when the button starts it: the badge (or the hippo) says so.
  echo ''
  redraw
  call esp_sleep(deep, secs)
endfunction

" :EspBattery: charge, voltage, and whether a computer is on the USB port.
function! esp#Battery() abort
  let p = esp_power()
  let lines = ['Battery   (R refresh, q close)']
  if p.battery_mv < 0
    call add(lines, s:Row('%-8s %s', 'Battery', 'no reading'))
  else
    call extend(lines, [
          \ s:Row('%-8s %d%%', 'Charge', p.battery_pct),
          \ s:Row('%-8s %d.%02d V', 'Voltage', p.battery_mv / 1000, p.battery_mv % 1000 / 10)])
  endif
  call add(lines, s:Row('%-8s %s', 'USB', p.source ==# 'usb'
        \ ? 'connected to a computer' : 'not detected (a charger can''t be seen)'))
  call s:Show('Battery', lines, 0, function('esp#Battery'))
endfunction

" :EspPower                   the battery, and when the board sleeps
" :EspPower idle {min}|off    reader mode after {min} minutes idle, on battery
" :EspPower deep {min}|off    deep sleep after {min} minutes in reader mode
function! esp#PowerComplete(lead, line, pos) abort
  return filter(['idle', 'deep'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#Power(...) abort
  if a:0
    if a:0 != 2 || index(['idle', 'deep'], a:1) < 0 || a:2 !~# '^\(\d\+\|off\)$'
      echoerr 'Usage: :EspPower [idle|deep {minutes}|off]'
      return
    endif
    call esp_power({a:1 . '_min': a:2 ==# 'off' ? 0 : str2nr(a:2)})
  endif
  let p = esp_power()
  let bat = p.battery_mv < 0 ? 'none measured'
        \ : printf('%d.%02d V, about %d%%', p.battery_mv / 1000, p.battery_mv % 1000 / 10, p.battery_pct)
  let lines = ['Power   (R refresh, q close)',
        \ s:Row('%-8s %s', 'Source', p.source ==# 'usb' ? 'USB (a computer is connected)'
        \           : p.source ==# 'battery' ? 'battery' : 'unknown'),
        \ s:Row('%-8s %s', 'Battery', bat),
        \ s:Row('%-8s %s', 'Reader', !p.reader ? 'no wake button: :EspSleep is a deep sleep'
        \           : p.idle_min ? printf('after %d min without a key, on battery', p.idle_min)
        \           : 'only on the wake button or :EspSleep'),
        \ s:Row('%-8s %s', 'Deep', p.reader && p.deep_min
        \           ? printf('after %d min in reader mode, if nothing is unsaved', p.deep_min)
        \           : 'only with :EspSleep' . (p.reader ? ' deep' : '')),
        \ s:Row('%-8s %s', 'Idle', printf('%d s since the last key', p.idle_s)),
        \ s:Row('%-8s %s', 'Sleeps', printf('%d reader, %d deep%s', p.sleeps, p.deep_sleeps,
        \           empty(p.last_wake) ? '' : '; last woken by ' . p.last_wake)),
        \ ]
  call s:Show('Power', lines, 0, function('esp#Power'))
endfunction

" ------------------------------------------------------------- :EspGpio --

" Words :EspGpio accepts after the pin, and what each does.
let s:gpio_modes = {'in': 'in', 'pullup': 'in_pullup', 'pulldown': 'in_pulldown',
      \ 'out': 'out', 'od': 'od', 'reset': 'off'}
let s:gpio_words = ['on', 'off', 'read', 'toggle'] + keys(s:gpio_modes)

function! esp#Gpio(pin, ...) abort
  if a:pin !~# '^\d\+$'
    echohl ErrorMsg | echomsg 'EspGpio: not a pin number: ' . a:pin | echohl None
    return
  endif
  let pin = str2nr(a:pin)
  let what = a:0 ? a:1 : 'read'
  if what ==# 'on' || what ==# '1' || what ==# 'high'
    call esp_gpio_write(pin, 1)
  elseif what ==# 'off' || what ==# '0' || what ==# 'low'
    call esp_gpio_write(pin, 0)
  elseif what ==# 'toggle'
    call esp_gpio_write(pin, !esp_gpio_read(pin))
  elseif has_key(s:gpio_modes, what)
    if esp_gpio_mode(pin, s:gpio_modes[what])
      echo 'GPIO ' . pin . ': mode ' . what
    endif
    return
  elseif what !=# 'read'
    echohl ErrorMsg
    echomsg 'EspGpio: unknown action "' . what . '" (' . join(sort(copy(s:gpio_words)), ', ') . ')'
    echohl None
    return
  endif
  let level = esp_gpio_read(pin)
  echo 'GPIO ' . pin . ': ' . level . (level ? ' (high)' : ' (low)')
endfunction

function! esp#GpioComplete(lead, line, pos) abort
  let words = split(a:line[: a:pos - 1], '\s\+', 1)
  if len(words) <= 2
    let cands = map(esp_gpio_pins(), 'string(v:val)')
  else
    let cands = sort(copy(s:gpio_words))
  endif
  return filter(cands, 'v:val =~# "^" . a:lead')
endfunction

" -------------------------------------------------------------- :EspNvs --

" :EspNvs                        list everything
" :EspNvs {ns}                   list one namespace
" :EspNvs {ns} {key}             show a value
" :EspNvs {ns} {key} {value}     set it (digits are stored as a number)
" :EspNvs! {ns} {key}            erase it
function! esp#Nvs(bang, ...) abort
  if a:bang
    if a:0 != 2
      echohl ErrorMsg | echomsg 'EspNvs!: needs {namespace} {key}' | echohl None
    elseif esp_nvs_erase(a:1, a:2)
      echo 'Erased ' . a:1 . ' ' . a:2
    else
      echo 'No ' . a:1 . ' ' . a:2 . ' to erase'
    endif
  elseif a:0 >= 3
    let value = join(a:000[2:], ' ')
    call esp_nvs_set(a:1, a:2, value =~# '^-\=\d\+$' ? str2nr(value) : value)
    echo a:1 . ' ' . a:2 . ' = ' . string(esp_nvs_get(a:1, a:2))
  elseif a:0 == 2
    echo a:1 . ' ' . a:2 . ' = ' . string(esp_nvs_get(a:1, a:2))
  else
    call s:NvsList(a:0 ? a:1 : '')
  endif
endfunction

function! s:NvsList(ns) abort
  let entries = empty(a:ns) ? esp_nvs_list() : esp_nvs_list(a:ns)
  let rows = [['Namespace', 'Key', 'Type', 'Value']]
  for e in sort(entries, {a, b -> a.namespace . "\n" . a.key < b.namespace . "\n" . b.key ? -1 : 1})
    let value = has_key(e, 'value') ? (type(e.value) == v:t_string ? string(e.value) : e.value)
          \ : has_key(e, 'size') ? '<' . e.size . ' bytes>' : '?'
    call add(rows, [e.namespace, e.key, e.type, value])
  endfor
  call s:Show('NVS', ['NVS' . (empty(a:ns) ? '' : ', namespace ' . a:ns) . ': ' . len(entries)
        \ . ' entries   (R refresh, q close)'] + s:Table('NVS', rows, 'llll'),
        \ 2, function('s:NvsList', [a:ns]))
endfunction

function! esp#NvsComplete(lead, line, pos) abort
  let words = split(a:line[: a:pos - 1], '\s\+', 1)
  let entries = esp_nvs_list()
  if len(words) <= 2
    let cands = map(copy(entries), 'v:val.namespace')
  elseif len(words) == 3
    let cands = map(filter(copy(entries), 'v:val.namespace ==# words[1]'), 'v:val.key')
  else
    return []
  endif
  return filter(uniq(sort(cands)), 'v:val =~# "^" . a:lead')
endfunction

" -------------------------------------------------------- :EspNet/:EspGet --

function! esp#Net() abort
  let n = esp_net_status()
  let lines = ['Network   (R refresh, q close)',
        \ s:Row('%-10s %s', 'Interface', n.iface . (empty(n.ssid) ? '' : ' (' . n.ssid . ')')),
        \ s:Row('%-10s %s', 'Status', n.up ? 'up' : (n.iface ==# 'none' ? 'no network interface' : 'no link or no address yet')),
        \ ]
  if n.up
    call extend(lines, [
          \ s:Row('%-10s %s', 'Address', n.ip),
          \ s:Row('%-10s %s', 'Netmask', n.netmask),
          \ s:Row('%-10s %s', 'Gateway', n.gw),
          \ s:Row('%-10s %s', 'DNS', empty(n.dns) ? '-' : n.dns)])
    if n.rssi
      call add(lines, s:Row('%-10s %d dBm', 'Signal', n.rssi))
    endif
  endif
  if !empty(n.mac)
    call add(lines, s:Row('%-10s %s', 'MAC', n.mac))
  endif
  call s:Show('Net', lines, 0, function('esp#Net'))
endfunction

" --------------------------------------------------------------- :EspTime --

" :EspTime                          the date and time, and where they come from
" :EspTime set {YYYY-MM-DD} {HH:MM[:SS]}   set it by hand (local time)
" :EspTime tz [{zone}]              the time zone: a name, or a POSIX TZ string
" :EspTime ntp on|off               set it from the network by itself, or not
" :EspTime server {host}            the NTP server
" :EspTime sync                     ask the NTP server now
function! esp#TimeComplete(lead, line, pos) abort
  let words = split(a:line[: a:pos - 1], '\s\+', 1)
  if len(words) <= 2
    return filter(['set', 'tz', 'ntp', 'server', 'sync'], 'v:val =~# "^" . a:lead')
  elseif words[1] ==# 'tz'
    return filter(copy(esp_time().zones), 'v:val =~? "^" . a:lead')
  elseif words[1] ==# 'ntp'
    return filter(['on', 'off'], 'v:val =~# "^" . a:lead')
  endif
  return []
endfunction

function! esp#Time(...) abort
  let what = a:0 ? a:1 : ''
  if what ==# 'set' && a:0 >= 3
    call esp_time({'set': a:2 . ' ' . a:3})
  elseif what ==# 'tz' && a:0 == 2
    call esp_time({'tz': a:2})
  elseif what ==# 'tz' && a:0 == 1
    echo 'Time zones: ' . join(esp_time().zones, ', ') . "\nor a POSIX TZ string, like PST8PDT,M3.2.0,M11.1.0"
    return
  elseif what ==# 'ntp' && a:0 == 2 && (a:2 ==# 'on' || a:2 ==# 'off')
    call esp_time({'ntp': a:2 ==# 'on'})
  elseif what ==# 'server' && a:0 == 2
    call esp_time({'server': a:2})
  elseif what ==# 'sync' && a:0 == 1
    echo 'Asking ' . esp_time().server . '...'
    redraw
    call esp_time({'sync': 10})
  elseif what !=# ''
    echoerr 'Usage: :EspTime [set {YYYY-MM-DD} {HH:MM} | tz [{zone}] | ntp on|off | server {host} | sync]'
    return
  endif
  let t = esp_time()
  let src = {'rtc': 'the RTC chip', 'ntp': 'NTP', 'manual': 'set by hand'}
  let lines = ['Time   (R refresh, q close)',
        \ s:Row('%-8s %s', 'Now', t.valid ? strftime('%a %Y-%m-%d %H:%M:%S', t.now) . ' ' . t.zone
        \           : 'not set (' . t.local . ')'),
        \ s:Row('%-8s %s', 'Zone', t.tz . (t.tz ==# t.tz_posix ? '' : ' (' . t.tz_posix . ')')),
        \ s:Row('%-8s %s', 'Source', !t.valid ? '-' : get(src, t.source, 'kept through a restart')
        \           . (t.synced ? ', last synced ' . strftime('%Y-%m-%d %H:%M', t.synced) : '')),
        \ s:Row('%-8s %s', 'NTP', (t.ntp ? 'on, ' : 'off, ') . t.server),
        \ s:Row('%-8s %s', 'RTC', empty(t.rtc) ? 'none on this board'
        \           : !t.rtc_ok ? t.rtc . ', not answering'
        \           : t.rtc_time < 0 ? t.rtc . ', time lost (never set, or no power while off)'
        \           : t.rtc . ', reads ' . strftime('%Y-%m-%d %H:%M:%S', t.rtc_time)),
        \ ]
  call s:Show('Time', lines, 0, function('esp#Time'))
endfunction

" :EspGet[!] {url} [{file}]: download; the file defaults to the URL's last
" path component in the current directory.
function! esp#Get(bang, url, ...) abort
  let name = a:0 ? a:1 : matchstr(substitute(a:url, '[?#].*', '', ''), '[^/]\+$')
  if empty(name) || a:url =~# '^[a-z]\+://[^/]*/\=$'
    let name = 'index.html'
  endif
  let path = fnamemodify(name, ':p')
  if !a:bang && (filereadable(path) || isdirectory(path))
    echohl ErrorMsg | echomsg 'EspGet: ' . path . ' exists (add ! to replace it)' | echohl None
    return
  endif
  echo 'Downloading ' . a:url . ' ...'
  redraw
  let r = esp_http_get(a:url, path)
  if !empty(r)
    redraw
    echo printf('%s: %d bytes', fnamemodify(r.path, ':~:.'), r.size)
  endif
endfunction

" --------------------------------------------------------------- :EspWifi* --

function! esp#WifiScan() abort
  echo 'Scanning...'
  redraw
  let aps = sort(esp_wifi_scan(), {a, b -> b.rssi - a.rssi})
  let rows = [['Network', 'Signal', 'Chan', 'Security']]
  for a in aps
    call add(rows, [empty(a.ssid) ? '(hidden)' : a.ssid, a.rssi . ' dB', a.channel, a.auth])
  endfor
  call s:Show('WifiScan', ['WiFi networks: ' . len(aps) . '   (R rescan, q close)']
        \ + s:Table('WifiScan', rows, 'lrrl'), 2, function('esp#WifiScan'))
endfunction

" ----------------------------------------------------------------- :EspBle* --

" :EspBleScan [{seconds}]: Bluetooth LE devices in range, strongest first.
function! esp#BleScan(...) abort
  let secs = a:0 ? str2nr(a:1) : 5
  echo 'Scanning for Bluetooth devices (' . secs . ' s)...'
  redraw
  let devs = sort(esp_ble_scan(secs), {a, b -> b.rssi - a.rssi})
  let rows = [['Name', 'Address', 'Type', 'Signal', '']]
  for d in devs
    call add(rows, [empty(d.name) ? '(no name)' : d.name, d.addr, d.addr_type, d.rssi . ' dB',
          \ d.connectable ? 'connectable' : ''])
  endfor
  call s:Show('BleScan', ['Bluetooth LE devices: ' . len(devs) . '   (R rescan, q close)']
        \ + s:Table('BleScan', rows, 'lllrl'), 2, function('esp#BleScan', [secs]))
endfunction

" ----------------------------------------------------------------- :EspConsole --

" :EspConsole [on|off]: whether Vim's output also goes to the serial console,
" on boards where a display shows it.
function! esp#ConsoleComplete(lead, line, pos) abort
  return filter(['on', 'off'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#Console(...) abort
  if a:0
    if a:1 !=# 'on' && a:1 !=# 'off'
      echoerr 'Usage: :EspConsole [on|off]'
      return
    endif
    call esp_console_output(a:1 ==# 'on')
  endif
  echo 'Serial console output: ' . (esp_console_output() ? 'on'
        \ : 'off (the screen only; type a key on the serial console to turn it back on)')
endfunction

" ------------------------------------------------------------------ :EspFlip --

" :EspFlip [on|off]: the display turned 180 degrees, or back; without an
" argument, turn it over. Which way is up depends on how the board stands, so
" it only says that it flipped. Kept for the next start.
function! esp#Flip(...) abort
  if !esp_display().active
    echoerr 'EspFlip: no display'
    return
  endif
  if a:0 && a:1 !=# 'on' && a:1 !=# 'off'
    echoerr 'Usage: :EspFlip [on|off]'
    return
  endif
  call esp_display_flip(a:0 ? a:1 ==# 'on' : !esp_display_flip())
  echo 'Display has flipped'
endfunction

" ------------------------------------------------------------------ :EspFont --

" :EspFont [{size}]: the display's font, named for the screen size it gives
" ("80x24"). Without an argument, lists them; a font's own name
" ("terminus-10x20") picks between two that give the same size.
function! esp#FontComplete(lead, line, pos) abort
  let sizes = []
  for f in esp_display().fonts
    if index(sizes, f.size) < 0
      call add(sizes, f.size)
    endif
  endfor
  return filter(sizes, 'v:val =~# "^" . a:lead')
endfunction

function! esp#Font(...) abort
  let d = esp_display()
  if !d.active
    echoerr 'EspFont: no display'
    return
  endif
  if a:0
    call esp_display_font(a:1)
    let d = esp_display()
    echo printf('Screen %dx%d (%s)', d.cols, d.rows, d.font)
    return
  endif
  " Columns lined up: sizes on their "x", the font's family and cell size.
  let rows = [['', 'Screen', 'Font', 'Glyph']]
  for f in d.fonts
    let family = substitute(f.font, '-\d\+x\d\+$', '', '')
    let family = substitute(toupper(family[0]) . family[1:], '-', ' ', 'g')
    call add(rows, [f.font ==# d.font ? '>' : ' ',
          \ printf('%*dx%-*d', 3, f.cols, 2, f.rows), family,
          \ printf('%*dx%-*d', 2, f.width, 2, f.height)])
  endfor
  let widths = map(range(4), {i -> max(map(copy(rows), {_, r -> strwidth(r[i])}))})
  for r in rows
    echo join(map(r[:-2], {i, c -> c . repeat(' ', widths[i] - strwidth(c))}) + [r[-1]], '  ')
  endfor
endfunction

" ------------------------------------------------------------ :EspBtKeyboard --

" :EspBtKeyboard                  status of the Bluetooth keyboard
" :EspBtKeyboard scan [{seconds}] list keyboards (HID devices) in range
" :EspBtKeyboard pair {n|addr}    pair with one from the list (or by address)
" :EspBtKeyboard forget           disconnect and forget it
let s:kbd_found = []

function! esp#BtKeyboardComplete(lead, line, pos) abort
  return filter(['scan', 'pair', 'list', 'forget'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#BtKeyboard(...) abort
  let what = a:0 ? a:1 : ''
  if what ==# ''
    let k = esp_bt_keyboard()
    let lines = ['Bluetooth keyboard   (R refresh, q close)',
          \ s:Row('%-10s %s', 'State', k.connected ? 'connected' : (k.paired ? 'paired, not connected' : 'none paired'))]
    if k.connected
      call add(lines, s:Row('%-10s %s  %s', 'Keyboard', k.name, k.addr))
      if k.battery >= 0
        call add(lines, s:Row('%-10s %d%%', 'Battery', k.battery))
      endif
    endif
    if !empty(k.layout)
      call add(lines, s:Row('%-10s %s', 'Reports', k.layout))
    endif
    if !empty(k.last_report)
      call add(lines, s:Row('%-10s %s', 'Last key', k.last_report))
    endif
    for b in esp_bt_keyboard_list()
      call add(lines, s:Row('%-10s %s  %s%s', 'Paired', empty(b.name) ? '(no name)' : b.name, b.addr,
            \ b.connected ? '  (connected)' : b.last ? '  (last used)' : ''))
    endfor
    call s:Show('BtKeyboard', lines, 1, function('esp#BtKeyboard'))
  elseif what ==# 'list'
    let rows = []
    let n = 1
    for b in esp_bt_keyboard_list()
      call add(rows, [n, empty(b.name) ? '(no name)' : b.name, b.addr,
            \ b.connected ? 'connected' : b.last ? 'last used' : ''])
      let n += 1
    endfor
    call s:Show('BtKeyboardList', ['Paired keyboards: ' . len(rows)
          \ . '   (:EspBtKeyboard forget {n}, q close)'] + s:Table('BtKeyboardList', rows, 'rlll'),
          \ 1, function('esp#BtKeyboard', ['list']))
  elseif what ==# 'scan'
    let secs = a:0 > 1 ? str2nr(a:2) : 8
    echo 'Put the keyboard in pairing mode. Scanning (' . secs . ' s)...'
    redraw
    let s:kbd_found = sort(filter(esp_ble_scan(secs), 'v:val.hid'), {a, b -> b.rssi - a.rssi})
    let rows = []
    let n = 1
    for d in s:kbd_found
      call add(rows, [n, empty(d.name) ? '(no name)' : d.name, d.addr, d.rssi . ' dB'])
      let n += 1
    endfor
    call s:Show('BtKeyboardScan', ['Keyboards in range: ' . len(s:kbd_found)
          \ . '   (:EspBtKeyboard pair {n}, q close)'] + s:Table('BtKeyboardScan', rows, 'rllr'),
          \ 1, function('esp#BtKeyboard', ['scan', secs]))
  elseif what ==# 'pair'
    if a:0 < 2
      echoerr 'Usage: :EspBtKeyboard pair {n|addr}'
      return
    endif
    let arg = a:2
    if arg =~# '^\d\+$' && str2nr(arg) >= 1 && str2nr(arg) <= len(s:kbd_found)
      let d = s:kbd_found[str2nr(arg) - 1]
      let [addr, type] = [d.addr, d.addr_type]
    else
      let [addr, type] = [arg, a:0 > 2 ? a:3 : 'public']
    endif
    echo 'Pairing with ' . addr . ' (up to 30 s)...'
    redraw
    if esp_bt_keyboard_pair(addr, type)
      redraw
      let notice = esp_bt_keyboard_notice()     " taken here, not repeated when idle
      echo (empty(notice) ? 'Paired' : notice) . '. Type on the keyboard; it reconnects by itself from now on.'
    endif
  elseif what ==# 'forget'
    if a:0 < 2
      if esp_bt_keyboard_forget()
        echo 'Every Bluetooth keyboard forgotten'
      endif
      return
    endif
    let bonds = esp_bt_keyboard_list()
    let arg = a:2
    if arg =~# '^\d\+$' && str2nr(arg) >= 1 && str2nr(arg) <= len(bonds)
      let b = bonds[str2nr(arg) - 1]
      let [addr, name] = [b.addr, empty(b.name) ? b.addr : b.name]
    else
      let [addr, name] = [arg, arg]
    endif
    if esp_bt_keyboard_forget(addr)
      echo 'Forgot ' . name
    endif
  else
    echoerr 'Usage: :EspBtKeyboard [scan [{seconds}] | pair {n|addr} | list | forget [{n|addr}]]'
  endif
endfunction

" :EspWifiConnect [{ssid} [{password}]]: the network is saved, password and
" all, and joined again at every start. With no arguments, or the saved
" network's name alone, joins it again with its saved password; another network
" without a password asks for one (leave it empty for an open network).
function! esp#WifiConnect(...) abort
  let saved = esp_wifi_saved()
  if a:0 == 0 || (a:0 == 1 && a:1 ==# saved)
    if empty(saved)
      echoerr 'EspWifiConnect: no network is saved; give its name'
      return
    endif
    let ssid = saved
    let args = [saved]
  elseif a:0 >= 2
    let ssid = a:1
    let args = [a:1, a:2]
  else
    let ssid = a:1
    call inputsave()
    let pw = inputsecret('Password for ' . ssid . ' (empty if open): ')
    call inputrestore()
    let args = [ssid, pw]
  endif
  if !call('esp_wifi_connect', args)
    return
  endif
  " Wait up to 20 s for an address, showing progress; CTRL-C stops waiting.
  for i in range(40)
    let n = esp_net_status()
    if n.up
      redraw
      echo 'Connected to ' . n.ssid . ': ' . n.ip . ' (' . n.rssi . ' dBm)'
      return
    endif
    redraw
    echo 'Connecting to ' . ssid . repeat('.', i % 4 + 1)
    sleep 500m
  endfor
  redraw
  echohl WarningMsg
  echomsg 'EspWifiConnect: no connection yet; it keeps trying in the background (:EspNet)'
  echohl None
endfunction
