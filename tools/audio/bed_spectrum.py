import sys
import numpy as np, wave
from scipy.signal import stft
from scipy.ndimage import median_filter
def spec(x):
    f,t,Z=stft(x,48000,nperseg=4800,noverlap=2400)  # 50 ms hop
    return f,np.abs(Z)**2
cent=[63,125,250,500,1000,1500,2000,3000,4000,6000,8000]
def bandpow(f,P):
    out=[]
    for fc in cent:
        m=(f>=fc/1.26)&(f<fc*1.26); out.append(P[m].sum(0))
    return np.array(out)
def analyse(x):
    f,P=spec(x); B=bandpow(f,P)
    sp=10*np.log10(B[4:7].sum(0)+1e-12); base=median_filter(sp,61)
    bed=(sp-base)<1.0; speech=(sp-base)>6
    bedspec=10*np.log10(B[:,bed].mean(1)); spspec=10*np.log10(B[:,speech].mean(1))
    tot=10*np.log10(B[:,bed].mean(1).sum())
    return bedspec-tot, spspec-tot, speech.mean()
w=wave.open(sys.argv[2]); r=np.frombuffer(w.readframes(w.getnframes()),np.int16).reshape(-1,2).astype(np.float32)/32768
a=np.memmap(sys.argv[1],dtype=np.int16,mode='r',offset=44).reshape(-1,16)
rb,rs,rf=analyse(r[10*48000:160*48000].mean(1))
ob,os_,of=analyse(a[35*48000:175*48000,14:16].astype(np.float32).mean(1)/32768)
print('speech frames ref %.2f ours %.2f'%(rf,of))
print('band   ref-bed ours-bed  diff | ref speech-bed  ours speech-bed')
for i,c in enumerate(cent):
    print('%5d %7.1f %7.1f %6.1f | %6.1f %6.1f'%(c,rb[i],ob[i],ob[i]-rb[i],rs[i]-rb[i],os_[i]-ob[i]))
