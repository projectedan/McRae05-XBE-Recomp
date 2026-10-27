import sys
import numpy as np
from scipy.signal import butter, sosfiltfilt
a=np.memmap(sys.argv[1],dtype=np.int16,mode='r',offset=44).reshape(-1,16)
s=a[35*48000:178*48000].astype(np.float32)/32768
sos=butter(4,[800,2500],btype='band',fs=48000,output='sos')
def env(x):
    y=sosfiltfilt(sos,x); n=len(y)//2400
    return (y[:n*2400]**2).reshape(n,2400).mean(1)
names={'FLpost':0,'Cpost':2,'LFE':3,'BLpost':4,'Cin':8,'b6':9,'b7':10,'b8':11,'b9':12,'rev10':13,'L':14}
E={k:env(s[:,i]) for k,i in names.items()}
db=lambda x:10*np.log10(x+1e-14)
c=db(E['Cin']); from scipy.ndimage import median_filter
base=median_filter(c,61); speech=(c-base)>8; bed=(c-base)<1
print('speech frac %.2f'%speech.mean())
print('%-7s %8s %8s'%('chan','bed dB','speech dB'))
for k in names: print('%-7s %8.1f %8.1f'%(k, db(E[k][bed].mean()), db(E[k][speech].mean())))
