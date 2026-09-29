" The :EspGit* commands (plugin/esp.vim) over the esp_git_*() builtins
" (libgit2, esp-vim/components/vim/api/esp_api_git.c). See ":help esp-git".
"
" Every command works on the repository of the current file, or of the
" current directory when the buffer has no file; the git windows remember
" theirs.

" ------------------------------------------------------------- helpers --

function! s:Root() abort
  if exists('b:esp_git_root')
    return b:esp_git_root
  endif
  let file = expand('%:p')
  return empty(file) || !empty(&buftype) ? getcwd() : fnamemodify(file, ':h')
endfunction

function! s:Warn(text) abort
  echohl WarningMsg
  echomsg a:text
  echohl None
endfunction

" The error text of a failed builtin, without Vim's "Vim(call):" prefix.
function! s:Error(e) abort
  return substitute(a:e, '^Vim\%((\a\+)\)\=:', '', '')
endfunction

" Files named in a command: none means the current file.
function! s:Files(args) abort
  if empty(a:args)
    let file = expand('%:p')
    return empty(file) || !empty(&buftype) ? [] : [file]
  endif
  return map(copy(a:args), 'expand(v:val)')
endfunction

" Reload buffers whose files a checkout, pull or restore changed.
function! s:Reload() abort
  silent! checktime
endfunction

" The ssh://, scp-like or other URL of remote {name} in {root}.
function! s:RemoteUrl(root, name) abort
  for r in esp_git_remote(a:root)
    if r.name ==# a:name
      return r.url
    endif
  endfor
  return ''
endfunction

" An SSH git URL as esp_ssh names hosts (scp://user@host:port/).
function! s:SshUrl(url) abort
  let m = matchlist(a:url, '^ssh://\%(\([^@/]*\)@\)\=\([^/:]\+\)\%(:\(\d\+\)\)\=/')
  if !empty(m)
    return 'scp://' . (empty(m[1]) ? 'git' : m[1]) . '@' . m[2] . (empty(m[3]) ? '' : ':' . m[3]) . '/'
  endif
  let m = matchlist(a:url, '^\%(\([^@/:]*\)@\)\=\([^/:]\+\):')
  if !empty(m) && a:url !~# '^\a\+://'
    return 'scp://' . (empty(m[1]) ? 'git' : m[1]) . '@' . m[2] . '/'
  endif
  return ''
endfunction

" Call {Fn} with {args}; on an unknown SSH host key, ask, trust it and try
" again (as :EspFiles does). {Url} is a Funcref giving the remote's URL.
function! s:Remote(Fn, args, Url) abort
  while 1
    try
      return call(a:Fn, a:args)
    catch /unknown host key/
      let what = matchstr(v:exception, '(\zs[^)]*\ze)')
      let host = matchstr(v:exception, 'unknown host key for \zs\S\+')
      let ssh = s:SshUrl(a:Url())
      if empty(ssh)
        throw v:exception
      endif
      redraw
      if confirm("The authenticity of " . host . " can't be established.\n"
            \ . "Its key is " . what . ".\nTrust it and remember it?", "&Yes\n&No", 2) != 1
        throw 'EspGit: ' . host . ' not trusted'
      endif
      call esp_ssh_trust(ssh)
    endtry
  endwhile
endfunction

function! s:Try(Fn) abort
  try
    call a:Fn()
  catch /^Vim\%((\a\+)\)\=:Interrupt/
    call s:Warn('EspGit: interrupted')
  catch /^EspGit:/
    call s:Warn(v:exception)
  catch
    echohl ErrorMsg
    echomsg s:Error(v:exception)
    echohl None
  endtry
endfunction

function! s:ClockCheck() abort
  if exists('*esp_time') && !esp_time().valid
    call s:Warn('EspGit: the clock is not set, so this is dated 1970 (:EspTime set, or join a network)')
  endif
endfunction

function! s:AutoGc(root) abort
  let g = esp_git_gc(a:root, 1000)
  if !get(g, 'skipped', 1)
    echomsg printf('EspGit: packed %d objects (%d loose ones removed)', g.packed, g.loose_removed)
  endif
endfunction

" ---------------------------------------------------------- commands --

function! esp#git#Init(...) abort
  call s:Try({-> s:Init(a:0 ? a:1 : getcwd())})
endfunction

function! s:Init(dir) abort
  let root = esp_git_init(fnamemodify(expand(a:dir), ':p'))
  if !empty(root)
    echo 'Initialized an empty git repository in ' . root
  endif
endfunction

" :EspGitStatus: a window of the changes. a add, u unstage, x discard,
" d diff, <CR> open, c commit, R refresh, q close.
function! esp#git#Status() abort
  call s:Try({-> s:Status(s:Root())})
endfunction

function! s:Status(dir) abort
  let st = esp_git_status(a:dir)
  if empty(st)
    return
  endif
  let where = empty(st.branch) ? 'HEAD detached at ' . st.head[:6] : 'on ' . st.branch
  if !empty(st.upstream)
    let where .= ', tracking ' . st.upstream
    if st.ahead || st.behind
      let where .= printf(' (%d ahead, %d behind)', st.ahead, st.behind)
    endif
  endif
  let lines = [st.root . ' -- ' . where . '   (a add, u unstage, x discard, d diff, c commit, R, q)']
  if !empty(st.state)
    call add(lines, 'In the middle of a ' . st.state . ': fix the U files, add them, then commit.')
  endif
  if empty(st.head)
    call add(lines, 'No commits yet.')
  endif
  let first = len(lines) + 1
  let files = []
  for f in st.files
    call add(lines, f.x . f.y . ' ' . f.path)
    call add(files, f.path)
  endfor
  if empty(files)
    call add(lines, 'Nothing to commit: the working tree matches HEAD.')
  endif
  call esp#View('GitStatus', lines, 0, function('esp#git#Refresh'))
  let b:esp_git_root = st.root
  let b:esp_git_files = files
  let b:esp_git_first = first
  syntax clear EspViewLabel
  syntax match EspGitStaged /^[MADRT]/ contained
  syntax match EspGitChanged /^.\zs[MDT?]/ contained
  syntax match EspGitConflict /^UU/ contained
  syntax match EspGitEntry /^[ MADRTU?][ MDTRU?] .*/ contains=EspGitStaged,EspGitChanged,EspGitConflict
  highlight default link EspGitStaged DiffAdd
  highlight default link EspGitChanged DiffChange
  highlight default link EspGitConflict ErrorMsg
  nnoremap <buffer> <silent> a :call esp#git#StatusDo('add')<CR>
  nnoremap <buffer> <silent> u :call esp#git#StatusDo('unstage')<CR>
  nnoremap <buffer> <silent> x :call esp#git#StatusDo('discard')<CR>
  nnoremap <buffer> <silent> d :call esp#git#StatusDo('diff')<CR>
  nnoremap <buffer> <silent> <CR> :call esp#git#StatusDo('open')<CR>
  nnoremap <buffer> <silent> c :EspGitCommit<CR>
  if !empty(files)
    execute 'normal! ' . first . 'G'
  endif
endfunction

function! esp#git#Refresh() abort
  call s:Try({-> s:Status(b:esp_git_root)})
endfunction

function! esp#git#StatusDo(what) abort
  let i = line('.') - b:esp_git_first
  if i < 0 || i >= len(b:esp_git_files)
    return
  endif
  let root = b:esp_git_root
  let file = root . '/' . b:esp_git_files[i]
  let code = getline('.')[:1]
  if a:what ==# 'open'
    wincmd p
    execute 'edit ' . fnameescape(file)
    return
  elseif a:what ==# 'diff'
    call s:Try({-> s:Diff(root, code[0] !=# ' ' && code[1] ==# ' ', [file])})
    return
  elseif a:what ==# 'discard'
    if code ==# '??'
      call s:Warn('EspGit: ' . b:esp_git_files[i] . ' is untracked: delete it yourself if you mean to')
      return
    endif
    if confirm('Throw away the changes to ' . b:esp_git_files[i] . '?', "&Yes\n&No", 2) != 1
      return
    endif
    call s:Try({-> esp_git_restore(root, [file])})
    call s:Reload()
  elseif a:what ==# 'add'
    call s:Try({-> esp_git_add(root, [file])})
  else
    call s:Try({-> esp_git_reset(root, [file])})
  endif
  let l = line('.')
  call s:Try({-> s:Status(root)})
  execute 'normal! ' . min([l, line('$')]) . 'G'
endfunction

function! esp#git#Add(...) abort
  let files = a:0 == 1 && a:1 ==# '.' ? [] : s:Files(a:000)
  if a:0 == 0 && empty(files)
    call s:Warn('EspGitAdd: which files? (". " adds every change)')
    return
  endif
  call s:Try({-> esp_git_add(s:Root(), files) && s:Echo('Staged ' . (empty(files) ? 'every change' : join(map(copy(files), 'fnamemodify(v:val, ":~:.")'))))})
endfunction

function! s:Echo(text) abort
  echo a:text
  return 1
endfunction

function! esp#git#Reset(...) abort
  let files = s:Files(a:000)
  call s:Try({-> esp_git_reset(s:Root(), files) && s:Echo('Unstaged ' . join(map(copy(files), 'fnamemodify(v:val, ":~:.")')))})
endfunction

function! esp#git#Restore(bang, ...) abort
  let files = s:Files(a:000)
  if empty(files)
    call s:Warn('EspGitRestore: which files?')
    return
  endif
  if !a:bang && confirm('Throw away the changes to ' . join(map(copy(files), 'fnamemodify(v:val, ":~:.")')) . '?', "&Yes\n&No", 2) != 1
    return
  endif
  call s:Try({-> esp_git_restore(s:Root(), files)})
  call s:Reload()
endfunction

" :EspGitCommit [message]: commit what is staged. Without a message, a
" buffer to write it in: :w commits, :q! gives up. With !, stage every
" change first (like git add -A; git commit).
function! esp#git#Commit(bang, message) abort
  let root = s:Root()
  if a:bang
    call s:Try({-> esp_git_add(root, [])})
  endif
  if !empty(a:message)
    call s:Try({-> s:DoCommit(root, a:message)})
    return
  endif
  let st = {}
  try
    let st = esp_git_status(root)
  catch
    echohl ErrorMsg | echomsg s:Error(v:exception) | echohl None
    return
  endtry
  let staged = filter(copy(st.files), 'v:val.x !~# "[ ?U]"')
  if empty(staged) && st.state !=# 'merge'
    call s:Warn('EspGitCommit: nothing is staged (:EspGitAdd, or :EspGitCommit! for everything)')
    return
  endif
  botright new
  setlocal buftype=acwrite bufhidden=wipe noswapfile nobuflisted
  silent file EspGitCommit
  let b:esp_git_root = st.root
  let lines = ['', '# The commit message goes above. Lines starting with # are left out.',
        \ '# :w commits, :q! gives up.', '#',
        \ '# On branch ' . (empty(st.branch) ? '(detached)' : st.branch)]
  if st.state ==# 'merge'
    call add(lines, '# Finishing a merge.')
  endif
  call add(lines, '# To be committed:')
  for f in staged
    call add(lines, '#   ' . f.x . ' ' . f.path)
  endfor
  call setline(1, lines)
  setlocal nomodified
  setlocal filetype=gitcommit
  syntax match EspGitComment /^#.*/
  highlight default link EspGitComment Comment
  autocmd BufWriteCmd <buffer> call esp#git#CommitWrite()
  normal! gg
  startinsert
endfunction

function! esp#git#CommitWrite() abort
  let root = b:esp_git_root
  let text = join(getline(1, '$'), "\n") . "\n"
  try
    call s:DoCommit(root, text)
    setlocal nomodified
    close
  catch
    echohl ErrorMsg | echomsg s:Error(v:exception) | echohl None
  endtry
endfunction

function! s:DoCommit(root, message) abort
  call s:ClockCheck()
  let id = esp_git_commit(a:root, a:message)
  if !empty(id)
    let summary = get(esp_git_log(a:root, 1), 0, {'summary': ''}).summary
    echo 'Committed ' . id[:6] . ' ' . summary
    call s:AutoGc(a:root)
  endif
endfunction

" :EspGitLog [max]: the history. <CR> shows a commit, R refreshes, q closes.
function! esp#git#Log(...) abort
  let max = a:0 ? a:1 : 100
  call s:Try({-> s:Log(s:Root(), max)})
endfunction

function! s:Log(dir, max) abort
  let root = esp_git_status(a:dir).root
  let log = esp_git_log(a:dir, a:max)
  let rows = []
  for c in log
    call add(rows, [c.id[:6], strftime('%Y-%m-%d %H:%M', c.time), c.author,
          \ (c.parents > 1 ? '(merge) ' : '') . c.summary])
  endfor
  let lines = [root . ' -- history   (<CR> show, R refresh, q close)']
  call extend(lines, empty(rows) ? ['No commits yet.'] : esp#Table('GitLog', rows, 'llll'))
  call esp#View('GitLog', lines, 0, function('esp#git#LogRefresh'))
  let b:esp_git_root = root
  let b:esp_git_max = a:max
  let b:esp_git_ids = map(copy(log), 'v:val.id')
  syntax clear EspViewLabel
  syntax match EspGitId /^\x\{7}/
  highlight default link EspGitId Identifier
  nnoremap <buffer> <silent> <CR> :call esp#git#LogShow()<CR>
endfunction

function! esp#git#LogRefresh() abort
  call s:Try({-> s:Log(b:esp_git_root, b:esp_git_max)})
endfunction

function! esp#git#LogShow() abort
  let i = line('.') - 2
  if i >= 0 && i < len(b:esp_git_ids)
    let root = b:esp_git_root
    let id = b:esp_git_ids[i]
    call s:Try({-> s:Scratch('EspGitShow', esp_git_show(root, id), root)})
  endif
endfunction

" A read-only diff buffer called {name}, reused.
function! s:Scratch(name, lines, root) abort
  let win = bufwinnr('^' . a:name . '$')
  if win > 0
    execute win . 'wincmd w'
  else
    botright new
    setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted
    execute 'silent file ' . a:name
    nnoremap <buffer> <silent> q :close<CR>
  endif
  let b:esp_git_root = a:root
  setlocal modifiable
  silent %delete _
  call setline(1, empty(a:lines) ? ['No differences.'] : a:lines)
  setlocal nomodifiable nomodified filetype=diff
  normal! gg
endfunction

function! esp#git#Show(...) abort
  let rev = a:0 ? a:1 : 'HEAD'
  let root = s:Root()
  call s:Try({-> s:Scratch('EspGitShow', esp_git_show(root, rev), root)})
endfunction

" :EspGitDiff [--cached] [files]: the working tree against the index (or,
" --cached, the index against HEAD).
function! esp#git#Diff(...) abort
  let args = copy(a:000)
  let cached = index(args, '--cached') >= 0 || index(args, '--staged') >= 0
  call filter(args, 'v:val !~# "^--"')
  let files = empty(args) ? [] : map(args, 'expand(v:val)')
  call s:Try({-> s:Diff(s:Root(), cached, files)})
endfunction

function! s:Diff(root, cached, files) abort
  let opts = {'cached': a:cached}
  if !empty(a:files)
    let opts.files = a:files
  endif
  call s:Scratch('EspGitDiff', esp_git_diff(a:root, opts), a:root)
endfunction

" :EspGitBranch: list; {name}: make one at HEAD; -d {name}: delete one.
function! esp#git#Branch(...) abort
  let root = s:Root()
  if a:0 == 0
    call s:Try({-> s:Branches(root)})
  elseif a:1 ==# '-d' && a:0 == 2
    call s:Try({-> esp_git_branch(root, a:2, {'delete': 1}) && s:Echo('Deleted branch ' . a:2)})
  else
    call s:Try({-> esp_git_branch(root, a:1) && s:Echo('Made branch ' . a:1 . ' (:EspGitCheckout ' . a:1 . ' to switch)')})
  endif
endfunction

function! s:Branches(root) abort
  let lines = []
  for b in esp_git_branch(a:root)
    call add(lines, (b.current ? '* ' : '  ') . (b.remote ? 'remotes/' : '') . b.name
          \ . (empty(b.upstream) ? '' : '  -> ' . b.upstream))
  endfor
  echo empty(lines) ? 'No branches yet (no commits)' : join(lines, "\n")
endfunction

function! esp#git#BranchComplete(lead, line, pos) abort
  try
    let names = map(esp_git_branch(s:Root()), 'v:val.name')
  catch
    return []
  endtry
  return filter(names, 'stridx(v:val, a:lead) == 0')
endfunction

function! esp#git#Checkout(ref) abort
  let root = s:Root()
  call s:Try({-> esp_git_checkout(root, a:ref) && s:Echo('Switched to ' . a:ref)})
  call s:Reload()
endfunction

" :EspGitClone {url} [dir]: into {dir}, or a directory named after the
" repository in the current one.
function! esp#git#Clone(url, ...) abort
  let dir = a:0 ? expand(a:1) : substitute(fnamemodify(substitute(a:url, '/\+$', '', ''), ':t'), '\.git$', '', '')
  let dir = fnamemodify(dir, ':p')
  let url = a:url
  call s:Try({-> s:Clone(url, dir)})
endfunction

function! s:Clone(url, dir) abort
  let root = s:Remote(function('esp_git_clone'), [a:url, a:dir], {-> a:url})
  if !empty(root)
    redraw
    echo 'Cloned ' . a:url . ' into ' . root
  endif
endfunction

function! s:RemoteOf(root, args) abort
  return {-> s:RemoteUrl(a:root, empty(a:args) ? 'origin' : a:args[0])}
endfunction

function! esp#git#Fetch(...) abort
  let root = s:Root()
  let args = copy(a:000)
  call s:Try({-> s:Fetched(s:Remote(function('esp_git_fetch'), [root] + args, s:RemoteOf(root, args)))})
endfunction

function! s:Fetched(r) abort
  if !empty(a:r)
    redraw
    echo printf('Fetched from %s: %d objects, %d KB', a:r.remote, a:r.objects, a:r.bytes / 1024)
  endif
endfunction

function! esp#git#Pull(...) abort
  let root = s:Root()
  let args = copy(a:000)
  call s:Try({-> s:Pulled(root, s:Remote(function('esp_git_pull'), [root] + args, s:RemoteOf(root, args)))})
endfunction

function! s:Pulled(root, r) abort
  if empty(a:r)
    return
  endif
  call s:Reload()
  redraw
  if a:r.result ==# 'conflicts'
    call s:Warn('EspGitPull: conflicts in ' . join(a:r.conflicts, ', ')
          \ . ': fix them, :EspGitAdd them, then :EspGitCommit')
    call s:Status(a:root)
  else
    echo 'Pulled: ' . a:r.result . (a:r.result ==# 'up to date' ? '' : ', now at ' . a:r.head[:6])
  endif
endfunction

function! esp#git#Push(...) abort
  let root = s:Root()
  let args = copy(a:000)
  call s:Try({-> s:Pushed(s:Remote(function('esp_git_push'), [root] + args, s:RemoteOf(root, args)))})
endfunction

function! s:Pushed(r) abort
  if !empty(a:r)
    redraw
    echo 'Pushed ' . a:r.branch . ' to ' . a:r.remote
  endif
endfunction

" :EspGitRemote: list; {name} {url}: add or change one; -d {name}: remove it.
function! esp#git#Remote(...) abort
  let root = s:Root()
  if a:0 == 0
    call s:Try({-> s:Echo(join(map(esp_git_remote(root), 'v:val.name . "  " . v:val.url'), "\n"))})
  elseif a:1 ==# '-d' && a:0 == 2
    call s:Try({-> esp_git_remote(root, a:2, '') && s:Echo('Removed remote ' . a:2)})
  elseif a:0 == 2
    call s:Try({-> esp_git_remote(root, a:1, a:2) && s:Echo(a:1 . ' is ' . a:2)})
  else
    call s:Warn('EspGitRemote: {name} {url}, or -d {name}')
  endif
endfunction

" :EspGitConfig [--global] {key} [value]: show or set a setting; --global is
" the device-wide /fat/.gitconfig.
function! esp#git#Config(...) abort
  let args = copy(a:000)
  let global = get(args, 0, '') ==# '--global'
  if global
    call remove(args, 0)
  endif
  if empty(args)
    call s:Warn('EspGitConfig: [--global] {key} [value]')
    return
  endif
  let where = global ? '' : s:Root()
  let key = args[0]
  if len(args) == 1
    call s:Try({-> s:Echo(key . ' = ' . esp_git_config(where, key))})
  else
    let value = substitute(join(args[1:]), '^\([''"]\)\(.*\)\1$', '\2', '')
    call s:Try({-> s:Echo(key . ' = ' . esp_git_config(where, key, value))})
  endif
endfunction

" :EspGitCredential {user}: the user and token for HTTPS remotes, asked
" for and kept in NVS. :EspGitCredential! forgets them.
function! esp#git#Credential(bang, ...) abort
  if a:bang
    silent! call esp_nvs_erase('esp_git', 'token')
    silent! call esp_nvs_erase('esp_git', 'user')
    echo 'EspGit: HTTPS user and token forgotten'
    return
  endif
  if a:0 == 0
    let user = esp_nvs_get('esp_git', 'user', '')
    echo empty(user) ? 'No HTTPS user set (:EspGitCredential {user})' : 'HTTPS user: ' . user
    return
  endif
  call inputsave()
  let token = inputsecret('Token (or password) for ' . a:1 . ': ')
  call inputrestore()
  if empty(token)
    call s:Warn('EspGitCredential: no token given; nothing changed')
    return
  endif
  call esp_nvs_set('esp_git', 'user', a:1)
  call esp_nvs_set('esp_git', 'token', token)
  redraw
  echo 'EspGit: HTTPS user and token saved'
endfunction

function! esp#git#Gc() abort
  let root = s:Root()
  call s:Try({-> s:Gc(root)})
endfunction

function! s:Gc(root) abort
  let g = esp_git_gc(a:root)
  echo printf('Packed %d objects; removed %d loose objects and %d old packs',
        \ g.packed, g.loose_removed, g.packs_removed)
endfunction
