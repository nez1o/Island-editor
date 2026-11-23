# Live debug channel for SlumberToggle in Eden (no GDB, the game keeps running). Needs v0.64-spawntest14+.
# usage: python3 tools/dbg.py "peek cam+315e4 4" ["pokef cam+30920 2.5" ...]   (one command per argument)
#        python3 tools/dbg.py -f commands.txt
# Writes slumbertoggle/cmd.txt on Eden's SD card, waits for the module to answer (it polls 4x a second)
# and prints slumbertoggle/dbg_out.txt. Set EDEN_SDMC to override the SD card folder.
#
# Commands (see "live debug channel" in source/program/main.cpp):
#   peek <addr> [words]      hex + float per 32-bit word
#   poke <addr> <hex32>...   pokef <addr> <float>...
#   hold <addr> <float>      written every island-editor frame (8 max); unhold clears them
#   snap <addr> <bytes>      remember a block;  diff  lists the words that changed since
#   find <hex bytes> [max]   search every readable+writable memory region
#   addr <addr>              print an address (and main+offset)
# Addresses: main, mod (this module), cam (edit camera), player, layout (editor), recs (building records),
# hex numbers, + and -, [expr] = the u64 stored at expr. Bad addresses answer "! cannot read".
import os, sys, time

def sd_dir():
    if os.environ.get('EDEN_SDMC'):
        return os.path.join(os.environ['EDEN_SDMC'], 'slumbertoggle')
    if os.name == 'nt':
        return os.path.join(os.environ['APPDATA'], 'eden', 'sdmc', 'slumbertoggle')
    data = os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share')
    return os.path.join(data, 'eden', 'sdmc', 'slumbertoggle')

sd = sd_dir()
cmd_path, out_path = os.path.join(sd, 'cmd.txt'), os.path.join(sd, 'dbg_out.txt')

if len(sys.argv) > 2 and sys.argv[1] == '-f':
    lines = open(sys.argv[2], encoding='utf-8').read().splitlines()
else:
    lines = sys.argv[1:]
if not lines:
    sys.exit('usage: dbg.py "<command>" ...  (see the top of this file)')

before = os.path.getmtime(out_path) if os.path.exists(out_path) else 0
with open(cmd_path, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')
deadline = time.time() + 30   # "find" over all memory can take a while
while time.time() < deadline:
    time.sleep(0.1)
    if not os.path.exists(cmd_path) and os.path.exists(out_path) and os.path.getmtime(out_path) != before:
        time.sleep(0.05)
        print(open(out_path, encoding='utf-8', errors='replace').read(), end='')
        sys.exit(0)
if os.path.exists(cmd_path):
    os.remove(cmd_path)
sys.exit('no answer in 30 s (game not running, or the module is older than spawntest14) - looked in ' + sd)
