# 커널 정상 종료(clean shutdown/reboot) 경로 부재 - 설계 필요

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-F367AD5D
  status: approved
  updatedAt: 2026-09-23T08:45:47.135Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# 커널 정상 종료(clean shutdown/reboot) 경로 부재 - 설계 필요

`PN-547EF839`(libvfat 볼륨 dirty 비트) 작업 중 발견(`PN-0B461E6F`로
후속 계획 분리)한 구조적 공백 - 이 커널 전체에 "정상 종료(clean
shutdown)"라는 개념 자체가 아직 없다.

## 현재 상태 (조사 완료, 2026-09-23)

- `SP-8B6B8D25`(초기 설계 명세) 전문을 검색했으나 shutdown/reboot/
  poweroff/halt 관련 서술이 전혀 없음(매칭된 4곳 전부 무관한 문맥 -
  Task 강제 종료/Zombie 전환 얘기).
- `RM-48E1E610`(Syscall 할당표)에도 Shutdown/Reboot/PowerOff/Halt류
  syscall이 전혀 예약돼 있지 않음.
- 즉 이 프로젝트 어디에도 "커널을 정상적으로 멈추거나 재시작하는
  방법"이 설계도 구현도 안 돼 있다 - 지금까지 모든 검증은 전부 QEMU
  강제 종료(pkill)/timeout으로만 끝났다.

## 왜 필요한가

- `PN-547EF839`(libvfat dirty 비트)의 "언마운트 시 클리어" 절반이
  이 기능 없이는 구현할 대상 자체가 없음.
- `libext4`(SP-7A9CED3E)의 `s_state`(EXT4_VALID_FS) 관리도 원칙적으로
  같은 문제(정상 언마운트 시점에 상태를 되돌려 쓸 방법이 없음).
- 표준 fsck 관례(정상 종료 여부로 다음 부팅 시 검사 필요성 판단)의
  전제 자체가 이 커널엔 아직 없음.

## 결정이 필요한 것

1. **커널을 멈추는 방법 자체를 새로 설계할지, 아니면 지금 범위 밖으로
   계속 미룰지** - QEMU 실측 검증이 전부 강제 종료로 충분했던 지금
   단계에서 이게 실제로 필요한 타이밍인지(다른 더 급한 설계가 많은
   상황에서의 우선순위 판단 포함).
2. 필요하다면 **트리거 지점** - 새 syscall(예: `Shutdown`/`Reboot`)로
   유저 프로세스(init 등)가 요청하는 구조인지, 아니면 ACPI 전원
   버튼/키 입력 같은 외부 이벤트가 트리거인지, 아니면 둘 다인지.
3. **순서/책임 소재** - 커널이 각 마운트된 볼륨에 순회하며
   `unmount()`류 훅을 부르는 주체가 될지(예: `MountTable::
   shutdownAll()`), 아니면 fs 서비스(devmgr/fs가 흡수된 커널 서비스)
   자신이 스스로의 생명주기로 처리할지.
4. `kernel::FileSystemDriver` 인터페이스에 `unmount()`/`onShutdown()`류
   훅을 추가하는 것 자체는 순수 구현 디테일(설계자 승인 불필요)로
   보이나, 위 1-3이 먼저 정해져야 그 훅이 정확히 "언제/누구에 의해"
   불리는지 알 수 있어 후순위로 미룸.

## [답변, 2026-09-23, QU-7C3AB7A2] 확정 - 새 syscall + ACPI 전원 이벤트 둘 다 구현

설계자 답변: "새 syscall(Shutdown/Reboot)로 유저 프로세스가 요청.
ACPI 전원 이벤트가 트리거. 둘다 구현해" - 위 결정 목록 1번(설계
필요함, 미루지 않음) 확정 + 2번(트리거 지점)이 "둘 다"로 확정.
3번(순서/책임 소재)과 4번(훅 인터페이스)은 여전히 구현 디테일로
착수 세션이 정한다. 착수는 `PN-0B461E6F`에서 이어감.

## 범위 밖

실제 훅 배선/libvfat·libext4 각자의 dirty/state 비트 정리 로직 자체는
이 DC의 답변 이후 `PN-0B461E6F`/`PN-547EF839`가 이어서 구현한다.

