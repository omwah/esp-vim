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
    let b:esp_view = a:name
    silent execute 'file esp://' . tolower(a:name)
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
