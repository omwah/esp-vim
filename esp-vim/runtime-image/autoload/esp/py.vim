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

" --------------------------------------------------------------- the REPL --
"
" :EspPyRepl: a buffer that works as Python's prompt. The last line is the
" prompt ">>> " (or "... " inside a block); <CR> there runs it, and the output
" follows. Vim's prompt buffers need +channel, which this build hasn't, so
" this is a plain buffer with Insert-mode mappings.

let s:ps1 = '>>> '
let s:ps2 = '... '

function! s:ReplLine() abort
  let text = getline('$')
  return text =~# '^\V' . s:ps1 ? text[len(s:ps1):]
        \ : text =~# '^\V' . s:ps2 ? text[len(s:ps2):] : text
endfunction

" A new prompt line, with the cursor at its end.
function! s:ReplPrompt(prompt, ...) abort
  call append('$', a:prompt . (a:0 ? a:1 : ''))
  call cursor(line('$'), col([line('$'), '$']))
endfunction

function! esp#py#Repl() abort
  let buf = bufnr('^\[Python REPL\]$')
  let win = buf > 0 ? bufwinid(buf) : -1
  if win != -1
    call win_gotoid(win)
  elseif buf > 0
    execute 'botright 12split | buffer ' . buf
  else
    botright 12new
    setlocal buftype=nofile bufhidden=hide noswapfile nobuflisted
    setlocal nonumber norelativenumber nospell nolist
    silent file [Python REPL]
    let b:repl_block = []
    let b:repl_hist = []
    let b:repl_hidx = 0
    call setline(1, 'MicroPython '
          \ . esp_py_eval('".".join(str(n) for n in __import__("sys").implementation.version[:3])')
          \ . ' in Vim.  <CR> runs, <Up>/<Down> history, <Tab> completes, :q closes.')
    call s:ReplPrompt(s:ps1)
    syntax match EspPyReplPrompt /^\(>>>\|\.\.\.\) /
    syntax match EspPyReplTitle /\%1l.*/
    highlight default link EspPyReplPrompt Identifier
    highlight default link EspPyReplTitle Comment
    inoremap <buffer> <silent> <CR> <Cmd>call esp#py#ReplEnter()<CR>
    inoremap <buffer> <silent> <expr> <Up> line('.') == line('$') ? "\<Cmd>call esp#py#ReplHistory(-1)\<CR>" : "\<Up>"
    inoremap <buffer> <silent> <expr> <Down> line('.') == line('$') ? "\<Cmd>call esp#py#ReplHistory(1)\<CR>" : "\<Down>"
    inoremap <buffer> <silent> <Tab> <C-R>=esp#py#ReplTab()<CR>
    inoremap <buffer> <silent> <C-C> <Cmd>call esp#py#ReplCancel()<CR>
    nnoremap <buffer> <silent> <CR> <Cmd>call esp#py#ReplEnter()<CR>
  endif
  call cursor(line('$'), col([line('$'), '$']))
  startinsert!
endfunction

" <CR>: on the prompt line, add it to the block and run the block when it is
" complete; on an earlier line, bring that line's text down to the prompt.
function! esp#py#ReplEnter() abort
  if line('.') != line('$')
    let text = getline('.')
    let text = text =~# '^\V' . s:ps1 ? text[len(s:ps1):]
          \ : text =~# '^\V' . s:ps2 ? text[len(s:ps2):] : text
    call setline('$', (empty(b:repl_block) ? s:ps1 : s:ps2) . text)
    call cursor(line('$'), col([line('$'), '$']))
    return
  endif
  let text = s:ReplLine()
  if text =~# '^\s*$'
    let text = ''                   " indentation alone ends a block
  elseif empty(b:repl_hist) || b:repl_hist[-1] !=# text
    call add(b:repl_hist, text)     " history is line by line, as readline's
  endif
  let b:repl_hidx = len(b:repl_hist)
  call add(b:repl_block, text)
  let src = join(b:repl_block, "\n")
  if !empty(text) && esp_py_more(src)
    " Inside a block: keep the indentation of the line before.
    call s:ReplPrompt(s:ps2, matchstr(text, '^\s*') . (text =~# ':\s*$' ? '    ' : ''))
  else
    let b:repl_block = []
    if src !~# '^\_s*$'
      let r = esp_py_exec(src, {'mode': 'single', 'name': '<stdin>', 'buf': bufnr('%')})
      if !r.ok
        call append('$', split(r.traceback, "\n"))
      endif
    endif
    call s:ReplPrompt(s:ps1)
  endif
  if mode() !=# 'i'
    startinsert!
  endif
endfunction

" <Up>/<Down> on the prompt line: the lines typed before.
function! esp#py#ReplHistory(step) abort
  if empty(b:repl_hist)
    return
  endif
  let b:repl_hidx = max([0, min([len(b:repl_hist), b:repl_hidx + a:step])])
  let entry = b:repl_hidx < len(b:repl_hist) ? b:repl_hist[b:repl_hidx] : ''
  call setline('$', (empty(b:repl_block) ? s:ps1 : s:ps2) . entry)
  call cursor(line('$'), col([line('$'), '$']))
endfunction

" <Tab>: complete the name before the cursor, or indent when there is none.
function! esp#py#ReplTab() abort
  let before = strpart(getline('.'), 0, col('.') - 1)
  let before = before =~# '^\V' . s:ps1 ? before[len(s:ps1):]
        \ : before =~# '^\V' . s:ps2 ? before[len(s:ps2):] : before
  if before =~# '^\s*$' || line('.') != line('$')
    return '    '
  endif
  let words = esp_py_complete(before)
  if empty(words)
    return ''
  endif
  let frag = matchstr(before, '\k*$')
  call complete(col('.') - len(frag), words)
  return ''
endfunction

" CTRL-C at the prompt: drop the block being typed.
function! esp#py#ReplCancel() abort
  let b:repl_block = []
  call append('$', 'KeyboardInterrupt')
  call s:ReplPrompt(s:ps1)
endfunction
