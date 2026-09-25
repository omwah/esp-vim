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
