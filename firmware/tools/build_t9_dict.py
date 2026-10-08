#!/usr/bin/env python3
"""Build the T9 dictionary for the handset.

The system word list (/usr/share/dict/american-english) has 104k words and NO
frequency data, and a T9 without frequency ordering is miserable to use: typing
4663 should offer "good" long before "hood" or "gone". So the ranking is built
in two tiers:

  tier 1  a curated list of the words people actually send, in rough frequency
          order - the top few hundred cover most of real messaging traffic
  tier 2  everything else from the system list, ranked by length (short words
          are both commoner and cheaper to type past)

Output format - records sorted by DIGIT KEY, then rank, then alphabetically:

    magic  "T9D1"        4 bytes
    count  uint32 LE
    record [len:u8][rank:u8][letters: len bytes, a-z]   x count

🔑 The digit key is NOT stored. It is derivable from the word, so storing it
would waste a byte per record to say something the letters already say. The
firmware computes keys as it walks the file.

Because records are sorted by key, every word matching a typed prefix is a
CONTIGUOUS RANGE - so lookup is two binary searches, not a scan.

🚨 FILTERED FOR A CHILD'S DEVICE. The system word list carries swear words and
slurs, and T9 offers EVERY word that shares a key sequence - so a child typing
an ordinary word could be shown one as a candidate. Measured before this filter:
22 of 26 probe words were in the dictionary. Removed here:
  t9_blocklist.txt        LDNOOBW (CC BY 4.0), single-word entries
  t9_blocklist_extra.txt  words that list misses
plus simple inflections of both (-s, -es, -ed, -ing, -er, -ers, -y). Words a
child legitimately sends ("butt", "sucks", "xx") are kept via t9_allowlist.txt.

Names are deliberately NOT in this file. The handset keeps its own personal
word list for those (spell mode), so a family's names never go into a
published dictionary.
"""
import os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))

def wordfile(name):
    words = set()
    for line in open(os.path.join(HERE, name), encoding='utf-8'):
        w = line.split('#', 1)[0].strip().lower()
        if w and ' ' not in w:
            words.add(w)
    return words

BLOCK = wordfile('t9_blocklist.txt') | wordfile('t9_blocklist_extra.txt')
ALLOW = wordfile('t9_allowlist.txt')
SUFFIXES = ('s', 'es', 'ed', 'ing', 'er', 'ers', 'y')

def blocked(w):
    if w in ALLOW:
        return False
    if w in BLOCK:
        return True
    for suf in SUFFIXES:
        if w.endswith(suf) and w[:-len(suf)] in ALLOW:
            return False      # "sucking", "escorted": the base word is allowed
        if w.endswith(suf) and w[:-len(suf)] in BLOCK:
            return True
        # doubled consonant: "fagged" -> "fag" + g + ed
        if w.endswith(suf) and len(w) > len(suf) + 1 and \
           w[-len(suf)-1] == w[-len(suf)-2] and w[:-len(suf)-1] in BLOCK:
            return True
    return False

T9 = {}
for digit, letters in {
    '2': 'abc', '3': 'def', '4': 'ghi', '5': 'jkl',
    '6': 'mno', '7': 'pqrs', '8': 'tuv', '9': 'wxyz',
}.items():
    for ch in letters:
        T9[ch] = digit

def key(word):
    return ''.join(T9[c] for c in word)

# --- tier 1: what actually gets sent -------------------------------------
# Rough frequency order. The messaging block at the end matters more than its
# position suggests - these are the words this device exists to send.
COMMON = """
the be to of and a in that have i it for not on with he as you do at this but
his by from they we say her she or an will my one all would there their what so
up out if about who get which go me when make can like time no just him know
take people into year your good some could them see other than then now look
only come its over think also back after use two how our work first well way
even new want because any these give day most us is are was were been has had
did does said made come went

yes no ok okay hi hey hello bye please thanks thank sorry love miss
mum mom mummy dad daddy nan nana grandad granny home house school class teacher
dinner tea lunch breakfast food hungry thirsty drink water snack
bed tired sleep sleepy wake awake early late
now soon later today tonight tomorrow morning afternoon evening night minute
minutes hour hours wait waiting ready coming going gone here there where
outside inside upstairs downstairs garden park street road shop shops town
bike walk walking run running car bus lift ride
phone call ring text message answer
play playing game games tv film movie book books music song
friend friends brother sister baby dog cat
help stuck lost hurt fine safe fun cold hot rain raining sunny
finished done start started stop stopped forgot forgotten remember
can't don't won't didn't i'm it's that's what's let's

cook cooking cool look looking looks book need needs keep keeping feel feels
week weeks weekend door room warm dark light open close closed left right
next last first best better worse worst more less lots little big small long
short old new nice bad great fine bored boring boring busy free ready almost
never always sometimes maybe probably really very much many few both each
every another same different another every anything something nothing everything
anyone someone everyone nobody somewhere anywhere everywhere
mine yours ours theirs myself yourself
where when why how what which who whose
bring bringing take taking give giving get getting put putting tell telling
ask asking let letting keep leave leaving send sending meet meeting show
buy buying pay paying find finding lose lost win won try trying
happy sad angry upset scared excited worried okay alright
mobile charger charge battery wifi internet online
homework reading writing maths science history
pizza chips pasta rice bread milk juice apple banana chocolate biscuit
""".split()

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/dict/american-english'
    out = sys.argv[2] if len(sys.argv) > 2 else 't9.dic'
    MAXLEN = 10          # 10 letters covers messaging; 11-12 is mostly ballast

    curated = []
    seen = set()
    for w in COMMON:
        w = ''.join(c for c in w.lower() if c.isalpha())   # strip apostrophes
        if w and w not in seen and len(w) <= MAXLEN:
            seen.add(w)
            curated.append(w)

    words = {}
    for i, w in enumerate(curated):
        # Fine-grained at the top where it matters, coarser down the tail.
        words[w] = min(99, i // 6)

    dropped = 0
    for w in list(words):
        if blocked(w):
            del words[w]; dropped += 1

    kept = 0
    for line in open(src, encoding='utf-8', errors='ignore'):
        w = line.strip()
        if "'" in w or not w.islower():          # drops possessives + proper nouns
            continue
        if not re.fullmatch(r'[a-z]{1,%d}' % MAXLEN, w):
            continue
        if blocked(w):
            dropped += 1
            continue
        kept += 1
        if w in words:
            continue
        # Tier 2: length-ranked, always behind every curated word.
        words[w] = 100 + min(len(w), 20)

    recs = sorted(words.items(), key=lambda kv: (key(kv[0]), kv[1], kv[0]))

    with open(out, 'wb') as f:
        f.write(b'T9D1')
        f.write(struct.pack('<I', len(recs)))
        for w, rank in recs:
            f.write(struct.pack('BB', len(w), rank))
            f.write(w.encode('ascii'))

    size = 8 + sum(2 + len(w) for w, _ in recs)
    print(f"source words kept   {kept}")
    print(f"blocked             {dropped}")
    print(f"curated (tier 1)    {len(curated)}")
    print(f"total records       {len(recs)}")
    print(f"file size           {size} bytes ({size/1024:.0f} KB) -> {out}")
    print(f"t9dic partition     1048576 bytes ({100*size/1048576:.0f}% used)")

    for probe in ('4663', '46', '843', '7669'):
        hits = [w for w, _ in recs if key(w).startswith(probe)][:6]
        exact = [w for w, r in sorted(
            [(w, r) for w, r in recs if key(w) == probe], key=lambda kv: kv[1])][:5]
        print(f"  {probe}: exact={exact}  prefix={hits[:4]}")

if __name__ == '__main__':
    main()
