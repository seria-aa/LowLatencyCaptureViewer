## v1.2.5.1 Pre-release

### 한국어

- F11 보더리스 전체화면 전환 중 동기적으로 발생하는 여러 창 크기 변경 알림을
  하나의 D3D11 출력 갱신으로 합쳤습니다.
- 창 스타일과 크기 변경이 끝나기 전에 렌더러가 재구성되는 경합을 방지해, 일부
  환경에서 F11 직후 `0x80070005` 오류와 함께 뷰어가 종료될 가능성을 줄였습니다.
- F5 Pixel-perfect 1:1 복원도 같은 직렬화된 출력 전환 경로를 사용합니다.
- DirectShow 캡처 그래프와 오디오 출력은 전환 중에도 유지되며, 새 프레임 큐나
  오디오 큐는 추가하지 않았습니다.

> 이 버전은 F11 전체화면 전환 수정의 실제 GPU·드라이버별 동작을 확인하기 위한
> 프리릴리스입니다.

### English

- Coalesced the synchronous window-size notifications generated during an F11
  borderless-fullscreen transition into one D3D11 output update.
- Prevented the renderer from being rebuilt before the window style and size
  transition has finished, reducing the chance of an immediate `0x80070005`
  failure on affected systems.
- F5 Pixel-perfect 1:1 restoration now uses the same serialized output
  transition path.
- The DirectShow capture graph and audio output remain active during the
  transition, with no new frame or audio queue.

> This prerelease is intended to validate the F11 fullscreen-transition fix
> across different GPU and driver combinations.
