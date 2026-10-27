# Audio analysis

For `record.bat` recordings (`audio_rec.wav`, 16 channels: FL FR C LFE BL BR after the effects image; sends 13, 23,
centre, 3D 6-9, reverb 10 as voices fill them; stereo L R) against a reference stereo WAV of the game (48 kHz).
Python 3 with numpy and scipy. The time windows inside each script are for the 2026-10-27 recordings (driving from
about 35 s); change them for a new recording.

- `speech_pop.py rec.wav ref.wav`: how far events (the co-driver's calls) rise above the bed per band, percentiles.
  Reference CMR clip: 1-2.5 kHz p97 13.8 / p99 15.8 dB. Ours before 01bdb25: 7.9 / 9.8.
- `bed_spectrum.py rec.wav ref.wav`: the bed's (no speech) band spectrum against the reference's.
- `send_levels.py rec.wav`: each channel and send's level in 0.8-2.5 kHz, bed and speech frames.
- `fold_sim.py rec.wav`: stereo folds simulated from the recorded speaker channels, and their speech pop.
