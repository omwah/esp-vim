" :EspSd -- every build has it (plugin/esp.vim; vimrc-lowmem).

" --------------------------------------------------------------- :EspSd --

function! esp#sd#SdComplete(lead, line, pos) abort
  return filter(['mount', 'eject'], 'v:val =~# "^" . a:lead')
endfunction

" :EspSd [mount|eject]: the microSD card at /sd.
function! esp#sd#Sd(...) abort
  let what = a:0 ? a:1 : ''
  if what ==# 'mount'
    if esp_sd_mount()
      let c = esp_sd()
      echo 'SD card mounted at /sd: ' . c.fs . ', ' . esp#view#Size(c.free) . ' free of ' . esp#view#Size(c.total)
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
    call extend(rows, [['Card', c.name . '  ' . c.type . '  ' . esp#view#Size(c.card_bytes)],
          \ ['Filesystem', c.fs . '  ' . esp#view#Size(c.free) . ' free of ' . esp#view#Size(c.total)],
          \ ['Bus', c.bus . ' at ' . (c.khz / 1000) . ' MHz']])
  else
    call add(rows, ['Bus', c.bus])
    if !empty(c.error)
      call add(rows, ['Why', c.error])
    endif
  endif
  call esp#view#Show('Sd', ['SD card   (' . (c.mounted ? ':EspSd eject' : ':EspSd mount')
        \ . ', R refresh, q close)'] + map(rows, 'esp#view#Row("%-11s %s", v:val[0], v:val[1])'),
        \ 0, function('esp#sd#Sd'))
endfunction
