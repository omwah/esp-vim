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
command! -bar -bang -nargs=* -complete=customlist,esp#SleepComplete EspSleep call esp#Sleep(<bang>0, <f-args>)
command! -bar -nargs=* -complete=customlist,esp#PowerComplete EspPower call esp#Power(<f-args>)
if esp_power().battery
  command! -bar EspBattery call esp#Battery()
endif
command! -bar -nargs=+ -complete=customlist,esp#GpioComplete EspGpio call esp#Gpio(<f-args>)
command! -bar -bang -nargs=* -complete=customlist,esp#NvsComplete EspNvs call esp#Nvs(<bang>0, <f-args>)
command! -bar -nargs=* -complete=dir EspFiles call espfiles#Open(<f-args>)
command! -bar EspNet call esp#Net()
command! -bar -nargs=* -complete=customlist,esp#TimeComplete EspTime call esp#Time(<f-args>)
command! -bar -bang -nargs=+ EspGet call esp#Get(<bang>0, <f-args>)
command! -bar -bang EspSshKeygen call esp#ssh#Keygen(<bang>0)
command! -bar -nargs=? -complete=dir EspGitInit call esp#git#Init(<f-args>)
command! -bar EspGitStatus call esp#git#Status()
command! -bar -nargs=* -complete=file EspGitAdd call esp#git#Add(<f-args>)
command! -bar -nargs=* -complete=file EspGitReset call esp#git#Reset(<f-args>)
command! -bar -bang -nargs=* -complete=file EspGitRestore call esp#git#Restore(<bang>0, <f-args>)
command! -bang -nargs=* EspGitCommit call esp#git#Commit(<bang>0, <q-args>)
command! -bar -nargs=? EspGitLog call esp#git#Log(<f-args>)
command! -bar -nargs=? -complete=customlist,esp#git#BranchComplete EspGitShow call esp#git#Show(<f-args>)
command! -bar -nargs=* -complete=file EspGitDiff call esp#git#Diff(<f-args>)
command! -bar -nargs=* -complete=customlist,esp#git#BranchComplete EspGitBranch call esp#git#Branch(<f-args>)
command! -bar -nargs=1 -complete=customlist,esp#git#BranchComplete EspGitCheckout call esp#git#Checkout(<f-args>)
command! -bar -nargs=+ -complete=dir EspGitClone call esp#git#Clone(<f-args>)
command! -bar -nargs=? EspGitFetch call esp#git#Fetch(<f-args>)
command! -bar -nargs=? EspGitPull call esp#git#Pull(<f-args>)
command! -bar -nargs=* -complete=customlist,esp#git#BranchComplete EspGitPush call esp#git#Push(<f-args>)
command! -bar -nargs=* EspGitRemote call esp#git#Remote(<f-args>)
command! -bar -nargs=+ EspGitConfig call esp#git#Config(<f-args>)
command! -bar -bang -nargs=? EspGitCredential call esp#git#Credential(<bang>0, <f-args>)
command! -bar EspGitGc call esp#git#Gc()
command! -nargs=+ EspPy call esp#py#Py(<q-args>)
command! -bar -range=% -nargs=? -complete=file EspPyRun call esp#py#Run(<line1>, <line2>, <range>, <q-args>)
command! -bar EspPyReset call esp#py#Reset()
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
command! -nargs=* EspWifiConnect call esp#WifiConnect(<f-args>)
command! -bar EspWifiDisconnect call esp_wifi_disconnect() | echo 'WiFi disconnected; still saved, and joined again at the next start'
command! -bar EspWifiForget call esp_wifi_forget() | echo 'WiFi network and password forgotten'
command! -bar EspWifiStatus call esp#Net()
command! -bar -nargs=? EspBleScan call esp#BleScan(<f-args>)
command! -bar -nargs=? -complete=customlist,esp#ConsoleComplete EspConsole call esp#Console(<f-args>)
command! -bar -nargs=? -complete=customlist,esp#FontComplete EspFont call esp#Font(<f-args>)
command! -bar -nargs=? -complete=customlist,esp#ConsoleComplete EspFlip call esp#Flip(<f-args>)
command! -nargs=* -complete=customlist,esp#BtKeyboardComplete EspBtKeyboard call esp#BtKeyboard(<f-args>)
