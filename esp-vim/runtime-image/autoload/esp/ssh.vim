" SCP/SFTP for Vim script: the esp_ssh_*() builtins, plus the conversation
" they need -- trusting a new host, asking for a password. Used by netrw
" (patch 0010) and :EspFiles' remote panes.
"
" A password you type is remembered for the rest of this Vim session (never
" written anywhere), so a file manager listing does not ask on every refresh.

let s:passwords = {}

function! s:Host(url) abort
  return matchstr(a:url, '^\a\+://\zs[^/]*')
endfunction

" Call {Fn} with {args} plus an options Dict, handling an unknown host key
" (ask, then trust) and failed authentication (ask for a password, retry).
" Throws "EspSsh: ..." when the user declines.
function! esp#ssh#Call(Fn, args, url) abort
  let host = s:Host(a:url)
  let opts = has_key(s:passwords, host) ? {'password': s:passwords[host]} : {}
  let tries = 0
  while 1
    try
      return call(a:Fn, a:args + [opts])
    catch /unknown host key/
      let what = matchstr(v:exception, '(\zs[^)]*\ze)')
      if confirm("The authenticity of " . host . " can't be established.\n"
            \ . "Its key is " . what . ".\nTrust it and remember it?", "&Yes\n&No", 2) != 1
        throw 'EspSsh: ' . host . ' not trusted'
      endif
      call esp_ssh_trust(a:url)
    catch /authentication failed/
      let tries += 1
      if tries > 3
        throw 'EspSsh: authentication failed for ' . host
      endif
      call inputsave()
      let pw = inputsecret('Password for ' . host . ': ')
      call inputrestore()
      if empty(pw)
        throw 'EspSsh: no password given for ' . host
      endif
      let opts = {'password': pw}
      let s:passwords[host] = pw
    endtry
  endwhile
endfunction

function! esp#ssh#Get(url, file) abort
  return esp#ssh#Call(function('esp_ssh_get'), [a:url, a:file], a:url)
endfunction

function! esp#ssh#Put(file, url) abort
  return esp#ssh#Call(function('esp_ssh_put'), [a:file, a:url], a:url)
endfunction

function! esp#ssh#List(url) abort
  return esp#ssh#Call(function('esp_ssh_list'), [a:url], a:url)
endfunction

function! esp#ssh#Mkdir(url) abort
  return esp#ssh#Call(function('esp_ssh_mkdir'), [a:url], a:url)
endfunction

function! esp#ssh#Remove(url) abort
  return esp#ssh#Call(function('esp_ssh_remove'), [a:url], a:url)
endfunction

function! esp#ssh#Rename(url, newpath) abort
  return esp#ssh#Call(function('esp_ssh_rename'), [a:url, a:newpath], a:url)
endfunction

" The remote path part of a URL, as esp_ssh_rename() takes it.
function! esp#ssh#Path(url) abort
  return matchstr(a:url, '^\a\+://[^/]*/\zs.*')
endfunction

" :EspSshKeygen[!]: make the device's SSH key and show the public half, to
" paste into a server's ~/.ssh/authorized_keys.
function! esp#ssh#Keygen(bang) abort
  let key = '/fat/.ssh/id_ecdsa'
  if filereadable(key) && !a:bang
    echohl WarningMsg
    echomsg 'EspSshKeygen: ' . key . ' already exists; showing its public key (add ! to replace it)'
    echohl None
    let pub = join(readfile(key . '.pub'))
  else
    echo 'Generating an ECDSA P-256 key...'
    redraw
    let pub = esp_ssh_keygen(key, 'esp-vim@' . tolower(esp_info().chip))
    if empty(pub)
      return
    endif
  endif
  botright new
  setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted wrap
  silent file EspSshKey
  call setline(1, ['Public key: add this line to ~/.ssh/authorized_keys on the server', '', pub])
  setlocal nomodifiable
  nnoremap <buffer> <silent> q :close<CR>
  execute 'resize ' . (3 + len(pub) / max([winwidth(0), 1]) + 1)
endfunction
