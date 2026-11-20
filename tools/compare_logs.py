#!/usr/bin/env python3
"""compare_logs.py - read the coop5.log of the HOST and of the GUEST and say what each PC sent and what became of it on the other one.

    python tools\\compare_logs.py <host coop5.log> <guest coop5.log> [--since HH:MM:SS] [--until HH:MM:SS]

Why: every "the flag did not arrive" report so far was one of a few things (held by the distance rule, own-entry only, already set, unknown here,
a level change, F7). This tool classifies every flag / trigger key / kill event / checkpoint in both directions from the lines the bridge and the mod
already write, so nobody has to read 2000 lines by hand. The two PCs' clocks differ by about a second, so the matching is by NAME and ORDER, not time;
--since/--until use each file's own clock.

What it reads (coop5.log lines):
  flag sent: NAME                 flag received: NAME            flag applied: NAME
  COOPTRG waits|held|released|recv|flag NAME ...                 (the mod's decisions, coop_debug 1)
  kill event sent #N ... [key K]  kill event received #N ... [key K]
  checkpoint sent|received: NAME (BYTES bytes[, crc32 X])
It never writes anything.
"""
import re
import sys
import collections

LINE = re.compile(r'^\[(?:\d+)\]\s+(\d\d:\d\d:\d\d\.\d+)\s+(.*)$')


def load(path, since, until):
    ev = []
    for raw in open(path, encoding='latin1', errors='replace'):
        m = LINE.match(raw.rstrip('\n'))
        if not m:
            continue
        t, msg = m.group(1), m.group(2)
        if since and t < since:
            continue
        if until and t > until:
            continue
        ev.append((t, msg))
    return ev


def parse(ev):
    d = collections.defaultdict(list)
    for t, msg in ev:
        m = re.match(r'flag sent: (.+)$', msg)
        if m:
            d['sent'].append((t, m.group(1).strip()))
            continue
        m = re.match(r'flag received: (.+)$', msg)
        if m:
            d['recv'].append((t, m.group(1).strip()))
            continue
        m = re.match(r'flag applied: (.+)$', msg)
        if m:
            d['applied'].append((t, m.group(1).strip()))
            continue
        m = re.match(r'script: COOPTRG (waits|held|released|refire) (\S+(?: \S+)*?)(?::| \(|$)', msg)
        if m:
            d[m.group(1)].append((t, m.group(2).strip()))
            continue
        m = re.match(r'script: COOPTRG flag (.+?) (waits|unknown here|dropped|already set here)', msg)
        if m:
            d['flag_' + m.group(2).replace(' ', '_')].append((t, m.group(1).strip()))
            continue
        m = re.match(r'script: COOPTRG recv (\S+(?: \S+)*?) (alreadyfired=.*|unknown here.*)$', msg)
        if m:
            d['recv_note'].append((t, m.group(1).strip() + ' ' + m.group(2)))
            continue
        m = re.match(r'kill event sent #(\d+).*?(?: key (\S+))?$', msg)
        if m:
            d['ksent'].append((t, m.group(1) + (' key ' + m.group(2) if m.group(2) and m.group(2) != '-' else '')))
            continue
        m = re.match(r'kill event received #(\d+).*?(?: key (\S+))?$', msg)
        if m:
            d['krecv'].append((t, m.group(1) + (' key ' + m.group(2) if m.group(2) else '')))
            continue
        m = re.match(r'checkpoint (sent|received): (\S+) \((\d+) bytes(?:, crc32 ([0-9A-Fa-f]+))?\)', msg)
        if m:
            d['cp_' + m.group(1)].append((t, m.group(2), m.group(3), (m.group(4) or '').upper()))
            continue
        if re.match(r'resync: F7 pressed', msg):
            d['f7'].append((t, 'F7'))
        elif re.match(r'dvar watch: sv_running=1 mapname=\'(\w+)\'', msg):
            d['map'].append((t, re.search(r"mapname='(\w+)'", msg).group(1)))
    return d


def direction(name_a, a, name_b, b):
    print('=' * 78)
    print('%s -> %s' % (name_a, name_b))
    print('=' * 78)
    sent = [n for _, n in a['sent']]
    recv = [n for _, n in b['recv']]
    applied = collections.Counter(n for _, n in b['applied'])
    waits = collections.Counter(n for _, n in b['waits'])
    released = collections.Counter(n for _, n in b['released'])
    held = collections.Counter(n for _, n in b['held'])
    fwait = collections.Counter(n for _, n in b['flag_waits'])
    funk = collections.Counter(n for _, n in b['flag_unknown_here'])
    fdrop = collections.Counter(n for _, n in b['flag_dropped'])
    fset = collections.Counter(n for _, n in b['flag_already_set_here'])
    note = collections.defaultdict(list)
    for _, n in b['recv_note']:
        k, _, rest = n.partition(' ')
        note[k].append(rest)
    sent_c = collections.Counter(sent)
    recv_c = collections.Counter(recv)
    print('sent %d (%d distinct), received on the other side %d (%d distinct)' % (len(sent), len(sent_c), len(recv), len(recv_c)))
    lost = sorted(n for n in sent_c if n not in recv_c)
    if lost:
        print('\nSENT BUT NOT RECEIVED (%d)  [the other side was not running a mission, a level change, or the log window]' % len(lost))
        for n in lost:
            print('   ' + n)
    # what became of every received name
    outcome = collections.defaultdict(list)
    for n in recv_c:
        if applied[n]:
            outcome['applied'].append(n)
        elif released[n]:
            outcome['released after waiting (fired by the proximity rule)'].append(n)
        elif held[n]:
            outcome['held: own entry only (never fired from the partner)'].append(n)
        elif fset[n]:
            outcome['already set here'].append(n)
        elif waits[n] or fwait[n]:
            outcome['WAITING (distance rule) when the log window ended or the level changed'].append(n)
        elif funk[n]:
            outcome['unknown here: no such flag in that level'].append(n)
        elif fdrop[n]:
            outcome['dropped after 10 minutes'].append(n)
        elif n in note:
            outcome['note: ' + note[n][0].split()[0]].append(n)
        else:
            outcome['NO TRACE (old build without the diagnostic lines, or the level changed)'].append(n)
    print('\nWHAT BECAME OF THE %d RECEIVED NAMES' % len(recv_c))
    for k in sorted(outcome, key=lambda x: (x.startswith('NO TRACE'), x)):
        names = sorted(outcome[k])
        print('  %-70s %3d' % (k, len(names)))
        if k.startswith(('WAITING', 'NO TRACE', 'unknown', 'dropped')):
            for n in names[:25]:
                print('       ' + n)
    ks = [n for _, n in a['ksent']]
    kr = [n for _, n in b['krecv']]
    if ks or kr:
        keyed_s = [n for n in ks if 'key' in n]
        keyed_r = [n for n in kr if 'key' in n]
        print('\nkill events: sent %d (with a key: %d), received %d (with a key: %d)' % (len(ks), len(keyed_s), len(kr), len(keyed_r)))
    cs = a['cp_sent']
    cr = b['cp_received']
    if cs or cr:
        print('\ncheckpoints: sent %d, received %d' % (len(cs), len(cr)))
        by_name = {}
        for t, nm, sz, crc in cr:
            by_name[(nm, sz)] = crc
        for t, nm, sz, crc in cs:
            got = by_name.get((nm, sz))
            if got is None:
                print('   %-28s %9s bytes  sent, NOT received (same name and size)' % (nm, sz))
            elif crc and got and crc != got:
                print('   %-28s %9s bytes  CRC MISMATCH sent %s received %s' % (nm, sz, crc, got))
    print()


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    since = until = None
    args = argv[3:]
    for i, x in enumerate(args):
        if x == '--since' and i + 1 < len(args):
            since = args[i + 1]
        if x == '--until' and i + 1 < len(args):
            until = args[i + 1]
    host = parse(load(argv[1], since, until))
    guest = parse(load(argv[2], since, until))
    for who, d in (('HOST', host), ('GUEST', guest)):
        maps = [m for _, m in d['map']]
        print('%s: missions %s; F7 pressed %d times; flags sent %d / received %d / applied %d' % (
            who, ', '.join(dict.fromkeys(maps)) or '-', len(d['f7']), len(d['sent']), len(d['recv']), len(d['applied'])))
    print()
    direction('HOST', host, 'GUEST', guest)
    direction('GUEST', guest, 'HOST', host)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
