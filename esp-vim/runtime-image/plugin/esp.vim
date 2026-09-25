" The :Esp* commands: the device's hardware and system, from inside Vim.
"
" Only the command definitions live here, so startup stays cheap; the work is
" in autoload/esp.vim, loaded on first use. The data comes from the esp_*()
" builtins (C, esp-vim/components/vim/api/). See ":help esp-commands".

if exists('g:loaded_esp') || !exists('*esp_info')
  finish
endif
let g:loaded_esp = 1

command! -bar EspInfo  call esp#Info()
command! -bar EspHeap  call esp#Heap()
command! -bar EspTasks call esp#Tasks()
command! -bar -bang EspReboot call esp#Reboot(<bang>0)
command! -bar -nargs=+ -complete=customlist,esp#GpioComplete EspGpio call esp#Gpio(<f-args>)
command! -bar -bang -nargs=* -complete=customlist,esp#NvsComplete EspNvs call esp#Nvs(<bang>0, <f-args>)
command! -bar -nargs=* -complete=dir EspFiles call espfiles#Open(<f-args>)
command! -bar EspNet call esp#Net()
command! -bar -bang -nargs=+ EspGet call esp#Get(<bang>0, <f-args>)
command! -bar -bang EspSshKeygen call esp#ssh#Keygen(<bang>0)
command! -bar -nargs=? EspWebStart call esp#web#Start(<f-args>)
command! -bar EspWebStop call esp#web#Stop()
command! -bar EspWebStatus call esp#web#Status()
command! -bar EspWebPasswd call esp#web#Passwd()

" Editor settings saved from the web interface persist in NVS; apply them at
" startup. (Checked with a builtin, so the autoload file only loads if needed.)
let s:set = esp_settings()
if s:set.tabstop != 8 || s:set.shiftwidth != 8 || s:set.expandtab || s:set.number
      \ || s:set.relativenumber || !s:set.wrap || !empty(s:set.colorscheme) || !empty(s:set.background)
  autocmd VimEnter * ++once call esp#web#ApplySettings()
endif
unlet s:set
command! -bar -nargs=+ EspSerial call esp#hw#Serial(<f-args>)
command! -nargs=+ EspSerialSend call esp#hw#SerialSend(<q-args>)
command! -bar -nargs=* EspI2cScan call esp#hw#I2cScan(<f-args>)
command! -bar -nargs=* EspSensors call esp#hw#Sensors(<f-args>)
command! -bar -nargs=1 EspAdc call esp#hw#Adc(<f-args>)
command! -bar EspWifiScan call esp#WifiScan()
command! -nargs=+ EspWifiConnect call esp#WifiConnect(<f-args>)
command! -bar EspWifiDisconnect call esp_wifi_disconnect() | echo 'WiFi disconnected and forgotten'
command! -bar EspWifiStatus call esp#Net()
