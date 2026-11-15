#!/usr/bin/env python3
"""Bake a single lab section into a standalone file.

Chrome's headless --screenshot path was observed to ignore the URL fragment
(the JS #only handler never ran), so select the section in CSS instead.
usage: mklab.py <section-id> <out.html> [pad]
"""
import sys
sec, out = sys.argv[1], sys.argv[2]
pad = (len(sys.argv) > 3 and sys.argv[3] == 'pad0')
s = open('variants.html').read()
css = '.grp{display:none}#%s{display:block}h1,h2,p{display:none}' % sec
if pad:
    css += 'body{padding:0}'
assert '</head>' in s
s = s.replace('</head>', '<style>%s</style></head>' % css, 1)
open(out, 'w').write(s)
print('baked', out, 'section', sec, 'pad0' if pad else '')
