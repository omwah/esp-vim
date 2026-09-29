" The :EspPy* commands (plugin/esp.vim) over the esp_py_*() builtins
" (MicroPython, esp-vim/components/vim/api/esp_api_py.c). See ":help esp-python".
"
" Python runs in one interpreter, in its module __main__: what one command
" defines, the next can use, until :EspPyReset.

" Set a Vim variable or option from Python (vim.py's vars and options): the
" value arrives already converted, whatever its type.
function! esp#py#Let(name, value) abort
  execute 'let ' . a:name . ' = a:value'
endfunction

function! s:ShowError(r) abort
  echohl ErrorMsg
  for line in split(a:r.traceback, "\n")
    echomsg line
  endfor
  echohl None
endfunction

" :EspPy {code}: one statement, as typed at Python's prompt: an expression's
" value is printed.
function! esp#py#Py(code) abort
  let r = esp_py_exec(a:code, {'mode': 'single', 'name': '<EspPy>'})
  if !r.ok
    call s:ShowError(r)
  endif
endfunction

" The [Python] output window: opened (or found) and emptied. Returns its
" buffer number, and leaves the window that was current, current.
function! s:Output() abort
  let back = win_getid()
  let buf = bufnr('^\[Python\]$')
  let win = buf > 0 ? bufwinid(buf) : -1
  if win == -1
    botright 10new
    setlocal buftype=nofile bufhidden=hide noswapfile nobuflisted
    setlocal nonumber norelativenumber nospell
    silent file [Python]
    nnoremap <buffer> <silent> q :close<CR>
    let buf = bufnr('%')
  endif
  call setbufvar(buf, '&modifiable', 1)
  silent call deletebufline(buf, 1, '$')
  call win_gotoid(back)
  return buf
endfunction

" A traceback's "File ..., line N" entries as quickfix items, the innermost
" (where the error is) first. "<buffer N>" is buffer N.
function! s:Quickfix(r) abort
  let items = []
  for line in split(a:r.traceback, "\n")
    let m = matchlist(line, '^\s*File "\(.\{-}\)", line \(\d\+\)')
    if empty(m)
      continue
    endif
    let item = {'lnum': str2nr(m[2]), 'text': a:r.error, 'type': 'E'}
    let b = matchlist(m[1], '^<buffer \(\d\+\)>$')
    if !empty(b)
      let item.bufnr = str2nr(b[1])
    elseif m[1] =~# '^<'
      continue                      " <EspPy>, <string>: nowhere to go
    else
      let item.filename = m[1]
    endif
    call insert(items, item)
  endfor
  call setqflist([], ' ', {'title': 'EspPyRun', 'items': items})
  return len(items)
endfunction

" :[range]EspPyRun [file]: run a file, or the current buffer (or its lines in
" range), output in the [Python] window; a traceback goes to quickfix too.
function! esp#py#Run(line1, line2, range, file) abort
  if !empty(a:file)
    let path = fnamemodify(expand(a:file), ':p')
    if !filereadable(path)
      echohl ErrorMsg | echomsg 'EspPyRun: cannot read ' . path | echohl None
      return
    endif
    let lines = readfile(path)
    let name = path
  else
    " Blank lines before a range keep the line numbers the buffer's.
    let lines = repeat([''], a:range ? a:line1 - 1 : 0)
          \ + getline(a:range ? a:line1 : 1, a:range ? a:line2 : '$')
    let name = empty(&buftype) && !empty(expand('%')) ? expand('%:p')
          \ : '<buffer ' . bufnr('%') . '>'
  endif
  let out = s:Output()
  let r = esp_py_exec(lines, {'name': name, 'buf': out})
  if !r.ok
    let tb = split(r.traceback, "\n")
    if getbufline(out, 1, '$') == ['']
      call setbufline(out, 1, tb)
    else
      call appendbufline(out, '$', tb)
    endif
  endif
  call setbufvar(out, '&modified', 0)
  call win_execute(bufwinid(out), 'normal! G')
  redraw
  if r.ok
    if getqflist({'title': 1}).title ==# 'EspPyRun'
      call setqflist([], ' ', {'title': 'EspPyRun', 'items': []})
    endif
    echo 'EspPyRun: done'
  else
    let n = s:Quickfix(r)
    echohl ErrorMsg
    echomsg r.error
    echohl None
    if n
      echomsg 'EspPyRun: :cc goes to the error, :cwindow lists the traceback'
    endif
  endif
endfunction

" :EspPyReset: a new interpreter; every variable, module and open file goes.
function! esp#py#Reset() abort
  if esp_py_reset()
    echo 'EspPyReset: Python will start afresh'
  endif
endfunction
