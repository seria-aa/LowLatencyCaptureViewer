## v1.1.2.1 Beta

### 한국어

- **오디오 only 모드**를 추가했습니다. 영상 핀과 D3D11 영상 표시를 열지 않고
  캡처 오디오와 WASAPI 출력만 실행합니다.
- 오디오 only 창에도 영상 뷰어와 같은 L/R 피크, dBFS, 채널·마스터 음량,
  클리핑 OSD를 표시합니다.
- 오디오 only OSD를 더블 버퍼링하고 30Hz로 갱신해 깜빡임과 느린 반응을
  줄였습니다. 이 변경은 일반 영상 모드의 캡처·표시 경로에 영향을 주지 않습니다.
- 16/24/32비트 PCM과 32비트 float 캡처 오디오 입력을 인식하며, 필요한 경우
  내부 16비트 스테레오 경로로 변환합니다.
- 진단 로그를 모듈화하고 파일 크기 제한·순환 보관을 적용했습니다.

### English

- Added **Audio-only mode**. It opens only the capture-audio and WASAPI paths;
  no video pin or D3D11 video presentation is created.
- Audio-only mode now shows the same L/R peak, dBFS, channel/master-volume,
  and clipping OSD as the video viewer.
- The audio-only OSD now uses double buffering and a 30 Hz refresh to reduce
  flicker and sluggish feedback. The normal video capture and presentation path
  is unchanged.
- Capture audio negotiation recognizes 16/24/32-bit PCM and 32-bit float input,
  converting to the internal 16-bit stereo path when required.
- Diagnostics logging is modularized with bounded, rotating log files.

This is a beta release. The tested low-latency video path remains uncompressed
NV12/YUY2; compressed capture support remains experimental and device-dependent.
