import sys
FPS=59.7275
names={'aw':(30,600),'emerald':(30,1900),'fe':(1700,2400),'gs':(1700,2400),'mc':(1700,2400)}
SN=['off','right','left','both']
for s,(skip,fr) in names.items():
    L=[l.split() for l in open('%s/sst_%s.txt'%(sys.argv[1],s))]
    a=[int(x) for x in L[0][2:] if x.lstrip('-').isdigit()]  # numeric only
    b=[int(x) for x in L[1][2:] if x.lstrip('-').isdigit()]
    d=[y-x for x,y in zip(a,b)]
    n=fr-skip
    # layout: tcalls A B | out A B | nz A B | status A x4 | status B x4 | step minA maxA minB maxB | rcalls rticks | ch x4 | on x4 | routed x4
    t=d[0:2]; out=d[2:4]; nz=d[4:6]; stA=d[6:10]; stB=d[10:14]
    smin=b[14:18]  # cumulative min/max (over the whole run, includes boot)
    rc,rt=d[18],d[19]; ch=d[20:24]; on=d[24:28]; rr=d[28:32]; au=d[32:36]
    print('== %s: %d frames timed'%(s,n))
    for i,(nm,st) in enumerate((('A',stA),('B',stB))):
        calls=t[i]
        if calls==0: print('  FIFO %s: no timer overflows'%nm); continue
        print('  FIFO %s: %.1f sound_timer calls/frame = %.0f Hz; %.2f output frames/call (%.0f/frame); status %s; nonzero sample %.0f%%; step range over whole run %.0f..%.0f Hz'
              %(nm,calls/float(n),calls/float(n)*FPS,out[i]/float(calls),out[i]/float(n),
                ', '.join('%s %.0f%%'%(SN[k],100.0*st[k]/calls) for k in range(4) if st[k]),100.0*nz[i]/calls,
                smin[2*i]/256.0 if smin[2*i]!=0xFFFFFFFF else 0, smin[2*i+1]/256.0))
    print('  render_gbc_sound: %.2f calls/frame, %.1f output frames per frame total (%.1f per call)'%(rc/float(n),rt/float(n),rt/float(max(rc,1))))
    for c in range(4):
        print('  tone ch%d: on in %.0f%% of renders, routed&rendering %.0f%% of output frames, with volume > 0 in %.0f%% of renders'%(c+1,100.0*on[c]/max(rc,1),100.0*ch[c]/max(rt,1),100.0*au[c]/max(rc,1)))
