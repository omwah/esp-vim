" :EspBattery, :EspPower -- every build has them (plugin/esp.vim; vimrc-lowmem).

" ------------------------------------------------- :EspBattery, :EspPower --

" :EspBattery: charge, voltage, and whether the USB port powers the board.
function! esp#power#Battery() abort
  let p = esp_power()
  let lines = ['Battery   (R refresh, q close)']
  if p.battery_mv < 0
    call add(lines, esp#view#Row('%-8s %s', 'Battery', 'no reading'))
  elseif p.battery_pct < 0
    call add(lines, esp#view#Row('%-8s %s', 'Battery', 'none fitted'))
  else
    call extend(lines, [
          \ esp#view#Row('%-8s %d%%', 'Charge', p.battery_pct),
          \ esp#view#Row('%-8s %d.%02d V', 'Voltage', p.battery_mv / 1000, p.battery_mv % 1000 / 10)])
  endif
  call add(lines, esp#view#Row('%-8s %s', 'USB', p.source ==# 'usb' ? 'powering the board'
        \ : 'not detected (on some boards a charger can''t be seen)'))
  call esp#view#Show('Battery', lines, 0, function('esp#power#Battery'))
endfunction

" :EspPower                   the battery, and when the board sleeps
" :EspPower idle {min}|off    reader mode after {min} minutes idle, on battery
" :EspPower deep {min}|off    deep sleep after {min} minutes in reader mode
function! esp#power#PowerComplete(lead, line, pos) abort
  return filter(['idle', 'deep'], 'v:val =~# "^" . a:lead')
endfunction

function! esp#power#Power(...) abort
  if a:0
    if a:0 != 2 || index(['idle', 'deep'], a:1) < 0 || a:2 !~# '^\(\d\+\|off\)$'
      echoerr 'Usage: :EspPower [idle|deep {minutes}|off]'
      return
    endif
    call esp_power({a:1 . '_min': a:2 ==# 'off' ? 0 : str2nr(a:2)})
  endif
  let p = esp_power()
  let bat = p.battery_mv < 0 ? 'none measured'
        \ : p.battery_pct < 0 ? 'none fitted'
        \ : printf('%d.%02d V, about %d%%', p.battery_mv / 1000, p.battery_mv % 1000 / 10, p.battery_pct)
  let lines = ['Power   (R refresh, q close)',
        \ esp#view#Row('%-8s %s', 'Source', p.source ==# 'usb' ? 'USB'
        \           : p.source ==# 'battery' ? 'battery' : 'unknown'),
        \ esp#view#Row('%-8s %s', 'Battery', bat),
        \ esp#view#Row('%-8s %s', 'Reader', !p.reader ? 'no wake button: :EspSleep is a deep sleep'
        \           : p.idle_min ? printf('after %d min without a key, on battery', p.idle_min)
        \           : 'only on the wake button or :EspSleep'),
        \ esp#view#Row('%-8s %s', 'Deep', p.reader && p.deep_min
        \           ? printf('after %d min in reader mode, if nothing is unsaved', p.deep_min)
        \           : 'only with :EspSleep' . (p.reader ? ' deep' : '')),
        \ esp#view#Row('%-8s %s', 'Idle', printf('%d s since the last key', p.idle_s)),
        \ esp#view#Row('%-8s %s', 'Sleeps', printf('%d reader, %d deep%s', p.sleeps, p.deep_sleeps,
        \           empty(p.last_wake) ? '' : '; last woken by ' . p.last_wake)),
        \ ]
  call esp#view#Show('Power', lines, 0, function('esp#power#Power'))
endfunction
