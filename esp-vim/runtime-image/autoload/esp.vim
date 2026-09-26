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
  let lines = ['Memory   (R refresh, q close)',
        \ s:Row('%-10s %10s %10s %10s %10s', 'Heap', 'free', 'largest', 'lowest', 'total')]
  for name in ['internal', 'psram', 'dma']
    let x = h[name]
    if x.total
      call add(lines, s:Row('%-10s %10s %10s %10s %10s', name,
            \ s:Size(x.free), s:Size(x.largest), s:Size(x.min_free), s:Size(x.total)))
    endif
  endfor
  call extend(lines, ['',
        \ s:Row('%-10s %10s %10s %10s', 'Vim', 'in use', 'peak', 'budget'),
        \ s:Row('%-10s %10s %10s %10s', '', s:Size(h.vim.used), s:Size(h.vim.peak),
        \        h.vim.budget ? s:Size(h.vim.budget) : 'none')])
  call s:Show('Heap', lines, 2, function('esp#Heap'))
endfunction

" ------------------------------------------------------------ :EspTasks --

function! esp#Tasks() abort
  let tasks = sort(esp_tasks(), {a, b -> a.priority != b.priority
        \ ? b.priority - a.priority : a.name < b.name ? -1 : a.name > b.name})
  let lines = ['FreeRTOS tasks: ' . len(tasks) . '   (R refresh, q close)',
        \ s:Row('%-16s %-10s %5s %5s %12s', 'Name', 'State', 'Prio', 'Core', 'Stack free')]
  for t in tasks
    call add(lines, s:Row('%-16s %-10s %5d %5s %12s', t.name, t.state, t.priority,
          \ t.core < 0 ? 'any' : t.core, s:Size(t.stack_free)))
  endfor
  call s:Show('Tasks', lines, 2, function('esp#Tasks'))
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
  let lines = ['NVS' . (empty(a:ns) ? '' : ', namespace ' . a:ns) . ': ' . len(entries)
        \ . ' entries   (R refresh, q close)',
        \ s:Row('%-15s %-15s %-6s %s', 'Namespace', 'Key', 'Type', 'Value')]
  for e in sort(entries, {a, b -> a.namespace . "\n" . a.key < b.namespace . "\n" . b.key ? -1 : 1})
    let value = has_key(e, 'value') ? (type(e.value) == v:t_string ? string(e.value) : e.value)
          \ : has_key(e, 'size') ? '<' . e.size . ' bytes>' : '?'
    call add(lines, s:Row('%-15s %-15s %-6s %s', e.namespace, e.key, e.type, value))
  endfor
  call s:Show('NVS', lines, 2, function('s:NvsList', [a:ns]))
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
  let lines = ['WiFi networks: ' . len(aps) . '   (R rescan, q close)',
        \ s:Row('%-32s %6s %4s  %s', 'Network', 'Signal', 'Chan', 'Security')]
  for a in aps
    call add(lines, s:Row('%-32s %4d dB %4d  %s', empty(a.ssid) ? '(hidden)' : a.ssid, a.rssi, a.channel, a.auth))
  endfor
  call s:Show('WifiScan', lines, 2, function('esp#WifiScan'))
endfunction

" ----------------------------------------------------------------- :EspBle* --

" :EspBleScan [{seconds}]: Bluetooth LE devices in range, strongest first.
function! esp#BleScan(...) abort
  let secs = a:0 ? str2nr(a:1) : 5
  echo 'Scanning for Bluetooth devices (' . secs . ' s)...'
  redraw
  let devs = sort(esp_ble_scan(secs), {a, b -> b.rssi - a.rssi})
  let lines = ['Bluetooth LE devices: ' . len(devs) . '   (R rescan, q close)',
        \ s:Row('%-24s %-17s %-6s %6s  %s', 'Name', 'Address', 'Type', 'Signal', '')]
  for d in devs
    call add(lines, s:Row('%-24s %-17s %-6s %4d dB  %s', empty(d.name) ? '(no name)' : d.name,
          \ d.addr, d.addr_type, d.rssi, d.connectable ? 'connectable' : ''))
  endfor
  call s:Show('BleScan', lines, 2, function('esp#BleScan', [secs]))
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

" ------------------------------------------------------------------ :EspFont --

" :EspFont [{name}]: the display's font. Without a name, lists them; a name
" may be shortened to its size ("10x20") when only one font has that size.
function! esp#FontComplete(lead, line, pos) abort
  return filter(map(copy(esp_display().fonts), 'v:val.name'), 'v:val =~# "^" . a:lead')
endfunction

function! esp#Font(...) abort
  let d = esp_display()
  if !d.active
    echoerr 'EspFont: no display'
    return
  endif
  if a:0
    let names = map(copy(d.fonts), 'v:val.name')
    let name = a:1
    if index(names, name) < 0
      let short = filter(copy(names), 'v:val =~# "-" . escape(name, ".") . "$"')
      if len(short) != 1
        echoerr 'EspFont: no font "' . name . '"; there are ' . join(names, ', ')
        return
      endif
      let name = short[0]
    endif
    call esp_display_font(name)
    let d = esp_display()
    echo printf('Font %s: %dx%d', d.font, d.cols, d.rows)
    return
  endif
  for f in d.fonts
    echo printf('%s %-16s %3dx%-3d  %dx%d px', f.name ==# d.font ? '>' : ' ',
          \ f.name, f.cols, f.rows, f.width, f.height)
  endfor
endfunction

" ------------------------------------------------------------ :EspBtKeyboard --

" :EspBtKeyboard                  status of the Bluetooth keyboard
" :EspBtKeyboard scan [{seconds}] list keyboards (HID devices) in range
" :EspBtKeyboard pair {n|addr}    pair with one from the list (or by address)
" :EspBtKeyboard forget           disconnect and forget it
let s:kbd_found = []

function! esp#BtKeyboardComplete(lead, line, pos) abort
  return filter(['scan', 'pair', 'forget'], 'v:val =~# "^" . a:lead')
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
    if !empty(k.last_report)
      call add(lines, s:Row('%-10s %s', 'Last key', k.last_report))
    endif
    call s:Show('BtKeyboard', lines, 1, function('esp#BtKeyboard'))
  elseif what ==# 'scan'
    let secs = a:0 > 1 ? str2nr(a:2) : 8
    echo 'Put the keyboard in pairing mode. Scanning (' . secs . ' s)...'
    redraw
    let s:kbd_found = sort(filter(esp_ble_scan(secs), 'v:val.hid'), {a, b -> b.rssi - a.rssi})
    let lines = ['Keyboards in range: ' . len(s:kbd_found) . '   (:EspBtKeyboard pair {n}, q close)']
    let n = 1
    for d in s:kbd_found
      call add(lines, s:Row('%2d  %-24s %-17s %4d dB', n, empty(d.name) ? '(no name)' : d.name, d.addr, d.rssi))
      let n += 1
    endfor
    call s:Show('BtKeyboardScan', lines, 1, function('esp#BtKeyboard', ['scan', secs]))
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
      echo 'Paired. Type on the keyboard; it reconnects by itself from now on.'
    endif
  elseif what ==# 'forget'
    if esp_bt_keyboard_forget()
      echo 'Bluetooth keyboard forgotten'
    endif
  else
    echoerr 'Usage: :EspBtKeyboard [scan [{seconds}] | pair {n|addr} | forget]'
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
