" :EspConsole, :EspFlip, :EspTouch, :EspFont -- the display (plugin/esp.vim;
" vimrc-lowmem).

" ----------------------------------------------------------------- :EspConsole --

" :EspConsole [on|off]: whether Vim's output also goes to the serial console,
" on boards where a display shows it.
function! esp#disp#ConsoleComplete(lead, line, pos) abort
  return filter(['on', 'off'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#disp#Console(...) abort
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
function! esp#disp#Flip(...) abort
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

" ----------------------------------------------------------------- :EspTouch --

" :EspTouch [flipx|flipy|swap|reset]: where taps land. flipx and flipy mirror
" them left-right and top-bottom on the screen, swap exchanges the screen's X
" and Y, reset goes back to the board's own; each is kept for the next start.
" Without an argument, shows the panel and its turn.
function! esp#disp#TouchComplete(lead, line, pos) abort
  return filter(['flipx', 'flipy', 'swap', 'reset'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#disp#Touch(...) abort
  let t = esp_touch()
  if !t.available
    echoerr 'EspTouch: no touch panel'
    return
  endif
  if a:0
    if a:1 ==# 'flipx'
      let t = esp_touch({'mirror_x': !t.mirror_x})
    elseif a:1 ==# 'flipy'
      let t = esp_touch({'mirror_y': !t.mirror_y})
    elseif a:1 ==# 'swap'
      let t = esp_touch({'swap': !t.swap})
    elseif a:1 ==# 'reset'
      let t = esp_touch({'reset': 1})
    else
      echoerr 'Usage: :EspTouch [flipx|flipy|swap|reset]'
      return
    endif
  endif
  let chip = t.chip . (t.addr >= 0 ? printf(' at 0x%02x', t.addr) : '')
  if t.switch1 >= 0
    let chip .= printf(', config 0x%02x, switch 0x%02x', t.config_version, t.switch1)
  endif
  echo 'Touch: ' . chip
  echo printf('Turn: swap %s, mirror x %s, mirror y %s (%s)',
        \ t.swap ? 'on' : 'off', t.mirror_x ? 'on' : 'off', t.mirror_y ? 'on' : 'off',
        \ t.saved ? 'set by :EspTouch' : "the board's")
endfunction

" ------------------------------------------------------------------ :EspFont --

" :EspFont [{size}]: the display's font, named for the screen size it gives
" ("80x24"). Without an argument, lists them; a font's own name
" ("terminus-10x20") picks between two that give the same size.
function! esp#disp#FontComplete(lead, line, pos) abort
  let sizes = []
  for f in esp_display().fonts
    if index(sizes, f.size) < 0
      call add(sizes, f.size)
    endif
  endfor
  return filter(sizes, 'v:val =~# "^" . a:lead')
endfunction

function! esp#disp#Font(...) abort
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
