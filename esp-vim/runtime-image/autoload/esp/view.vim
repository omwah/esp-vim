" The view windows and tables the :Esp* commands show (autoload/esp.vim, the
" files beside this one, esp/git.vim). Each command's code is in a small file
" of its own, so that a board with little memory loads only what it uses:
" Vim keeps every function it has read.

" ---------------------------------------------------------------- views --

" Show {lines} in the view window called {name}, reusing it if it is open.
" {header} is the line number of a table header to highlight (0: none).
" {Refresh} is what R calls to redraw it.
function! esp#view#Show(name, lines, header, Refresh) abort
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
    " No undo: a view is redrawn whole (R), and on a board with little memory
    " every redraw's undo would add up.
    setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted undolevels=-1
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
function! esp#view#Size(n) abort
  if a:n < 1024
    return a:n . ' B'
  elseif a:n < 1024 * 1024
    return printf('%.1f KB', a:n / 1024.0)
  endif
  return printf('%.1f MB', a:n / 1048576.0)
endfunction

function! esp#view#Row(fmt, ...) abort
  return substitute(call('printf', [a:fmt] + a:000), '\s\+$', '', '')
endfunction

" The width a view has: its window's, when it is already open, else the
" screen's (esp#view#Show opens it the full width).
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
function! esp#view#Table(name, rows, align) abort
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
