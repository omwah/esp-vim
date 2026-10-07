" :EspInfo, :EspHeap, :EspLog, :EspReboot -- every build has them
" (plugin/esp.vim; on a board without PSRAM, vimrc-lowmem).

" ------------------------------------------------------------- :EspInfo --

" :EspLog[!]: the system's log lines kept off the screen; ! empties it.
function! esp#sys#Log(...) abort
  let lines = esp_log(a:0 && a:1)
  if a:0 && a:1
    echo 'EspLog: emptied (' . len(lines) . ' lines)'
    return
  endif
  call esp#view#Show('Log', ['Log -- the system''s messages, oldest first   (R refresh, q close)']
        \ + (empty(lines) ? ['(none)'] : lines), 1, function('esp#sys#Log'))
  normal! G
endfunction

function! esp#sys#Info() abort
  let i = esp_info()
  let up = i.uptime_ms / 1000
  let lines = [i.chip . ' -- system information   (R refresh, q close)',
        \ esp#view#Row('%-10s %s, revision %s', 'Chip', i.chip, i.revision),
        \ esp#view#Row('%-10s %d cores at %d MHz', 'CPU', i.cores, i.cpu_mhz),
        \ esp#view#Row('%-10s %s', 'Features', empty(i.features) ? 'none' : join(i.features, ', ')),
        \ esp#view#Row('%-10s %s', 'Flash', esp#view#Size(get(i, 'flash', 0))),
        \ esp#view#Row('%-10s %s', 'PSRAM', i.psram ? esp#view#Size(i.psram) : 'none'),
        \ ]
  if has_key(i, 'board')
    call insert(lines, esp#view#Row('%-10s %s', 'Board', i.board
          \ . (has_key(i, 'panel') ? ', display ' . i.panel : '')), 1)
  endif
  if has_key(i, 'mac')
    call add(lines, esp#view#Row('%-10s %s', 'MAC', i.mac))
  endif
  call extend(lines, [
        \ esp#view#Row('%-10s %s', 'ESP-IDF', i.idf),
        \ esp#view#Row('%-10s %s', 'Vim', i.vim),
        \ esp#view#Row('%-10s %d:%02d:%02d', 'Uptime', up / 3600, up / 60 % 60, up % 60),
        \ esp#view#Row('%-10s %s', 'Reset', i.reset),
        \ ])
  call esp#view#Show('Info', lines, 0, function('esp#sys#Info'))
endfunction

" ------------------------------------------------------------- :EspHeap --

function! esp#sys#Heap() abort
  let h = esp_heap()
  let rows = [['Heap', 'free', 'largest', 'lowest', 'total']]
  for name in ['internal', 'psram', 'dma']
    let x = h[name]
    if x.total
      call add(rows, [name, esp#view#Size(x.free), esp#view#Size(x.largest), esp#view#Size(x.min_free), esp#view#Size(x.total)])
    endif
  endfor
  call extend(rows, [[], ['Vim', 'in use', 'peak', 'budget'],
        \ ['', esp#view#Size(h.vim.used), esp#view#Size(h.vim.peak), h.vim.budget ? esp#view#Size(h.vim.budget) : 'none']])
  let py = esp_py_heap()
  if py.running
    call extend(rows, [[], ['Python', 'in use', 'free', 'total'],
          \ ['', esp#view#Size(py.used), esp#view#Size(py.free), esp#view#Size(py.total)]])
  endif
  call esp#view#Show('Heap', ['Memory   (R refresh, q close)'] + esp#view#Table('Heap', rows, 'lrrrr'),
        \ 2, function('esp#sys#Heap'))
endfunction

" ----------------------------------------------------------- :EspReboot --

function! esp#sys#Reboot(bang) abort
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
