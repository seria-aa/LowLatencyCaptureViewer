## v1.2.10

### 한국어

- 오디오 탭에 **콘솔 LPCM 5.1(실험적)** 옵션을 추가했습니다. **WASAPI Shared 전용**이며 기본값은 꺼짐입니다.
- 호환되는 48kHz 6·8채널 PCM 입력을 5.1로 재생합니다. 센터·LFE를 유지하고, 8채널 입력의 뒤·옆 서라운드는 합쳐서 출력합니다.
- 5.1에서도 음량·음소거·클록 드리프트 보정을 사용할 수 있습니다. 지원하지 않는 채널 배치나 실행 중 입력 형식 변경은 안전하게 중단합니다.
- 기존 기본 스테레오 설정과 ASIO·WASAPI Exclusive의 스테레오 지원은 유지합니다. 새 코덱이나 외부 런타임은 추가하지 않았습니다.

> 콘솔은 **5.1 LPCM**, Windows 출력 장치는 **5.1 스피커 구성**으로 설정하세요.
> 캡처보드가 PC에 다채널 PCM을 제공해야 하며, HDMI 패스스루 지원만으로는 충분하지 않습니다.
> 스테레오 출력 장치에서는 다운믹스됩니다. Dolby/DTS 비트스트림은 지원하지 않습니다.
> 자동 검증과 실제 장비 검증은 다릅니다. 물리 5.1 스피커의 채널별 청취 검증은 아직 완료되지 않아 실험 기능으로 제공합니다.

### English

- Added opt-in **Console LPCM 5.1 (experimental)** in the Audio tab. It is **WASAPI Shared only** and disabled by default.
- Plays compatible 48 kHz six/eight-channel PCM inputs as 5.1, preserving center/LFE and folding back/side surrounds from eight-channel inputs.
- Supports volume, mute and clock-drift correction across all six channels. Unsupported layouts and runtime input-format changes are rejected safely.
- Retains the default stereo configuration and existing ASIO/WASAPI Exclusive stereo support. No new codec or external runtime is bundled.

> Set the console to **5.1 LPCM** and configure the Windows playback device for **5.1 speakers**.
> The capture card must expose multichannel PCM to the PC; HDMI passthrough support alone is insufficient.
> Stereo playback devices downmix. Dolby/DTS bitstreams are not supported.
> Automated checks do not replace hardware validation. Physical speaker-by-speaker playback has not yet been verified, so this feature remains experimental.
