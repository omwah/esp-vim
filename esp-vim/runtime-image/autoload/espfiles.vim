" :EspFiles -- a two-pane file manager in the spirit of Midnight Commander.
"
" Every change goes through the esp_fs_*() builtins (components/esp_fs),
" which validate each path -- the same core the web interface will use. This
" file only draws the panes and asks questions.
"
" Each MC function key has a letter alias, because the Tab5 keyboard has no
" F-key row. See ":help :EspFiles".

let s:keys = [
      \ ['<CR>', 'l', 'open: enter a directory, edit a file'],
      \ ['<BS>', 'h', 'parent directory (also "-")'],
      \ ['<Tab>', '', 'switch pane'],
      \ ['<Space>', 't', 'tag / untag, then move down (also <Insert>)'],
      \ ['', 'u', 'untag all'],
      \ ['<F3>', 'v', 'view (read-only, in a new tab)'],
      \ ['<F4>', 'e', 'edit (in a new tab)'],
      \ ['<F5>', 'c', 'copy tagged (or current) to the other pane'],
      \ ['<F6>', 'r', 'move / rename'],
      \ ['<F7>', 'm', 'make directory'],
      \ ['<F8>', 'd', 'delete (also <Del>)'],
      \ ['', 'o', 'other pane shows this directory'],
      \ ['<C-L>', 'R', 'refresh'],
      \ ['<F1>', 'g?', 'this help'],
      \ ['<F10>', 'q', 'quit'],
      \ ]

" ---------------------------------------------------------------- open --

function! espfiles#Open(...) abort
  let left = s:StartDir(a:0 >= 1 ? a:1 : getcwd())
  let right = s:StartDir(a:0 >= 2 ? a:2 : left)
  tabnew
  let t:espfiles = 1
  call s:Setup('left', left)
  rightbelow vnew
  call s:Setup('right', right)
  wincmd h
  echo 'F3/v view  F4/e edit  F5/c copy  F6/r move  F7/m mkdir  F8/d delete  F10/q quit  F1/g? help'
endfunction

function! s:StartDir(dir) abort
  let d = fnamemodify(expand(a:dir), ':p')
  let d = len(d) > 1 ? substitute(d, '/\+$', '', '') : d
  return isdirectory(d) || d ==# '/' ? d : '/fat'
endfunction

function! s:Setup(side, dir) abort
  enew
  setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted nowrap
  setlocal nonumber norelativenumber nolist nospell foldcolumn=0 cursorline
  let &l:fillchars = 'eob: '
  silent execute 'file EspFiles-' . a:side
  setlocal filetype=espfiles
  let b:espfiles = {'side': a:side, 'dir': a:dir, 'tags': {}, 'entries': [], 'space': ''}
  let &l:statusline = ' %{espfiles#Status()}%=%{espfiles#Space()} '
  call s:Syntax()
  call s:Maps()
  call s:Render('')
endfunction

function! espfiles#Status() abort
  let t = len(get(get(b:, 'espfiles', {}), 'tags', {}))
  return get(get(b:, 'espfiles', {}), 'dir', '') . (t ? '   [' . t . ' tagged]' : '')
endfunction

" Free space, worked out once per render: the status line is redrawn often.
function! espfiles#Space() abort
  return get(get(b:, 'espfiles', {}), 'space', '')
endfunction

function! s:SpaceOf(dir) abort
  if a:dir ==# '/'
    return ''
  endif
  let r = esp_fs_info(a:dir)
  if empty(r)
    return ''
  endif
  return r.readonly ? 'read-only' : s:Size(r.free) . ' free of ' . s:Size(r.total)
endfunction

function! s:Syntax() abort
  syntax clear
  syntax match EspFilesParent /^  \.\.\/.*/
  syntax match EspFilesDir /^  .\{-}\/\ze\s/
  syntax match EspFilesTagged /^\*.*/
  highlight default link EspFilesParent Comment
  highlight default link EspFilesDir Directory
  highlight default link EspFilesTagged Search
endfunction

function! s:Maps() abort
  let pairs = [
        \ [['<CR>', 'l', '<Right>', '<2-LeftMouse>'], 'Enter()'],
        \ [['<BS>', 'h', '-', '<Left>'], 'Parent()'],
        \ [['<Tab>'], 'OtherPane()'],
        \ [['<Space>', 't', '<Insert>'], 'Tag()'],
        \ [['u'], 'Untag()'],
        \ [['<F3>', 'v'], 'View()'],
        \ [['<F4>', 'e'], 'Edit()'],
        \ [['<F5>', 'c'], 'Copy(0)'],
        \ [['<F6>', 'r'], 'Copy(1)'],
        \ [['<F7>', 'm'], 'Mkdir()'],
        \ [['<F8>', 'd', '<Del>'], 'Delete()'],
        \ [['o'], 'SameDir()'],
        \ [['<C-L>', 'R'], 'RefreshAll()'],
        \ [['<F1>', 'g?'], 'Help()'],
        \ [['<F10>', 'q'], 'Quit()'],
        \ ]
  for [keys, fn] in pairs
    for k in keys
      execute 'nnoremap <buffer> <silent> ' . k . ' :<C-U>call <SID>' . fn . '<CR>'
    endfor
  endfor
endfunction

" -------------------------------------------------------------- render --

function! s:Size(n) abort
  if a:n < 1024
    return a:n . ' B'
  elseif a:n < 1024 * 1024
    return printf('%.1fK', a:n / 1024.0)
  endif
  return printf('%.1fM', a:n / 1048576.0)
endfunction

function! s:Join(dir, name) abort
  return (a:dir ==# '/' ? '' : a:dir) . '/' . a:name
endfunction

function! s:ParentOf(dir) abort
  return a:dir ==# '/' ? '/' : fnamemodify(a:dir, ':h')
endfunction

" Draw the pane; put the cursor on {select} if given, else keep its entry.
function! s:Render(select) abort
  let st = b:espfiles
  let keep = empty(a:select) ? get(s:Current(), 'name', '') : a:select
  let entries = esp_fs_list(st.dir)
  call sort(entries, {a, b -> a.type !=# b.type ? (a.type ==# 'dir' ? -1 : 1)
        \ : tolower(a.name) ==# tolower(b.name) ? 0 : tolower(a.name) < tolower(b.name) ? -1 : 1})
  let st.entries = entries
  let names = {}
  for e in entries
    let names[e.name] = 1
  endfor
  call filter(st.tags, 'has_key(names, v:key)')
  let st.space = s:SpaceOf(st.dir)

  let width = max([winwidth(0), 30])
  let dated = width >= 50
  let namew = width - 2 - 9 - (dated ? 17 : 0) - 1
  let lines = []
  if st.dir !=# '/'
    call add(lines, '  ../')
  endif
  for e in entries
    let name = e.name . (e.type ==# 'dir' ? '/' : '')
    if strchars(name) > namew
      let name = strcharpart(name, 0, namew - 1) . '~'
    endif
    let line = (has_key(st.tags, e.name) ? '* ' : '  ') . name
          \ . repeat(' ', namew - strdisplaywidth(name))
          \ . printf('%9s', e.type ==# 'dir' ? '<DIR>' : s:Size(e.size))
    if dated
      let line .= ' ' . (e.mtime > 0 ? strftime('%Y-%m-%d %H:%M', e.mtime) : repeat(' ', 16))
    endif
    call add(lines, line)
  endfor

  setlocal modifiable
  silent %delete _
  call setline(1, empty(lines) ? ['  (empty)'] : lines)
  setlocal nomodifiable nomodified
  let row = 1
  if !empty(keep)
    let idx = index(map(copy(entries), 'v:val.name'), keep)
    if idx >= 0
      let row = idx + 1 + s:Offset()
    endif
  endif
  call cursor(row, 1)
endfunction

" Lines before the first entry: the "../" line, except at "/".
function! s:Offset() abort
  return b:espfiles.dir ==# '/' ? 0 : 1
endfunction

" The entry under the cursor, or {} on "../" or an empty pane.
function! s:Current() abort
  if !exists('b:espfiles')
    return {}
  endif
  let i = line('.') - 1 - s:Offset()
  return i >= 0 && i < len(b:espfiles.entries) ? b:espfiles.entries[i] : {}
endfunction

" Tagged entries, or the current one: what an operation applies to.
function! s:Targets() abort
  let st = b:espfiles
  if !empty(st.tags)
    return filter(copy(st.entries), 'has_key(st.tags, v:val.name)')
  endif
  let e = s:Current()
  return empty(e) ? [] : [e]
endfunction

function! s:OtherWin() abort
  for w in range(1, winnr('$'))
    let st = getbufvar(winbufnr(w), 'espfiles', {})
    if !empty(st) && st.side !=# b:espfiles.side
      return w
    endif
  endfor
  return 0
endfunction

function! s:OtherDir() abort
  let w = s:OtherWin()
  return w ? getbufvar(winbufnr(w), 'espfiles').dir : b:espfiles.dir
endfunction

" Redraw both panes (they may show the same directory).
function! s:RefreshAll() abort
  let here = winnr()
  for w in range(1, winnr('$'))
    if !empty(getbufvar(winbufnr(w), 'espfiles', {}))
      execute w . 'wincmd w'
      call s:Render('')
    endif
  endfor
  execute here . 'wincmd w'
endfunction

" ------------------------------------------------------------- actions --

function! s:Go(dir, select) abort
  let old = b:espfiles.dir
  let b:espfiles.dir = a:dir
  let b:espfiles.tags = {}
  try
    call s:Render(a:select)
  catch
    let b:espfiles.dir = old
    call s:Render('')
    echohl ErrorMsg | echomsg v:exception | echohl None
  endtry
endfunction

function! s:Enter() abort
  if line('.') == 1 && s:Offset()
    return s:Parent()
  endif
  let e = s:Current()
  if empty(e)
    return
  elseif e.type ==# 'dir'
    call s:Go(s:Join(b:espfiles.dir, e.name), '')
  else
    call s:Edit()
  endif
endfunction

function! s:Parent() abort
  let dir = b:espfiles.dir
  if dir !=# '/'
    call s:Go(s:ParentOf(dir), fnamemodify(dir, ':t'))
  endif
endfunction

function! s:OtherPane() abort
  let w = s:OtherWin()
  if w
    execute w . 'wincmd w'
  endif
endfunction

function! s:SameDir() abort
  let w = s:OtherWin()
  let dir = b:espfiles.dir
  if w
    execute w . 'wincmd w'
    call s:Go(dir, '')
    wincmd p
  endif
endfunction

function! s:Tag() abort
  let e = s:Current()
  if !empty(e)
    if has_key(b:espfiles.tags, e.name)
      call remove(b:espfiles.tags, e.name)
    else
      let b:espfiles.tags[e.name] = 1
    endif
    call s:Render(e.name)
  endif
  normal! j
endfunction

function! s:Untag() abort
  let b:espfiles.tags = {}
  call s:Render('')
endfunction

function! s:Open(readonly) abort
  let e = s:Current()
  if empty(e)
    return
  elseif e.type ==# 'dir'
    return s:Enter()
  endif
  execute 'tabedit ' . fnameescape(s:Join(b:espfiles.dir, e.name))
  if a:readonly
    setlocal readonly nomodifiable
    nnoremap <buffer> <silent> q :tabclose<CR>
  endif
endfunction

function! s:View() abort
  call s:Open(1)
endfunction

function! s:Edit() abort
  call s:Open(0)
endfunction

" Copy ({move} = 0) or move/rename ({move} = 1) the targets.
function! s:Copy(move) abort
  let items = s:Targets()
  if empty(items)
    return
  endif
  let what = a:move ? 'Move' : 'Copy'
  let other = s:OtherDir()
  " One item into the pane's own directory is a rename: offer its name.
  let default = len(items) == 1 && other ==# b:espfiles.dir
        \ ? s:Join(other, items[0].name) : (other ==# '/' ? '/' : other . '/')
  let label = len(items) == 1 ? items[0].name : len(items) . ' items'
  call inputsave()
  let dest = input(what . ' ' . label . ' to: ', default, 'dir')
  call inputrestore()
  redraw
  if empty(dest)
    return
  endif
  let dest = fnamemodify(dest, ':p')
  let into = isdirectory(dest) || dest =~# '/$'
  if !into && len(items) > 1
    echohl ErrorMsg | echomsg what . ': ' . dest . ' is not a directory' | echohl None
    return
  endif
  let dest = substitute(dest, '/\+$', '', '')
  let all = 0
  let done = 0
  for e in items
    let src = s:Join(b:espfiles.dir, e.name)
    let dst = into ? s:Join(empty(dest) ? '/' : dest, e.name) : dest
    let overwrite = all
    if !all && (filereadable(dst) || isdirectory(dst))
      let c = confirm(dst . ' exists. Overwrite?', "&Yes\n&No\n&All\n&Cancel", 2)
      if c == 4 || c == 0
        break
      elseif c == 2
        continue
      endif
      let overwrite = 1
      let all = c == 3
    endif
    let ok = a:move ? esp_fs_move(src, dst, overwrite) : esp_fs_copy(src, dst, overwrite)
    if !ok
      break
    endif
    let done += 1
  endfor
  let b:espfiles.tags = {}
  call s:RefreshAll()
  echo (a:move ? 'Moved ' : 'Copied ') . done . ' of ' . len(items)
endfunction

function! s:Mkdir() abort
  call inputsave()
  let name = input('New directory: ')
  call inputrestore()
  redraw
  if !empty(name)
    let path = name =~# '^/' ? name : s:Join(b:espfiles.dir, name)
    if esp_fs_mkdir(path)
      call s:RefreshAll()
      call s:Render(fnamemodify(substitute(path, '/\+$', '', ''), ':t'))
    endif
  endif
endfunction

function! s:Delete() abort
  let items = s:Targets()
  if empty(items)
    return
  endif
  let label = len(items) == 1 ? items[0].name . (items[0].type ==# 'dir' ? '/ and everything in it' : '')
        \ : len(items) . ' items'
  if confirm('Delete ' . label . '?', "&Yes\n&No", 2) != 1
    return
  endif
  let done = 0
  for e in items
    if !esp_fs_delete(s:Join(b:espfiles.dir, e.name))
      break
    endif
    let done += 1
  endfor
  let b:espfiles.tags = {}
  call s:RefreshAll()
  echo 'Deleted ' . done . ' of ' . len(items)
endfunction

function! s:Help() abort
  echo 'EspFiles keys (":help :EspFiles" for more):'
  for [fkey, letter, desc] in s:keys
    echo printf('  %-8s %-3s %s', fkey, letter, desc)
  endfor
endfunction

function! s:Quit() abort
  if tabpagenr('$') > 1
    tabclose
  else
    only
    enew
  endif
endfunction
