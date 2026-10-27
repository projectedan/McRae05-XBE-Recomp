import sys
import numpy as np, wave
from scipy.signal import butter, sosfiltfilt
from scipy.ndimage import median_filter
def env_db(x, lo, hi):
    sos=butter(4,[lo,hi],btype='band',fs=48000,output='sos'); y=sosfiltfilt(sos,x)
    n=len(y)//2400; e=(y[:n*2400]**2).reshape(n,2400).mean(1)  # 50 ms
    return 10*np.log10(e+1e-12)
def pop(x, name):
    for lo,hi in [(300,1000),(1000,2500),(2500,5000),(5000,9000)]:
        e=env_db(x,lo,hi); base=median_filter(e,size=60)  # 3 s median
        d=e-base
        print(name, '%5d-%5d'%(lo,hi), 'p90 %.1f p97 %.1f p99 %.1f'%tuple(np.percentile(d,[90,97,99])))
w=wave.open(sys.argv[2]); r=np.frombuffer(w.readframes(w.getnframes()),np.int16).reshape(-1,2).astype(np.float32)/32768
pop(r[10*48000:160*48000].mean(1),'ref ')
a=np.memmap(sys.argv[1],dtype=np.int16,mode='r',offset=44).reshape(-1,16)
pop(a[35*48000:175*48000,14:16].astype(np.float32).mean(1)/32768,'ours')
