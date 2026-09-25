" :EspSerial, :EspSerialSend, :EspI2cScan, :EspSensors, :EspAdc.

" ---------------------------------------------------------------- serial --

" Line ending :EspSerialSend adds.
let g:esp_serial_eol = get(g:, 'esp_serial_eol', "\r\n")
let s:timers = {}         " port -> timer id
let s:last_port = 0

function! s:SerialBuf(port) abort
  return 'EspSerial-' . a:port
endfunction

" :EspSerial {port} {baud} [{tx} {rx}]
function! esp#hw#Serial(port, baud, ...) abort
  let port = str2nr(a:port)
  if !call('esp_serial_open', [port, str2nr(a:baud)] + map(copy(a:000), 'str2nr(v:val)'))
    return
  endif
  let name = s:SerialBuf(port)
  let w = bufwinnr('^' . name . '$')
  if w > 0
    execute w . 'wincmd w'
  else
    botright new
    setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted
    execute 'silent file ' . name
    let b:esp_serial_port = port
    nnoremap <buffer> <silent> q :call esp#hw#SerialClose(b:esp_serial_port)<CR>
    nnoremap <buffer> <silent> s :call esp#hw#SerialPrompt()<CR>
    call setline(1, '')
  endif
  let s:last_port = port
  if !has_key(s:timers, port)
    " Ten times a second: immediate to a person. (Each tick makes Vim hide
    " and show the cursor, a few bytes on a serial console; negligible.)
    let s:timers[port] = timer_start(100, function('s:SerialPoll', [port]), {'repeat': -1})
  endif
  echo 'UART' . port . ' at ' . a:baud . ' baud.  s: send a line   q: close'
endfunction

function! s:SerialPoll(port, timer) abort
  let text = esp_serial_read(a:port)
  if empty(text)
    return
  endif
  let b = bufnr(s:SerialBuf(a:port))
  if b < 0
    return
  endif
  let lines = split(substitute(text, "\r", '', 'g'), "\n", 1)
  let last = getbufline(b, '$')[0]
  call setbufline(b, '$', last . lines[0])
  if len(lines) > 1
    call appendbufline(b, '$', lines[1:])
  endif
  " Follow the output, in whichever window shows it.
  let w = bufwinid(b)
  if w != -1
    call win_execute(w, 'normal! G$')
  endif
endfunction

" :EspSerialSend {text}: to the port of the current serial buffer, else the
" last one opened.
function! esp#hw#SerialSend(text) abort
  let port = get(b:, 'esp_serial_port', s:last_port)
  if port == 0
    echohl ErrorMsg | echomsg 'EspSerialSend: no serial port is open (:EspSerial)' | echohl None
    return
  endif
  call esp_serial_write(port, a:text . g:esp_serial_eol)
endfunction

function! esp#hw#SerialPrompt() abort
  call inputsave()
  let t = input('send> ')
  call inputrestore()
  if !empty(t)
    call esp#hw#SerialSend(t)
  endif
endfunction

function! esp#hw#SerialClose(port) abort
  if has_key(s:timers, a:port)
    call timer_stop(remove(s:timers, a:port))
  endif
  call esp_serial_close(a:port)
  let b = bufnr(s:SerialBuf(a:port))
  if b > 0
    execute 'bwipe! ' . b
  endif
  echo 'UART' . a:port . ' closed'
endfunction

" ------------------------------------------------------------ i2c / adc --

function! s:Pins(args) abort
  return map(copy(a:args), 'str2nr(v:val)')
endfunction

" :EspI2cScan [{sda} {scl}]
function! esp#hw#I2cScan(...) abort
  let found = call('esp_i2c_scan', s:Pins(a:000))
  if empty(found)
    echo 'No I2C devices answered'
  else
    echo 'I2C devices: ' . join(map(copy(found), 'printf("0x%02x", v:val)'), ' ')
  endif
endfunction

" :EspSensors [{sda} {scl}]
function! esp#hw#Sensors(...) abort
  let found = call('esp_sensors', s:Pins(a:000))
  if empty(found)
    echo 'No I2C devices answered'
    return
  endif
  botright new
  setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted
  silent file EspSensors
  call setline(1, ['I2C devices   (q close)'] + map(copy(found), 'printf("0x%02x  %s", v:val.addr, v:val.name)'))
  setlocal nomodifiable
  nnoremap <buffer> <silent> q :close<CR>
  execute 'resize ' . (len(found) + 1)
endfunction

" :EspAdc {pin}
function! esp#hw#Adc(pin) abort
  let r = esp_adc_read(str2nr(a:pin))
  if !empty(r)
    echo printf('GPIO %d (ADC%d channel %d): raw %d%s', str2nr(a:pin), r.unit, r.channel, r.raw,
          \ r.mv >= 0 ? printf(', %d mV', r.mv) : ', not calibrated')
  endif
endfunction
