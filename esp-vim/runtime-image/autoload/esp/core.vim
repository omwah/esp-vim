" The :Esp* commands every build has (plugin/esp.vim; on a board without
" PSRAM, vimrc-lowmem), and the views the rest share. Kept apart from
" autoload/esp.vim, so that the commands a board with little memory defines
" don't load all of that: Vim couldn't hold it, and stopped partway with
" "Missing :endfunction".

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

" The same, for the other autoload scripts (esp.vim, esp/git.vim).
function! esp#core#View(name, lines, header, Refresh) abort
  call s:Show(a:name, a:lines, a:header, a:Refresh)
endfunction

function! esp#core#Table(name, rows, align) abort
  return s:Table(a:name, a:rows, a:align)
endfunction

function! esp#core#Size(n) abort
  return s:Size(a:n)
endfunction

function! esp#core#Row(fmt, ...) abort
  return call('s:Row', [a:fmt] + a:000)
endfunction

" ------------------------------------------------------------- :EspInfo --

" :EspLog[!]: the system's log lines kept off the screen; ! empties it.
function! esp#core#Log(...) abort
  let lines = esp_log(a:0 && a:1)
  if a:0 && a:1
    echo 'EspLog: emptied (' . len(lines) . ' lines)'
    return
  endif
  call s:Show('Log', ['Log -- the system''s messages, oldest first   (R refresh, q close)']
        \ + (empty(lines) ? ['(none)'] : lines), 1, function('esp#core#Log'))
  normal! G
endfunction

function! esp#core#Info() abort
  let i = esp_info()
  let up = i.uptime_ms / 1000
  let lines = [i.chip . ' -- system information   (R refresh, q close)',
        \ s:Row('%-10s %s, revision %s', 'Chip', i.chip, i.revision),
        \ s:Row('%-10s %d cores at %d MHz', 'CPU', i.cores, i.cpu_mhz),
        \ s:Row('%-10s %s', 'Features', empty(i.features) ? 'none' : join(i.features, ', ')),
        \ s:Row('%-10s %s', 'Flash', s:Size(get(i, 'flash', 0))),
        \ s:Row('%-10s %s', 'PSRAM', i.psram ? s:Size(i.psram) : 'none'),
        \ ]
  if has_key(i, 'board')
    call insert(lines, s:Row('%-10s %s', 'Board', i.board
          \ . (has_key(i, 'panel') ? ', display ' . i.panel : '')), 1)
  endif
  if has_key(i, 'mac')
    call add(lines, s:Row('%-10s %s', 'MAC', i.mac))
  endif
  call extend(lines, [
        \ s:Row('%-10s %s', 'ESP-IDF', i.idf),
        \ s:Row('%-10s %s', 'Vim', i.vim),
        \ s:Row('%-10s %d:%02d:%02d', 'Uptime', up / 3600, up / 60 % 60, up % 60),
        \ s:Row('%-10s %s', 'Reset', i.reset),
        \ ])
  call s:Show('Info', lines, 0, function('esp#core#Info'))
endfunction

" ------------------------------------------------------------- :EspHeap --

function! esp#core#Heap() abort
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
        \ 2, function('esp#core#Heap'))
endfunction

" --------------------------------------------------------------- :EspSd --

function! esp#core#SdComplete(lead, line, pos) abort
  return filter(['mount', 'eject'], 'v:val =~# "^" . a:lead')
endfunction

" :EspSd [mount|eject]: the microSD card at /sd.
function! esp#core#Sd(...) abort
  let what = a:0 ? a:1 : ''
  if what ==# 'mount'
    if esp_sd_mount()
      let c = esp_sd()
      echo 'SD card mounted at /sd: ' . c.fs . ', ' . s:Size(c.free) . ' free of ' . s:Size(c.total)
    endif
    return
  elseif what ==# 'eject'
    if esp_sd_eject()
      " Not left in a directory that has gone: relative names would fail.
      " $HOME is /fat, or on a board with no /fat, the card itself: then /.
      if getcwd() =~# '^/sd\%(/\|$\)'
        let home = $HOME =~# '^/sd\%(/\|$\)' ? '/' : $HOME
        execute 'cd' fnameescape(home)
        echo 'SD card unmounted: it can come out (now in ' . home . ')'
      else
        echo 'SD card unmounted: it can come out'
      endif
    endif
    return
  elseif what !=# ''
    echoerr 'Usage: :EspSd [mount | eject]'
    return
  endif
  let c = esp_sd()
  if !c.configured
    echo 'This board has no SD card slot'
    return
  endif
  let rows = [['State', c.mounted ? 'mounted at /sd' : 'not mounted']]
  if c.mounted
    call extend(rows, [['Card', c.name . '  ' . c.type . '  ' . s:Size(c.card_bytes)],
          \ ['Filesystem', c.fs . '  ' . s:Size(c.free) . ' free of ' . s:Size(c.total)],
          \ ['Bus', c.bus . ' at ' . (c.khz / 1000) . ' MHz']])
  else
    call add(rows, ['Bus', c.bus])
    if !empty(c.error)
      call add(rows, ['Why', c.error])
    endif
  endif
  call s:Show('Sd', ['SD card   (' . (c.mounted ? ':EspSd eject' : ':EspSd mount')
        \ . ', R refresh, q close)'] + map(rows, 's:Row("%-11s %s", v:val[0], v:val[1])'),
        \ 0, function('esp#core#Sd'))
endfunction

" ----------------------------------------------------------- :EspReboot --

function! esp#core#Reboot(bang) abort
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

" ----------------------------------------------------------------- :EspConsole --

" :EspConsole [on|off]: whether Vim's output also goes to the serial console,
" on boards where a display shows it.
function! esp#core#ConsoleComplete(lead, line, pos) abort
  return filter(['on', 'off'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#core#Console(...) abort
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
function! esp#core#Flip(...) abort
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
function! esp#core#FontComplete(lead, line, pos) abort
  let sizes = []
  for f in esp_display().fonts
    if index(sizes, f.size) < 0
      call add(sizes, f.size)
    endif
  endfor
  return filter(sizes, 'v:val =~# "^" . a:lead')
endfunction

function! esp#core#Font(...) abort
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
