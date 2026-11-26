# Live debug channel for SlumberToggle in Eden (no GDB, the game keeps running). Needs v0.64-spawntest14+.
# usage: python3 tools/dbg.py "peek cam+315e4 4" ["pokef cam+30920 2.5" ...]   (one command per argument)
#        python3 tools/dbg.py -f commands.txt
# Writes slumbertoggle/cmd.txt on Eden's SD card, waits for the module to answer (it polls 4x a second)
# and prints slumbertoggle/dbg_out.txt. Set EDEN_SDMC to override the SD card folder; on a real Switch set
# SWITCH_FTP=<ip>:<port> (sys-ftpd-light) to go over FTP instead (login from ~/.netrc or SWITCH_FTP_USER/PASS). DBG_WAIT=<s> waits longer (find).
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

WAIT = float(os.environ.get('DBG_WAIT', '30'))   # "find" over all memory can take minutes

# On a Switch: SWITCH_FTP=<ip>:<port> (e.g. sys-ftpd-light, port 5000), optional SWITCH_FTP_USER /
# SWITCH_FTP_PASS. cmd.txt / dbg_out.txt then go through sd:/slumbertoggle/ over FTP.
if os.environ.get('SWITCH_FTP'):
    import ftplib, io
    host, _, port = os.environ['SWITCH_FTP'].partition(':')
    def connect():
        ftp = ftplib.FTP()
        ftp.connect(host, int(port or 5000), timeout=15)
        user, pw = os.environ.get('SWITCH_FTP_USER'), os.environ.get('SWITCH_FTP_PASS', '')
        if not user:   # else ~/.netrc ("machine <ip> login <user> password <pass>")
            try:
                import netrc
                auth = netrc.netrc().authenticators(host)
                if auth: user, pw = auth[0], auth[2]
            except (FileNotFoundError, netrc.NetrcParseError):
                pass
        try:
            if user: ftp.login(user, pw)
            else: ftp.login()   # anonymous
        except ftplib.error_perm:
            if not user: raise
            ftp.close(); ftp = ftplib.FTP(); ftp.connect(host, int(port or 5000), timeout=15)
            ftp.login()         # the server allows anonymous logins
        return ftp
    def fetch(ftp, name):
        buf = io.BytesIO()
        try: ftp.retrbinary('RETR /slumbertoggle/' + name, buf.write)
        except ftplib.error_perm: return None
        return buf.getvalue().decode('utf-8', 'replace')
    ftp = connect()
    # a unique first line, so two identical answers in a row still count as new (FTP gives no mtime)
    nonce = 'addr %#x' % (int(time.time() * 1000) & 0xffffffffff)
    lines = [nonce] + lines
    # written under another name and renamed: the module polls 4x a second and could read a half-written file
    ftp.storbinary('STOR /slumbertoggle/cmd.tmp', io.BytesIO(('\n'.join(lines) + '\n').encode()))
    ftp.rename('/slumbertoggle/cmd.tmp', '/slumbertoggle/cmd.txt')
    deadline = time.time() + WAIT
    while time.time() < deadline:
        time.sleep(1.0)
        try:
            names = ftp.nlst('/slumbertoggle')
        except ftplib.all_errors:
            ftp = connect(); continue
        if any(n.rstrip('/').endswith('cmd.txt') for n in names): continue
        out = fetch(ftp, 'dbg_out.txt')
        if out is not None and out.startswith('> ' + nonce) and out.rstrip().endswith('# done'):
            print(out.split('\n', 2)[2], end='')   # without the nonce lines
            sys.exit(0)
    try: ftp.delete('/slumbertoggle/cmd.txt')
    except ftplib.all_errors: pass
    sys.exit('no answer in %d s over FTP (game not running, or cmd.txt still waiting)' % WAIT)

before = os.path.getmtime(out_path) if os.path.exists(out_path) else 0
with open(cmd_path, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')
deadline = time.time() + WAIT
while time.time() < deadline:
    time.sleep(0.1)
    if not os.path.exists(cmd_path) and os.path.exists(out_path) and os.path.getmtime(out_path) != before:
        time.sleep(0.05)
        print(open(out_path, encoding='utf-8', errors='replace').read(), end='')
        sys.exit(0)
if os.path.exists(cmd_path):
    os.remove(cmd_path)
sys.exit('no answer in %d s' % WAIT + ' (game not running, or the module is older than spawntest14) - looked in ' + sd)
