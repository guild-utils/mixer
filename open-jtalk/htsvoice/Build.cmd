csc.exe htsvconv.cs
htsvconv.exe "Voice TYPE-��\Voice"
move Voice.htsvoice VoiceAlpha.htsvoice
htsvconv.exe "Voice TYPE-��\Voice"
move Voice.htsvoice VoiceBeta.htsvoice

htsvconv.exe "Gamma"
move Gamma.htsvoice VoiceGamma.htsvoice

htsvconv.exe "Voice TYPE-�� ver1"
move "Voice TYPE-�� ver1.htsvoice" VoiceDelta.htsvoice
