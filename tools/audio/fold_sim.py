import sys
import numpy as np
from scipy.signal import butter, sosfiltfilt, sosfilt
from scipy.ndimage import median_filter
a=np.memmap(sys.argv[1],dtype=np.int16,mode='r',offset=44).reshape(-1,16)
s=a[35*48000:178*48000].astype(np.float32)/32768
FL,FR,C,LFE,BL,BR=[s[:,i] for i in range(6)]
lp=butter(2,150,btype='low',fs=48000,output='sos')
LFElp=sosfilt(lp,LFE)
def envd(x,lo,hi):
    sos=butter(4,[lo,hi],btype='band',fs=48000,output='sos'); y=sosfiltfilt(sos,x); n=len(y)//2400
    return 10*np.log10((y[:n*2400]**2).reshape(n,2400).mean(1)+1e-12)
def pop(x):
    r=[]
    for lo,hi in [(300,1000),(1000,2500),(2500,5000)]:
        e=envd(x,lo,hi); d=e-median_filter(e,60); r+= [np.percentile(d,97),np.percentile(d,99)]
    return r
mixes={
 'A now (C 1.41, LFE .316)': FL+1.41*C+0.316*LFE+0.707*BL,
 'B C .707, LFE .316':        FL+0.707*C+0.316*LFE+0.707*BL,
 'C C .707, LFE lp150 x1':    FL+0.707*C+1.0*LFElp+0.707*BL,
 'D C 1.0, LFE .2':           FL+1.0*C+0.2*LFE+0.707*BL,
 'E C .707, no LFE':          FL+0.707*C+0.707*BL,
}
print('ref                          300-1k 9.8 11.5 | 1-2.5k 13.8 15.8 | 2.5-5k 11.5 13.7')
for k,x in mixes.items():
    r=pop(x); print('%-28s %5.1f %5.1f | %5.1f %5.1f | %5.1f %5.1f  rms %.1f'%((k,)+tuple(r)+(20*np.log10(np.sqrt((x**2).mean())),)))
