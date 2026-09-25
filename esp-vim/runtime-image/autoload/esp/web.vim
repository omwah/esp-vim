" The web interface: :EspWebStart, :EspWebStop, :EspWebStatus, :EspWebPasswd.
"
" While the server runs, a once-a-second timer (on Vim's own main loop, so
" nothing touches Vim from the server's task) does two things: applies
" settings the browser saved, and publishes what is being edited.

let s:timer = -1
let s:last = ''

function! s:Urls(port) abort
  let n = esp_net_status()
  let host = n.up ? n.ip : '<device address>'
  return 'https://' . host . (a:port == 443 ? '' : ':' . a:port) . '/'
endfunction

function! esp#web#Start(...) abort
  let port = a:0 ? str2nr(a:1) : 443
  if !esp_web_info().password_set
    echohl ErrorMsg
    echomsg 'EspWebStart: set a password first with :EspWebPasswd'
    echohl None
    return
  endif
  echo 'Starting the web server (the first start makes its certificate)...'
  redraw
  let i = esp_web_start(port)
  if empty(i)
    return
  endif
  if s:timer < 0
    let s:timer = timer_start(1000, function('s:Tick'), {'repeat': -1})
  endif
  let s:last = ''
  call s:Publish()
  redraw
  echo 'Web interface: ' . s:Urls(i.port)
  echo 'Certificate fingerprint (SHA-256), to check against the browser:'
  echo '  ' . i.fingerprint
endfunction

function! esp#web#Stop() abort
  call esp_web_stop()
  if s:timer >= 0
    call timer_stop(s:timer)
    let s:timer = -1
  endif
  echo 'Web interface stopped'
endfunction

function! esp#web#Status() abort
  let i = esp_web_info()
  echo 'Web interface: ' . (i.running ? 'running at ' . s:Urls(i.port) : 'stopped')
  echo 'Password:      ' . (i.password_set ? 'set' : 'not set (:EspWebPasswd)')
  if i.running
    echo 'Logged in:     ' . i.sessions . ' browser(s)'
  endif
  if !empty(i.fingerprint)
    echo 'Certificate:   ' . i.fingerprint
  endif
endfunction

function! esp#web#Passwd() abort
  call inputsave()
  let a = inputsecret('New web password: ')
  let b = inputsecret('Again: ')
  call inputrestore()
  redraw
  if a !=# b
    echohl ErrorMsg | echomsg 'EspWebPasswd: the passwords do not match' | echohl None
  elseif esp_web_passwd(a)
    echo 'Web password set; browsers must log in again'
  endif
endfunction

" Settings saved in the browser (or at power-on): apply them.
function! esp#web#ApplySettings() abort
  let s = esp_settings()
  execute 'set tabstop=' . s.tabstop . ' shiftwidth=' . s.shiftwidth
  let &expandtab = s.expandtab
  let &number = s.number
  let &relativenumber = s.relativenumber
  let &wrap = s.wrap
  if !empty(s.background)
    let &background = s.background
  endif
  if !empty(s.colorscheme)
    execute 'silent! colorscheme ' . s.colorscheme
  endif
endfunction

function! s:Publish() abort
  let key = bufnr() . ':' . b:changedtick . ':' . line('.') . ':' . col('.') . ':' . mode()
  if key ==# s:last
    return
  endif
  let s:last = key
  let wc = wordcount()
  call esp_web_publish({
        \ 'file': expand('%:p'), 'filetype': &filetype, 'mode': mode(),
        \ 'line': line('.'), 'col': col('.'), 'lines': line('$'),
        \ 'words': wc.words, 'chars': wc.chars, 'bytes': wc.bytes,
        \ 'modified': &modified, 'buffers': len(getbufinfo({'buflisted': 1}))})
endfunction

function! s:Tick(timer) abort
  if esp_web_settings_changed()
    call esp#web#ApplySettings()
    redraw
  endif
  call s:Publish()
endfunction
